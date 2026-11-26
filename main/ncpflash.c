#include "ncpflash.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "dongle.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"
#include "ncpflash_core.h"
#include "settings.h"
#include "stats.h"
#include "xflash.h"

static const char *TAG = "dongle_fw";

// The image area: a 4 KB header sector, then the .gbl. The header is written last, so a power cut or a
// failed check mid-upload leaves no image rather than half of one.
#define AREA_LABEL "dongle_fw"
#define AREA_SUBTYPE 0x40
#define AREA_OFFSET 0x820000  // right after ota_1 (partitions.csv); registered by hand on older tables
#define AREA_SIZE 0x200000
#define IMG_OFF 4096
#define HDR_MAGIC 0x5A42474Cu     // "LGBZ": Silicon Labs .gbl
#define HDR_MAGIC_TI 0x5A49544Cu  // "LTIZ": TI Z-Stack .zip/.hex (converted at flash time)
#define HDR_MAGIC_GCF 0x4643474Cu // "LGCF": ConBee .GCF
#define HDR_MAGIC_NRF 0x46524E4Cu // "LNRF": nRF52840 .hex
#define ERASE_STEP (64 * 1024)

typedef struct {
    uint32_t magic, size, tag_len;
    uint8_t md5[16], sha256[32];
    char name[64];
    int64_t epoch;  // upload time (0 if the clock wasn't set)
    uint32_t crc;   // CRC-32 of everything above
} img_hdr_t;

typedef enum { ST_IDLE = 0, ST_RUNNING, ST_DONE, ST_FAILED } fstate_t;

typedef struct {
    fstate_t state;
    char step[16];
    uint32_t done, total;
    char error[200];
    nf_result_t res;
    int64_t started, ended;  // epoch s (0 if unknown)
    uint32_t started_up, ended_up;
} flash_state_t;

static const esp_partition_t *s_part;
static SemaphoreHandle_t s_lock;
static img_hdr_t s_hdr;
static nf_gbl_t s_gbl;
static bool s_have;
static img_kind_t s_kind;   // what the stored image is
static char s_desc[96];     // its one-line summary (non-.gbl kinds)
static flash_state_t s_fs;

static uint32_t magic_for(img_kind_t k) {
    switch (k) {
        case IMG_TI: return HDR_MAGIC_TI;
        case IMG_GCF: return HDR_MAGIC_GCF;
        case IMG_NRF: return HDR_MAGIC_NRF;
        default: return HDR_MAGIC;
    }
}
static img_kind_t kind_for_magic(uint32_t m) {
    switch (m) {
        case HDR_MAGIC: return IMG_GBL;
        case HDR_MAGIC_TI: return IMG_TI;
        case HDR_MAGIC_GCF: return IMG_GCF;
        case HDR_MAGIC_NRF: return IMG_NRF;
        default: return IMG_UNKNOWN;
    }
}

static portMUX_TYPE s_busy_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_busy;

// upload in progress
static struct {
    bool active;
    uint32_t size, got, erased;
    img_kind_t kind;
    char name[64];
    mbedtls_md5_context md5;
    mbedtls_sha256_context sha;
} s_up;

static bool busy_take(void) {
    portENTER_CRITICAL(&s_busy_mux);
    bool ok = !s_busy;
    s_busy = true;
    portEXIT_CRITICAL(&s_busy_mux);
    return ok;
}

static void busy_give(void) {
    portENTER_CRITICAL(&s_busy_mux);
    s_busy = false;
    portEXIT_CRITICAL(&s_busy_mux);
}

bool ncpflash_busy(void) { return s_busy; }

static int64_t epoch_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec > 1700000000 ? tv.tv_sec : 0;
}

static void hex(char *out, const uint8_t *d, size_t n) {
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hx[d[i] >> 4];
        out[i * 2 + 1] = hx[d[i] & 15];
    }
    out[n * 2] = 0;
}

static void fmt_ncp(char *out, size_t n, uint8_t ezsp, uint16_t v) {
    snprintf(out, n, "EZSP v%u, stack %u.%u.%u.%u", ezsp, (v >> 12) & 15, (v >> 8) & 15, (v >> 4) & 15, v & 15);
}

// ---------------------------------------------------------------- io for the core

static int io_image_read(void *ctx, uint32_t off, uint8_t *buf, size_t len) {
    (void)ctx;
    if (s_part == NULL || IMG_OFF + off + len > s_part->size) return -1;
    return esp_partition_read(s_part, IMG_OFF + off, buf, len) == ESP_OK ? 0 : -1;
}

static int io_read(void *ctx, uint8_t *buf, size_t max, uint32_t timeout_ms) {
    (void)ctx;
    if (!dongle_ready()) return -1;
    size_t n = xStreamBufferReceive(dongle_claim_rx(), buf, max, pdMS_TO_TICKS(timeout_ms));
    if (n == 0 && !dongle_ready()) return -1;
    return (int)n;
}

static int io_write(void *ctx, const uint8_t *buf, size_t len) {
    (void)ctx;
    return dongle_claimed_write(buf, len, 1000) == ESP_OK ? 0 : -1;
}

static int io_baud(void *ctx, uint32_t baud) {
    (void)ctx;
    return dongle_set_baud(baud) == ESP_OK ? 0 : -1;
}

static int io_lines(void *ctx, bool dtr, bool rts) {
    (void)ctx;
    return dongle_set_lines(dtr, rts) == ESP_OK ? 0 : -1;
}

static void io_sleep(void *ctx, uint32_t ms) {
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}

static uint32_t io_now(void *ctx) {
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void io_progress(void *ctx, const char *step, uint32_t done, uint32_t total) {
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_fs.step, step, sizeof(s_fs.step));
    s_fs.done = done;
    s_fs.total = total;
    xSemaphoreGive(s_lock);
}

static void io_log(void *ctx, const char *msg) {
    (void)ctx;
    ESP_LOGI(TAG, "%s", msg);
}

static void rts_dtr(void) {  // universal-silabs-flasher's rts_dtr pattern
    dongle_set_lines(false, true);
    vTaskDelay(pdMS_TO_TICKS(100));
    dongle_set_lines(true, false);
    vTaskDelay(pdMS_TO_TICKS(500));
    dongle_set_lines(false, false);
}

// The board's hardware route into its Gecko bootloader, from the dongle list.
static int io_hw_reset(void *ctx, const char **how) {
    (void)ctx;
    dongle_info_t d;
    dongle_info(&d);
    switch (d.model->reset) {
        case RST_NONE:
            if (d.model->family != FAM_UNKNOWN) return -1;  // known board without one (e.g. SkyConnect)
            // unknown adapter: the common DTR/RTS circuit is worth a try
            // fallthrough
        case RST_RTS_DTR:
            io_log(NULL, "Trying the RTS/DTR reset into the bootloader");
            rts_dtr();
            *how = "RTS/DTR reset";
            return 0;
        case RST_SLZB07_GPIO: {
            // universal-silabs-flasher slzb07: CP2102N GPIO4/GPIO5 (1,1) (0,0) (1,0) (1,1), 100 ms apart
            io_log(NULL, "Trying the CP2102N GPIO reset into the bootloader");
            static const uint8_t seq[4][2] = {{1, 1}, {0, 0}, {1, 0}, {1, 1}};
            for (int i = 0; i < 4; i++) {
                uint8_t st = (uint8_t)((seq[i][0] ? 0x10 : 0) | (seq[i][1] ? 0x20 : 0));
                if (dongle_set_gpio(0x30, st) != ESP_OK) return -1;
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            *how = "CP2102N GPIO reset";
            return 0;
        }
        case RST_BAUD_KNOCK: {
            // Connect ZBT-2: DTR/RTS first, then the ESP32-S3 bridge's command mode (150, 300, 1200 baud, "BZ")
            io_log(NULL, "Trying the ESP32-S3 bridge's baud knock into the bootloader");
            rts_dtr();
            static const uint32_t knock[] = {150, 300, 1200};
            for (int i = 0; i < 3; i++) {
                if (dongle_set_baud(knock[i]) != ESP_OK) return -1;
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            static const uint8_t bz[] = {'B', 'Z'};
            dongle_claimed_write(bz, sizeof(bz), 500);
            vTaskDelay(pdMS_TO_TICKS(500));
            *how = "ESP32-S3 baud knock";
            return 0;
        }
        default:
            return -1;
    }
}

static const nf_io_t s_io = {
    .read = io_read,
    .write = io_write,
    .set_baud = io_baud,
    .set_lines = io_lines,
    .sleep_ms = io_sleep,
    .now_ms = io_now,
    .image_read = io_image_read,
    .progress = io_progress,
    .log = io_log,
    .hw_reset = io_hw_reset,
};

// ---------------------------------------------------------------- the stored image

static bool hdr_valid(const img_hdr_t *h) {
    return kind_for_magic(h->magic) != IMG_UNKNOWN && h->size > 0 && h->size <= AREA_SIZE - IMG_OFF && h->tag_len <= h->size &&
           h->crc == nf_crc32(0, (const uint8_t *)h, offsetof(img_hdr_t, crc));
}

// MD5 (and SHA-256) of what's in flash now. NULL or the reason.
static const char *image_hashes(uint32_t size, uint8_t md5[16], uint8_t sha[32]) {
    uint8_t *buf = malloc(4096);
    if (!buf) return "out of memory";
    mbedtls_md5_context m;
    mbedtls_sha256_context s;
    mbedtls_md5_init(&m);
    mbedtls_sha256_init(&s);
    mbedtls_md5_starts(&m);
    mbedtls_sha256_starts(&s, 0);
    const char *fail = NULL;
    for (uint32_t off = 0; off < size && !fail; off += 4096) {
        uint32_t n = size - off < 4096 ? size - off : 4096;
        if (esp_partition_read(s_part, IMG_OFF + off, buf, n) != ESP_OK) fail = "reading the stored image failed";
        else {
            mbedtls_md5_update(&m, buf, n);
            mbedtls_sha256_update(&s, buf, n);
        }
    }
    mbedtls_md5_finish(&m, md5);
    mbedtls_sha256_finish(&s, sha);
    mbedtls_md5_free(&m);
    mbedtls_sha256_free(&s);
    free(buf);
    return fail;
}

// What the metadata says, and whether that's a reason not to flash it.
typedef struct {
    char fw_type[24], ezsp[24], sdk[24], flow[12];
    int baud;
} meta_t;

static void parse_meta(const nf_gbl_t *g, meta_t *m) {
    memset(m, 0, sizeof(*m));
    if (g->metadata[0] == 0 || g->metadata_cut) return;
    cJSON *j = cJSON_Parse(g->metadata);
    if (!j) return;
    const cJSON *v;
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "fw_type"))) strlcpy(m->fw_type, v->valuestring, sizeof(m->fw_type));
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "ezsp_version"))) strlcpy(m->ezsp, v->valuestring, sizeof(m->ezsp));
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "sdk_version"))) strlcpy(m->sdk, v->valuestring, sizeof(m->sdk));
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "baudrate"))) m->baud = v->valueint;
    // metadata v3 (Nerivec): fw_version / fw_variant instead of ezsp_version
    if (!m->ezsp[0] && cJSON_IsString(v = cJSON_GetObjectItem(j, "fw_version"))) strlcpy(m->ezsp, v->valuestring, sizeof(m->ezsp));
    if (cJSON_IsString(v = cJSON_GetObjectItem(j, "fw_variant"))) strlcpy(m->flow, v->valuestring, sizeof(m->flow));
    cJSON_Delete(j);
}

// NULL, or why this image must not go on the coordinator. A different baud is fine: the bridge verifies
// the new firmware at its baud and switches itself over (link_after).
static const char *meta_problem(const meta_t *m, uint32_t baud, char *buf, size_t n) {
    (void)baud;
    bridge_settings_t cfg;
    settings_get(&cfg);
    bool thread = cfg.role == BRIDGE_ROLE_THREAD;
    const char *want = thread ? "openthread_rcp" : "zigbee_ncp";
    if (m->fw_type[0] && strcmp(m->fw_type, want) != 0) {
        if (strcmp(m->fw_type, "openthread_rcp") == 0 && !thread)
            snprintf(buf, n, "This is OpenThread RCP firmware: set the dongle role to Thread (Settings) to use it with a border router");
        else if (strcmp(m->fw_type, "zigbee_ncp") == 0 && thread)
            snprintf(buf, n, "This is Zigbee coordinator firmware: set the dongle role to Zigbee (Settings) to use it with Zigbee2MQTT");
        else
            snprintf(buf, n, "This is %s firmware, not a %s build (%s)", m->fw_type, thread ? "Thread RCP" : "Zigbee coordinator", want);
        return buf;
    }
    if (m->baud > 0 && (m->baud < 9600 || m->baud > 921600)) {
        snprintf(buf, n, "This firmware talks at %d baud, outside what the bridge drives (9600-921600)", m->baud);
        return buf;
    }
    if (strcmp(m->flow, "hw_flow") == 0) {
        dongle_info_t d;
        dongle_info(&d);
        if (d.model->family != FAM_UNKNOWN && !d.model->hw_flow) {
            snprintf(buf, n, "This build needs RTS/CTS hardware flow control, which %s doesn't have wired", d.model->name);
            return buf;
        }
    }
    return NULL;
}

// What the bridge changes about its own link when this image goes on: baud and CTS handshake.
typedef struct {
    uint32_t baud;     // 0 = unchanged
    int hw_flow;       // -1 unchanged, 0 off, 1 on
} link_change_t;

static link_change_t link_after(const meta_t *m, const bridge_settings_t *cfg) {
    link_change_t c = {.baud = 0, .hw_flow = -1};
    if (m->baud > 0 && (uint32_t)m->baud != cfg->baud) c.baud = (uint32_t)m->baud;
    if (strcmp(m->flow, "hw_flow") == 0 && !cfg->hw_flow) c.hw_flow = 1;
    else if ((strcmp(m->flow, "sw_flow") == 0 || strcmp(m->flow, "no_flow") == 0) && cfg->hw_flow) c.hw_flow = 0;
    return c;
}

static int store_read(uint32_t off, uint8_t *buf, size_t len) { return io_image_read(NULL, off, buf, len); }

static void load_image(void) {
    img_hdr_t h;
    s_have = false;
    if (esp_partition_read(s_part, 0, &h, sizeof(h)) != ESP_OK || !hdr_valid(&h)) return;
    img_kind_t k = kind_for_magic(h.magic);
    nf_gbl_t g;
    memset(&g, 0, sizeof(g));
    char desc[96] = "";
    const char *why = k == IMG_GBL ? nf_gbl_check(&s_io, h.size, &g) : xf_check(k, store_read, h.size, h.name, desc, sizeof(desc));
    if (why) {
        ESP_LOGW(TAG, "Stored dongle image %s no longer checks out: %s", h.name, why);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_hdr = h;
    s_gbl = g;
    s_kind = k;
    strlcpy(s_desc, desc, sizeof(s_desc));
    s_have = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Stored dongle image: %s (%lu bytes)", h.name, (unsigned long)h.size);
}

const char *ncpflash_upload_begin(uint32_t size, const char *name) {
    if (s_part == NULL) return "No flash area for dongle images";
    if (size < 64) return "The file is too small to be firmware";
    if (size > AREA_SIZE - IMG_OFF) return "The file is too big for the dongle image area (2 MB)";
    dongle_info_t di;
    dongle_info(&di);
    img_kind_t kind = xf_kind_for(name ? name : "", di.model->family);
    if (kind == IMG_UNKNOWN) {
        size_t nl = name ? strlen(name) : 0;
        if (nl > 4 && strcasecmp(name + nl - 4, ".hex") == 0)
            return "A .hex can be TI or Nordic firmware: plug in the dongle it's for first";
        return "Upload a .gbl (Silicon Labs), .zip or .hex (TI Z-Stack), .GCF (ConBee) or .hex (nRF52840)";
    }
    if (!busy_take()) return "The dongle is being flashed or another image is uploading";
    // Drop the old image first: from here on there is none until this one checks out.
    if (esp_partition_erase_range(s_part, 0, IMG_OFF) != ESP_OK) {
        busy_give();
        return "Erasing the image area failed";
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_have = false;
    xSemaphoreGive(s_lock);
    memset(&s_up, 0, sizeof(s_up));
    s_up.active = true;
    s_up.kind = kind;
    s_up.size = size;
    strlcpy(s_up.name, name && name[0] ? name : "firmware.gbl", sizeof(s_up.name));
    mbedtls_md5_init(&s_up.md5);
    mbedtls_sha256_init(&s_up.sha);
    mbedtls_md5_starts(&s_up.md5);
    mbedtls_sha256_starts(&s_up.sha, 0);
    return NULL;
}

const char *ncpflash_upload_write(const uint8_t *data, size_t len) {
    if (!s_up.active) return "No upload in progress";
    if (s_up.got + len > s_up.size) return "More data than announced";
    while (s_up.got + len > s_up.erased) {
        uint32_t n = s_up.size - s_up.erased;
        if (n > ERASE_STEP) n = ERASE_STEP;
        n = (n + 4095) & ~4095u;
        if (esp_partition_erase_range(s_part, IMG_OFF + s_up.erased, n) != ESP_OK) return "Erasing the image area failed";
        s_up.erased += n;
    }
    if (esp_partition_write(s_part, IMG_OFF + s_up.got, data, len) != ESP_OK) return "Writing the image to flash failed";
    mbedtls_md5_update(&s_up.md5, data, len);
    mbedtls_sha256_update(&s_up.sha, data, len);
    s_up.got += len;
    return NULL;
}

const char *ncpflash_upload_end(bool complete) {
    if (!s_up.active) return "No upload in progress";
    uint8_t md5[16], sha[32];
    mbedtls_md5_finish(&s_up.md5, md5);
    mbedtls_sha256_finish(&s_up.sha, sha);
    mbedtls_md5_free(&s_up.md5);
    mbedtls_sha256_free(&s_up.sha);
    s_up.active = false;
    const char *fail = NULL;
    static char why[160];
    if (!complete || s_up.got != s_up.size) fail = "The upload was cut short";
    uint8_t md5b[16], shab[32];
    if (!fail && (fail = image_hashes(s_up.size, md5b, shab)) == NULL && memcmp(md5, md5b, 16) != 0)
        fail = "The image read back from flash doesn't match what was uploaded (bad flash write)";
    nf_gbl_t g;
    memset(&g, 0, sizeof(g));
    char desc[96] = "";
    if (!fail) {
        if (s_up.kind == IMG_GBL) fail = nf_gbl_check(&s_io, s_up.size, &g);
        else fail = xf_check(s_up.kind, store_read, s_up.size, s_up.name, desc, sizeof(desc));
    }
    if (!fail) {
        img_hdr_t h = {.magic = magic_for(s_up.kind), .size = s_up.size, .tag_len = s_up.kind == IMG_GBL ? g.tag_len : s_up.size,
                       .epoch = epoch_now()};
        memcpy(h.md5, md5, 16);
        memcpy(h.sha256, sha, 32);
        strlcpy(h.name, s_up.name, sizeof(h.name));
        h.crc = nf_crc32(0, (const uint8_t *)&h, offsetof(img_hdr_t, crc));
        img_hdr_t back;
        if (esp_partition_write(s_part, 0, &h, sizeof(h)) != ESP_OK || esp_partition_read(s_part, 0, &back, sizeof(back)) != ESP_OK ||
            memcmp(&h, &back, sizeof(h)) != 0) {
            fail = "Saving the image header failed";
        } else {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_hdr = h;
            s_gbl = g;
            s_kind = s_up.kind;
            strlcpy(s_desc, desc, sizeof(s_desc));
            s_have = true;
            xSemaphoreGive(s_lock);
        }
    }
    busy_give();
    char m[33];
    hex(m, md5, 16);
    if (fail) {
        snprintf(why, sizeof(why), "%s", fail);
        stats_event(EV_WARN, "Dongle firmware %s refused: %s", s_up.name, why);
        return why;
    }
    stats_event(EV_INFO, "Dongle firmware %s stored (%lu bytes, MD5 %s)", s_up.name, (unsigned long)s_up.size, m);
    return NULL;
}

const char *ncpflash_delete(void) {
    if (s_part == NULL) return "No flash area for dongle images";
    if (!busy_take()) return "The dongle is being flashed or an image is uploading";
    esp_err_t e = esp_partition_erase_range(s_part, 0, IMG_OFF);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_have = false;
    xSemaphoreGive(s_lock);
    busy_give();
    return e == ESP_OK ? NULL : "Erasing the image area failed";
}

// ---------------------------------------------------------------- flashing

static void finish(fstate_t st, const char *err) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_fs.state = st;
    if (err) strlcpy(s_fs.error, err, sizeof(s_fs.error));
    s_fs.ended = epoch_now();
    s_fs.ended_up = stats_uptime_s();
    xSemaphoreGive(s_lock);
}

static void xprogress(const char *step, uint32_t done, uint32_t total) { io_progress(NULL, step, done, total); }

// TI, ConBee and nRF images: the same claim/release and reporting as the .gbl path, the work in xflash.c.
static void flash_other(const img_hdr_t *h, img_kind_t k, const bridge_settings_t *cfg) {
    char err[200] = "";
    if (!dongle_claim()) {
        const char *why = dongle_ready() ? "The dongle is busy" : "No dongle connected";
        stats_event(EV_ERROR, "Dongle firmware not flashed: %s", why);
        finish(ST_FAILED, why);
        return;
    }
    stats_event(EV_WARN, "Flashing %s firmware %s: the client is cut off until it's done", xf_kind_name(k), h->name);
    vTaskDelay(pdMS_TO_TICKS(500));
    xf_result_t r;
    int64_t t0 = esp_timer_get_time();
    bool ok = xf_flash(k, store_read, h->size, h->name, xprogress, &r, err, sizeof(err));
    uint32_t secs = (uint32_t)((esp_timer_get_time() - t0) / 1000000);
    bool baud_change = ok && r.link_baud && r.link_baud != cfg->baud;
    if (baud_change) dongle_set_app_baud(r.link_baud);
    dongle_release();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_fs.res, 0, sizeof(s_fs.res));
    s_fs.res.entered = r.entered;
    s_fs.res.app_left_running = r.left_running;
    s_fs.res.in_bootloader = r.in_bootloader;
    strlcpy(s_fs.res.before_spinel, r.before, sizeof(s_fs.res.before_spinel));
    strlcpy(s_fs.res.after_spinel, r.after, sizeof(s_fs.res.after_spinel));
    xSemaphoreGive(s_lock);
    if (baud_change) {
        char serr[96] = "";
        if (settings_save_link(r.link_baud, false, serr, sizeof(serr)) != ESP_OK)
            stats_event(EV_ERROR, "New firmware is on, but saving the bridge's baud failed: %s", serr);
        else
            stats_event(EV_INFO, "Bridge baud set to %lu to match the new firmware", (unsigned long)r.link_baud);
    }
    if (ok) {
        if (r.after[0]) dongle_note_fw(r.after, "");
        ESP_LOGI(TAG, "Dongle flashed in %lu s: %s -> %s", (unsigned long)secs, r.before[0] ? r.before : "?", r.after[0] ? r.after : "?");
        stats_event(EV_INFO, "Dongle firmware flashed in %lu s%s%s%s%s", (unsigned long)secs, r.before[0] || r.after[0] ? ": " : "",
                    r.before, r.before[0] && r.after[0] ? " -> " : "", r.after);
        finish(ST_DONE, NULL);
    } else {
        const char *state = r.left_running ? "its previous firmware is still running"
                            : r.in_bootloader ? "it's in its bootloader: flash again"
                                              : "check it";
        ESP_LOGE(TAG, "Dongle flash failed: %s (%s)", err, state);
        stats_event(EV_ERROR, "Dongle flash failed: %s (%s)", err, state);
        finish(ST_FAILED, err);
    }
}

static void flash_task(void *arg) {
    (void)arg;
    char err[200] = "";
    img_hdr_t h;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    h = s_hdr;
    xSemaphoreGive(s_lock);
    bridge_settings_t cfg;
    settings_get(&cfg);

    // 1. The stored image again, from flash: header, MD5, structure, CRC-32, metadata.
    io_progress(NULL, "image", 0, h.size);
    img_hdr_t now;
    uint8_t md5[16], sha[32];
    nf_gbl_t g;
    meta_t m;
    memset(&g, 0, sizeof(g));
    memset(&m, 0, sizeof(m));
    const char *why = NULL;
    char pbuf[160];
    if (esp_partition_read(s_part, 0, &now, sizeof(now)) != ESP_OK || !hdr_valid(&now) || memcmp(&now, &h, sizeof(h)) != 0)
        why = "The stored image changed or is gone";
    else if ((why = image_hashes(h.size, md5, sha)) == NULL && (memcmp(md5, h.md5, 16) != 0 || memcmp(sha, h.sha256, 32) != 0))
        why = "The stored image no longer matches its MD5/SHA-256 (flash damage): upload it again";
    else if (why == NULL && kind_for_magic(h.magic) != IMG_GBL) {
        // TI / ConBee / nRF: checked again, then flashed through xflash.c
        char desc[96];
        img_kind_t k = kind_for_magic(h.magic);
        if ((why = xf_check(k, store_read, h.size, h.name, desc, sizeof(desc))) == NULL) {
            flash_other(&h, k, &cfg);
            busy_give();
            vTaskDelete(NULL);
            return;
        }
    } else if (why == NULL && (why = nf_gbl_check(&s_io, h.size, &g)) == NULL) {
        parse_meta(&g, &m);
        why = meta_problem(&m, cfg.baud, pbuf, sizeof(pbuf));
    }
    if (why) {
        ESP_LOGE(TAG, "Not flashing: %s", why);
        stats_event(EV_ERROR, "Dongle firmware not flashed: %s", why);
        finish(ST_FAILED, why);
        busy_give();
        vTaskDelete(NULL);
        return;
    }

    // 2. Take the dongle from Zigbee2MQTT.
    if (!dongle_claim()) {
        why = dongle_ready() ? "The dongle is busy" : "No dongle connected";
        stats_event(EV_ERROR, "Dongle firmware not flashed: %s", why);
        finish(ST_FAILED, why);
        busy_give();
        vTaskDelete(NULL);
        return;
    }
    stats_event(EV_WARN, "Flashing dongle firmware %s: Zigbee2MQTT is cut off until it's done", h.name);
    vTaskDelay(pdMS_TO_TICKS(500));  // the bridge drops Zigbee2MQTT within 250 ms

    // 3. Flash.
    nf_flash_opts_t o = {.app_baud = cfg.baud,
                         .new_baud = m.baud > 0 ? (uint32_t)m.baud : cfg.baud,
                         .upload_len = g.tag_len,
                         .expect = cfg.role == BRIDGE_ROLE_THREAD ? NF_EXPECT_SPINEL : NF_EXPECT_EZSP};
    nf_result_t r;
    int64_t t0 = esp_timer_get_time();
    bool ok = nf_flash(&s_io, &o, &r, err, sizeof(err));
    uint32_t secs = (uint32_t)((esp_timer_get_time() - t0) / 1000000);
    link_change_t lc = link_after(&m, &cfg);
    // The new firmware answered at o.new_baud: from here that's the link's baud (release puts it back).
    if (ok && lc.baud) dongle_set_app_baud(lc.baud);
    dongle_release();
    if (ok && (lc.baud || lc.hw_flow >= 0)) {
        bridge_settings_t nc = cfg;
        if (lc.baud) nc.baud = lc.baud;
        if (lc.hw_flow >= 0) nc.hw_flow = lc.hw_flow == 1;
        char serr[96] = "";
        if (settings_save_link(nc.baud, nc.hw_flow, serr, sizeof(serr)) != ESP_OK)
            stats_event(EV_ERROR, "New firmware is on, but saving the bridge's new link settings failed: %s", serr);
        if (lc.baud)
            stats_event(EV_INFO, "Bridge baud set to %lu (was %lu) to match the new dongle firmware", (unsigned long)lc.baud,
                        (unsigned long)cfg.baud);
        if (lc.hw_flow >= 0) {
            char ferr[96] = "";
            if (dongle_set_hw_flow(lc.hw_flow == 1, ferr, sizeof(ferr)) == ESP_OK)
                stats_event(EV_INFO, "Hardware flow control (CTS) turned %s for the new %s firmware", lc.hw_flow ? "on" : "off",
                            m.flow);
            else
                stats_event(EV_WARN, "The new firmware is %s, but the CTS handshake couldn't be turned on: %s", m.flow, ferr);
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_fs.res = r;
    xSemaphoreGive(s_lock);
    char before[48] = "unknown", after[48] = "?";
    if (r.before.ok) fmt_ncp(before, sizeof(before), r.before.ezsp_version, r.before.stack_version);
    else if (r.before_spinel[0]) strlcpy(before, r.before_spinel, sizeof(before));
    if (r.after_spinel[0]) {
        strlcpy(after, r.after_spinel, sizeof(after));
        dongle_note_fw(r.after_spinel, "");
    }
    if (r.after.ok) {
        fmt_ncp(after, sizeof(after), r.after.ezsp_version, r.after.stack_version);
        stats_lock();
        g_stats.ezsp_version = r.after.ezsp_version;
        g_stats.stack_type = r.after.stack_type;
        g_stats.stack_version = r.after.stack_version;
        stats_unlock();
    }
    if (ok) {
        ESP_LOGI(TAG, "Dongle flashed in %lu s: %s -> %s", (unsigned long)secs, before, after);
        stats_event(EV_INFO, "Dongle firmware flashed in %lu s: %s -> %s", (unsigned long)secs, before, after);
        finish(ST_DONE, NULL);
    } else {
        const char *state = r.app_left_running ? "its previous firmware is still running"
                            : r.in_bootloader  ? "it's in its bootloader with no working firmware: flash again"
                                               : "check it";
        ESP_LOGE(TAG, "Dongle flash failed: %s (%s)", err, state);
        stats_event(EV_ERROR, "Dongle flash failed: %s (%s)", err, state);
        finish(ST_FAILED, err);
    }
    busy_give();
    vTaskDelete(NULL);
}

bool ncpflash_flash(const char *md5, char *why, size_t why_len) {
    img_hdr_t h;
    bool have;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    h = s_hdr;
    have = s_have;
    xSemaphoreGive(s_lock);
    char m[33];
    hex(m, h.md5, 16);
    if (!have) {
        strlcpy(why, "No dongle firmware uploaded", why_len);
        return false;
    }
    if (md5 == NULL || strcasecmp(md5, m) != 0) {
        strlcpy(why, "That isn't the stored image (MD5 differs): reload the page", why_len);
        return false;
    }
    if (!dongle_ready()) {
        strlcpy(why, "No dongle connected", why_len);
        return false;
    }
    dongle_info_t d;
    dongle_info(&d);
    img_kind_t k = kind_for_magic(h.magic);
    family_t want = xf_kind_family(k);
    if (d.model->family != want && d.model->family != FAM_UNKNOWN) {
        snprintf(why, why_len, "%s has a %s radio (%s): this is %s firmware", d.model->name, family_name(d.model->family),
                 d.model->radio, xf_kind_name(k));
        return false;
    }
    bridge_settings_t rc;
    settings_get(&rc);
    if (k != IMG_GBL && rc.role == BRIDGE_ROLE_THREAD) {
        snprintf(why, why_len, "%s firmware is Zigbee: set the dongle role to Zigbee first", xf_kind_name(k));
        return false;
    }
    if (!busy_take()) {
        strlcpy(why, "The dongle is already being flashed", why_len);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_fs, 0, sizeof(s_fs));
    s_fs.state = ST_RUNNING;
    strlcpy(s_fs.step, "image", sizeof(s_fs.step));
    s_fs.started = epoch_now();
    s_fs.started_up = stats_uptime_s();
    xSemaphoreGive(s_lock);
    if (xTaskCreate(flash_task, "dongle_fw", 8192, NULL, 7, NULL) != pdPASS) {
        finish(ST_FAILED, "out of memory");
        busy_give();
        strlcpy(why, "out of memory", why_len);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- JSON

void ncpflash_add_json(cJSON *root) {
    img_hdr_t h;
    nf_gbl_t *g = malloc(sizeof(nf_gbl_t));
    flash_state_t *fs = malloc(sizeof(flash_state_t));
    if (!g || !fs) {
        free(g);
        free(fs);
        return;
    }
    bool have;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    h = s_hdr;
    *g = s_gbl;
    *fs = s_fs;
    have = s_have;
    img_kind_t kind = s_kind;
    char desc[96];
    strlcpy(desc, s_desc, sizeof(desc));
    xSemaphoreGive(s_lock);
    bridge_settings_t cfg;
    settings_get(&cfg);

    cJSON *d = cJSON_AddObjectToObject(root, "dongle_fw");
    cJSON_AddNumberToObject(d, "max_size", s_part ? AREA_SIZE - IMG_OFF : 0);
    cJSON_AddNumberToObject(d, "baud", cfg.baud);
    cJSON_AddBoolToObject(d, "busy", s_busy);
    stats_lock();
    bool present = g_stats.dongle_open, z2m = g_stats.client_connected;
    uint8_t ev = g_stats.ezsp_version;
    uint16_t sv = g_stats.stack_version;
    stats_unlock();
    cJSON_AddBoolToObject(d, "dongle", present);
    cJSON_AddBoolToObject(d, "z2m_connected", z2m);
    dongle_info_t di;
    dongle_info(&di);
    if (di.fw_running[0]) {  // Spinel / Z-Stack version read off the traffic (or the last flash)
        cJSON_AddStringToObject(d, "current", di.fw_running);
    } else if (ev) {
        char v[48];
        fmt_ncp(v, sizeof(v), ev, sv);
        cJSON_AddStringToObject(d, "current", v);
    } else {
        cJSON_AddNullToObject(d, "current");
    }

    if (have) {
        cJSON *im = cJSON_AddObjectToObject(d, "image");
        char hx[65];
        cJSON_AddStringToObject(im, "name", h.name);
        cJSON_AddNumberToObject(im, "size", h.size);
        cJSON_AddNumberToObject(im, "upload_size", h.tag_len);
        hex(hx, h.md5, 16);
        cJSON_AddStringToObject(im, "md5", hx);
        hex(hx, h.sha256, 32);
        cJSON_AddStringToObject(im, "sha256", hx);
        if (h.epoch) cJSON_AddNumberToObject(im, "uploaded", (double)h.epoch);
        cJSON_AddStringToObject(im, "kind", xf_kind_name(kind));
        if (desc[0]) cJSON_AddStringToObject(im, "summary", desc);
    }
    if (have && kind != IMG_GBL) {
        // nothing .gbl-specific to add
    } else if (have) {
        cJSON *im = cJSON_GetObjectItem(d, "image");
        cJSON_AddBoolToObject(im, "signed", g->is_signed);
        cJSON_AddBoolToObject(im, "encrypted", g->encrypted);
        meta_t m;
        parse_meta(g, &m);
        if (m.fw_type[0]) cJSON_AddStringToObject(im, "fw_type", m.fw_type);
        if (m.ezsp[0]) cJSON_AddStringToObject(im, "version", m.ezsp);
        if (m.sdk[0]) cJSON_AddStringToObject(im, "sdk", m.sdk);
        if (m.baud) cJSON_AddNumberToObject(im, "image_baud", m.baud);
        if (m.flow[0]) cJSON_AddStringToObject(im, "flow", m.flow);
        char pbuf[160];
        const char *p = meta_problem(&m, cfg.baud, pbuf, sizeof(pbuf));
        if (p) cJSON_AddStringToObject(im, "problem", p);
        link_change_t lc = link_after(&m, &cfg);
        if (lc.baud) cJSON_AddNumberToObject(im, "sets_baud", lc.baud);
        if (lc.hw_flow >= 0) cJSON_AddBoolToObject(im, "sets_hw_flow", lc.hw_flow == 1);
    } else {
        cJSON_AddNullToObject(d, "image");
    }

    cJSON *f = cJSON_AddObjectToObject(d, "flash");
    static const char *names[] = {"idle", "running", "done", "failed"};
    cJSON_AddStringToObject(f, "state", names[fs->state]);
    if (fs->state != ST_IDLE) {
        cJSON_AddStringToObject(f, "step", fs->step);
        cJSON_AddNumberToObject(f, "done", fs->done);
        cJSON_AddNumberToObject(f, "total", fs->total);
        if (fs->error[0]) cJSON_AddStringToObject(f, "error", fs->error);
        if (fs->started) cJSON_AddNumberToObject(f, "started", (double)fs->started);
        uint32_t end = fs->state == ST_RUNNING ? stats_uptime_s() : fs->ended_up;
        cJSON_AddNumberToObject(f, "seconds", end - fs->started_up);
        if (fs->res.entered) cJSON_AddStringToObject(f, "entered", fs->res.entered);
        if (fs->res.bootloader[0]) cJSON_AddStringToObject(f, "bootloader", fs->res.bootloader);
        char v[48];
        if (fs->res.before.ok) {
            fmt_ncp(v, sizeof(v), fs->res.before.ezsp_version, fs->res.before.stack_version);
            cJSON_AddStringToObject(f, "before", v);
        }
        if (fs->res.after.ok) {
            fmt_ncp(v, sizeof(v), fs->res.after.ezsp_version, fs->res.after.stack_version);
            cJSON_AddStringToObject(f, "after", v);
        }
        if (fs->state == ST_FAILED)
            cJSON_AddStringToObject(f, "dongle_left", fs->res.app_left_running ? "running" : fs->res.in_bootloader ? "bootloader" : "unknown");
        cJSON_AddNumberToObject(f, "retries", fs->res.retries);
    }
    free(g);
    free(fs);
}

// ---------------------------------------------------------------- on-device test (test builds only)

#ifdef CONFIG_BRIDGE_TEST_HOOKS
#include "esp_http_client.h"
#include "net.h"

// Pulls <url>/1.gbl .. /6.gbl through the same upload path the web page uses and logs each result
// (tools/dongle_store_test.py serves them and checks the log).
static void store_test_task(void *arg) {
    (void)arg;
    if (s_have) {  // second boot: the image from the first run must still be here; then tidy up
        ESP_LOGW(TAG, "TEST persisted: %s", s_hdr.name);
        const char *e = ncpflash_delete();
        ESP_LOGW(TAG, "TEST cleaned up: %s", e ? e : "image removed");
        vTaskDelete(NULL);
        return;
    }
    while (!net_has_ip()) vTaskDelay(pdMS_TO_TICKS(500));
    vTaskDelay(pdMS_TO_TICKS(3000));
    for (int n = 1; n <= 6; n++) {
        char url[160], name[16];
        snprintf(url, sizeof(url), "%s/%d.gbl", CONFIG_BRIDGE_TEST_DONGLE_URL, n);
        snprintf(name, sizeof(name), "%d.gbl", n);
        esp_http_client_config_t c = {.url = url, .timeout_ms = 10000};
        esp_http_client_handle_t h = esp_http_client_init(&c);
        const char *res = NULL;
        int64_t t0 = esp_timer_get_time();
        if (esp_http_client_open(h, 0) != ESP_OK) {
            res = "can't connect";
        } else {
            int64_t len = esp_http_client_fetch_headers(h);
            res = ncpflash_upload_begin(len > 0 ? (uint32_t)len : 0, name);
            if (res == NULL) {
                uint8_t *buf = malloc(4096);
                bool ok = buf != NULL;
                int64_t got = 0;
                while (ok && got < len) {
                    int r = esp_http_client_read(h, (char *)buf, 4096);
                    if (r <= 0) ok = false;
                    else if ((res = ncpflash_upload_write(buf, r)) != NULL) ok = false;
                    else got += r;
                }
                free(buf);
                const char *e = ncpflash_upload_end(ok && got == len);
                if (res == NULL) res = e;
            }
        }
        esp_http_client_cleanup(h);
        ESP_LOGW(TAG, "TEST dongle image %d: %s (%lld ms)", n, res ? res : "stored", (esp_timer_get_time() - t0) / 1000);
        cJSON *r = cJSON_CreateObject();
        ncpflash_add_json(r);
        char *s = cJSON_PrintUnformatted(r);
        ESP_LOGW(TAG, "TEST json %d: %s", n, s ? s : "?");
        free(s);
        cJSON_Delete(r);
    }
    char why[96], m[33];
    hex(m, s_hdr.md5, 16);
    bool ok = ncpflash_flash("00000000000000000000000000000000", why, sizeof(why));
    ESP_LOGW(TAG, "TEST flash with the wrong MD5: %s", ok ? "STARTED" : why);
    ok = ncpflash_flash(m, why, sizeof(why));
    ESP_LOGW(TAG, "TEST flash with no dongle: %s", ok ? "STARTED" : why);
    ESP_LOGW(TAG, "TEST done");
    vTaskDelete(NULL);
}
#endif

// ---------------------------------------------------------------- start

void ncpflash_start(void) {
    s_lock = xSemaphoreCreateMutex();
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, AREA_SUBTYPE, AREA_LABEL);
    if (s_part == NULL) {
        // Boards updated over the network keep the partition table they were first flashed with: the area
        // is free flash right after ota_1, so claim it the same way the table would.
        esp_err_t e = esp_partition_register_external(NULL, AREA_OFFSET, AREA_SIZE, AREA_LABEL, ESP_PARTITION_TYPE_DATA,
                                                      AREA_SUBTYPE, &s_part);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "No dongle image area: %s", esp_err_to_name(e));
            s_part = NULL;
            return;
        }
    }
    ESP_LOGI(TAG, "Dongle image area at 0x%lx, %lu KB", (unsigned long)s_part->address, (unsigned long)(s_part->size / 1024));
    load_image();
#ifdef CONFIG_BRIDGE_TEST_HOOKS
    if (CONFIG_BRIDGE_TEST_DONGLE_URL[0]) xTaskCreate(store_test_task, "dfw_test", 6144, NULL, 4, NULL);
#endif
}

#include "xflash.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dongle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gcfflash_core.h"
#include "ihex_core.h"
#include "miniz.h"
#include "tinfl_ring.h"
#include "nrfdfu_core.h"
#include "tiflash_core.h"
#include "unzip_core.h"

static const char *TAG = "xflash";

static bool ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcasecmp(s + a - b, suf) == 0;
}

img_kind_t xf_kind_for(const char *name, family_t family) {
    if (ends_with(name, ".gbl")) return IMG_GBL;
    if (ends_with(name, ".gcf")) return IMG_GCF;
    if (ends_with(name, ".zip")) return IMG_TI;  // Koenkk's releases
    if (ends_with(name, ".hex")) {
        if (family == FAM_TI) return IMG_TI;
        if (family == FAM_NORDIC) return IMG_NRF;
        return IMG_UNKNOWN;  // a .hex for what? the dongle decides
    }
    return IMG_UNKNOWN;
}

const char *xf_kind_name(img_kind_t k) {
    switch (k) {
        case IMG_GBL: return "Silicon Labs .gbl";
        case IMG_TI: return "TI Z-Stack";
        case IMG_GCF: return "ConBee .GCF";
        case IMG_NRF: return "nRF52840 .hex";
        default: return "unknown";
    }
}

family_t xf_kind_family(img_kind_t k) {
    switch (k) {
        case IMG_GBL: return FAM_SILABS;
        case IMG_TI: return FAM_TI;
        case IMG_GCF: return FAM_DECONZ;
        case IMG_NRF: return FAM_NORDIC;
        default: return FAM_UNKNOWN;
    }
}

// ---------------------------------------------------------------- io shared by the cores

static xf_read_fn s_rd;               // the stored file
static const uint8_t *s_buf;          // or a converted image in PSRAM
static uint32_t s_buf_len;
static xf_progress_fn s_progress;

static int io_read(void *ctx, uint8_t *buf, size_t max, uint32_t timeout_ms) {
    (void)ctx;
    if (!dongle_fl_ready()) return -1;
    size_t n = xStreamBufferReceive(dongle_claim_rx(), buf, max, pdMS_TO_TICKS(timeout_ms));
    if (n == 0 && !dongle_fl_ready()) return -1;
    return (int)n;
}
static int io_write(void *ctx, const uint8_t *buf, size_t len) {
    (void)ctx;
    if (!dongle_fl_ready()) return -1;
    return dongle_claimed_write(buf, len, 2000) == ESP_OK ? 0 : -1;
}
static int io_baud(void *ctx, uint32_t baud) {
    (void)ctx;
    return dongle_set_baud(baud) == ESP_OK ? 0 : -1;
}
static int io_lines(void *ctx, bool dtr, bool rts) {
    (void)ctx;
    esp_err_t e = dongle_set_lines(dtr, rts);
    return e == ESP_OK || e == ESP_ERR_NOT_SUPPORTED ? 0 : -1;
}
static void io_sleep(void *ctx, uint32_t ms) {
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
}
static uint32_t io_now(void *ctx) {
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}
static int io_image_store(void *ctx, uint32_t off, uint8_t *buf, size_t len) {
    (void)ctx;
    return s_rd ? s_rd(off, buf, len) : -1;
}
static int io_image_buf(void *ctx, uint32_t off, uint8_t *buf, size_t len) {
    (void)ctx;
    if (!s_buf || off + len > s_buf_len) return -1;
    memcpy(buf, s_buf + off, len);
    return 0;
}
static void io_progress(void *ctx, const char *step, uint32_t done, uint32_t total) {
    (void)ctx;
    if (s_progress) s_progress(step, done, total);
}
static void io_log(void *ctx, const char *msg) {
    (void)ctx;
    ESP_LOGI(TAG, "%s", msg);
}
static int io_reopen(void *ctx, uint32_t timeout_ms) {
    (void)ctx;
    return dongle_wait_reopen(timeout_ms) == ESP_OK ? 0 : -1;
}

static void *big_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

// ---------------------------------------------------------------- ConBee (.GCF)

static gf_device_t conbee_kind(const dongle_info_t *d) {
    if (strcmp(d->model->id, "conbee_2") == 0) return GF_DEV_CONBEE2;
    if (strcmp(d->model->id, "conbee_3") == 0) return GF_DEV_CONBEE3;
    return GF_DEV_UNKNOWN;
}

static const gf_io_t s_gf_store = {.read = io_read, .write = io_write, .set_baud = io_baud, .sleep_ms = io_sleep, .now_ms = io_now,
                                   .image_read = io_image_store, .progress = io_progress, .log = io_log, .reopen = io_reopen};

static const char *check_gcf(uint32_t size, const char *name, char *desc, size_t dl) {
    dongle_info_t d;
    dongle_fl_info(&d);
    gf_device_t want = conbee_kind(&d);
    gf_file_t f;
    const char *why;
    if (want != GF_DEV_UNKNOWN) {
        why = gf_file_check(&s_gf_store, size, name, want, &f);
    } else {  // no ConBee attached right now: fine if it suits either
        why = gf_file_check(&s_gf_store, size, name, GF_DEV_CONBEE2, &f);
        if (why) why = gf_file_check(&s_gf_store, size, name, GF_DEV_CONBEE3, &f);
    }
    if (!why)
        snprintf(desc, dl, "deCONZ firmware for %s%s", f.device == GF_DEV_CONBEE2 ? "ConBee II" : "ConBee III",
                 f.name_version ? "" : " (no version in the file name)");
    if (!why && f.name_version) {
        size_t n = strlen(desc);
        snprintf(desc + n, dl - n, ", version 0x%08lx", (unsigned long)f.name_version);
    }
    return why;
}

static bool flash_gcf(uint32_t size, const char *name, xf_result_t *r, char *err, size_t el) {
    dongle_info_t d;
    dongle_fl_info(&d);
    gf_device_t dev = conbee_kind(&d);
    if (dev == GF_DEV_UNKNOWN) {
        snprintf(err, el, "The stick isn't a ConBee II or III");
        return false;
    }
    gf_flash_opts_t o = {.device = dev, .file_name = name, .size = size, .budget_ms = 0};
    gf_result_t g;
    bool ok = gf_flash(&s_gf_store, &o, &g, err, el);
    if (g.before) snprintf(r->before, sizeof(r->before), "deCONZ 0x%08lx", (unsigned long)g.before);
    if (g.after) snprintf(r->after, sizeof(r->after), "deCONZ 0x%08lx", (unsigned long)g.after);
    r->entered = g.entered;
    r->left_running = g.app_left_running;
    r->in_bootloader = g.in_bootloader;
    r->link_baud = GF_BAUD;
    return ok;
}

// ---------------------------------------------------------------- nRF52840 (.hex -> .bin + built init packet)

#define NRF_BIN_MAX (ND_APP_LIMIT - ND_APP_START)

static const nd_io_t s_nd_store = {.read = io_read, .write = io_write, .set_lines = io_lines, .sleep_ms = io_sleep, .now_ms = io_now,
                                   .image_read = io_image_store, .progress = io_progress, .log = io_log, .enter_dfu = NULL};
static const nd_io_t s_nd_buf = {.read = io_read, .write = io_write, .set_lines = io_lines, .sleep_ms = io_sleep, .now_ms = io_now,
                                 .image_read = io_image_buf, .progress = io_progress, .log = io_log, .enter_dfu = NULL};

// .hex -> .bin in PSRAM, then the init packet the open bootloader wants. Caller frees *bin.
static const char *nrf_prepare(uint32_t size, uint8_t **bin, uint32_t *bin_len, uint8_t *init, size_t *init_len) {
    *bin = big_alloc(NRF_BIN_MAX);
    if (!*bin) return "out of memory";
    uint32_t start = 0, dropped = 0;
    const char *why = nd_hex_to_bin(&s_nd_store, size, *bin, NRF_BIN_MAX, bin_len, &start, &dropped);
    if (why) return why;
    if (start != ND_APP_START) return "This .hex isn't an nRF52840 Dongle application (it doesn't start at 0x1000)";
    s_buf = *bin;
    s_buf_len = *bin_len;
    uint8_t sha[32];
    if ((why = nd_image_sha256(&s_nd_buf, *bin_len, sha)) != NULL) return why;
    *init_len = nd_init_build(*bin_len, sha, 1, ND_HW_VERSION, init, ND_INIT_MAX);
    if (!*init_len) return "couldn't build the DFU init packet";
    nd_init_t in;
    if ((why = nd_init_parse(init, *init_len, &in)) != NULL) return why;
    if ((why = nd_init_policy(&in)) != NULL) return why;
    return nd_image_check(&s_nd_buf, *bin_len, &in);
}

static const char *check_nrf(uint32_t size, char *desc, size_t dl) {
    uint8_t *bin = NULL, init[ND_INIT_MAX];
    uint32_t bin_len = 0;
    size_t init_len = 0;
    const char *why = nrf_prepare(size, &bin, &bin_len, init, &init_len);
    if (!why) snprintf(desc, dl, "nRF52840 stick application, %lu KB", (unsigned long)(bin_len + 1023) / 1024);
    free(bin);
    s_buf = NULL;
    return why;
}

static bool flash_nrf(uint32_t size, xf_result_t *r, char *err, size_t el) {
    uint8_t *bin = NULL, init[ND_INIT_MAX];
    uint32_t bin_len = 0;
    size_t init_len = 0;
    const char *why = nrf_prepare(size, &bin, &bin_len, init, &init_len);
    if (why) {
        snprintf(err, el, "%s", why);
        free(bin);
        s_buf = NULL;
        return false;
    }
    nd_flash_opts_t o = {.init = init, .init_len = init_len, .bin_len = bin_len};
    nd_result_t nr;
    bool ok = nd_flash(&s_nd_buf, &o, &nr, err, el);
    free(bin);
    s_buf = NULL;
    r->entered = nr.entered;
    r->in_bootloader = !ok && nr.app_erased;
    if (!ok && !nr.app_erased && strstr(err, "bootloader") == NULL)
        snprintf(err + strlen(err), el - strlen(err), " (press the stick's RESET button sideways to start its bootloader, then flash again)");
    if (ok) {
        // the bootloader starts the new app: it comes back on USB as itself
        dongle_wait_reopen(15000);
        snprintf(r->after, sizeof(r->after), "nRF52840 application (%lu KB)", (unsigned long)(bin_len + 1023) / 1024);
    }
    return ok;
}

// ---------------------------------------------------------------- TI CC26x2 (Koenkk .zip / .hex -> flat image)

#define TI_FLASH_MAX 0xB0000  // CC2652P7: 704 KB (the 352 KB parts fit too)

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    ihex_t hex;
    const char *err;
} ti_conv_t;

static int ti_put(void *ctx, uint32_t addr, const uint8_t *d, size_t n) {
    ti_conv_t *c = ctx;
    if (addr >= c->cap || n > c->cap - addr) {  // never addr + n: 32-bit, it can wrap
        ihex_fail(&c->hex, "data beyond any CC26x2 flash (over 704 KB)");
        return -1;
    }
    memcpy(c->buf + addr, d, n);
    return 0;
}

static int ti_hex_text(void *ctx, const uint8_t *d, size_t n) {
    ti_conv_t *c = ctx;
    const char *e = ihex_feed(&c->hex, (const char *)d, n);
    if (e) {
        c->err = e;
        return -1;
    }
    return 0;
}

// The stored .zip or .hex -> flat image in PSRAM (0xFF where the file has nothing). Caller frees *out.
static const char *ti_convert(xf_read_fn rd, uint32_t size, const char *name, uint8_t **out, uint32_t *len) {
    ti_conv_t c = {.cap = TI_FLASH_MAX};
    c.buf = big_alloc(TI_FLASH_MAX);
    *out = c.buf;
    *len = 0;
    if (!c.buf) return "out of memory";
    memset(c.buf, 0xFF, TI_FLASH_MAX);
    ihex_init(&c.hex, ti_put, &c);
    bool zip = strlen(name) > 4 && strcasecmp(name + strlen(name) - 4, ".zip") == 0;
    uint8_t *piece = malloc(4096);
    rom_inf_t *inf = zip ? big_alloc(sizeof(rom_inf_t)) : NULL;
    uz_t *uz = zip ? big_alloc(sizeof(uz_t)) : NULL;
    const char *why = NULL;
    if (!piece || (zip && (!inf || !uz))) {
        why = "out of memory";
    } else {
        if (zip) {
            memset(inf, 0, sizeof(*inf));
            tinfl_init(&inf->d);
            uz_inflater_t ui = {.ctx = inf, .inflate = rom_inflate};
            uz_init(uz, &ui, ti_hex_text, &c);
        }
        for (uint32_t off = 0; off < size && !why; off += 4096) {
            size_t n = size - off < 4096 ? size - off : 4096;
            if (rd(off, piece, n) != 0) why = "reading the stored image failed";
            else if (zip) why = uz_feed(uz, piece, n);
            else why = ihex_feed(&c.hex, (const char *)piece, n);
            if ((off & 0xFFFF) == 0) vTaskDelay(1);  // a 4 MB unpack is seconds of CPU: let the idle task run
        }
        if (!why && zip) why = uz_finish(uz);
        if (why && c.err) why = c.err;
        if (!why) why = ihex_finish(&c.hex);
        if (!why && c.hex.total_bytes == 0) why = "the file has no firmware data";
        if (!why && c.hex.min_addr != 0) why = "this isn't a whole-chip CC26x2 image (it doesn't start at 0)";
    }
    *len = c.hex.end_addr;
    free(piece);
    free(inf);
    free(uz);
    return why;
}

static const tf_io_t s_tf_buf = {.read = io_read, .write = io_write, .set_baud = io_baud, .set_lines = io_lines, .sleep_ms = io_sleep,
                                 .now_ms = io_now, .image_read = io_image_buf, .progress = io_progress, .log = io_log};

static uint32_t ti_chip_flash(void) {
    dongle_info_t d;
    dongle_fl_info(&d);
    if (d.model->family != FAM_TI || !d.model->radio) return 0;  // unknown: the image decides
    return (strstr(d.model->radio, "P7") || strstr(d.model->radio, "R7")) ? 0xB0000 : 0x58000;
}

static const char *xf_ti_check(xf_read_fn rd, uint32_t size, const char *name, char *desc, size_t dl) {
    uint8_t *img = NULL;
    uint32_t len = 0;
    const char *why = ti_convert(rd, size, name, &img, &len);
    if (!why) {
        s_buf = img;
        s_buf_len = len;
        tf_image_t ti;
        why = tf_image_check(&s_tf_buf, len, ti_chip_flash(), &ti);
        if (!why)
            snprintf(desc, dl, "Z-Stack image for a %lu KB CC26x2, ROM bootloader on DIO%u (active %s)", (unsigned long)(len / 1024),
                     ti.bl_pin, ti.bl_level ? "high" : "low");
    }
    free(img);
    s_buf = NULL;
    return why;
}

static bool xf_ti_flash(xf_read_fn rd, uint32_t size, const char *name, xf_result_t *r, char *err, size_t el) {
    uint8_t *img = NULL;
    uint32_t len = 0;
    const char *why = ti_convert(rd, size, name, &img, &len);
    if (why) {
        snprintf(err, el, "%s", why);
        free(img);
        return false;
    }
    s_buf = img;
    s_buf_len = len;
    dongle_info_t d;
    dongle_fl_info(&d);
    tf_entry_t entry = d.model->reset == RST_TI_SONOFF   ? TF_ENTRY_SONOFF
                       : d.model->reset == RST_TI_CC2538 ? TF_ENTRY_DEFAULT
                       : d.model->family == FAM_TI       ? TF_ENTRY_NONE
                                                         : TF_ENTRY_DEFAULT;
    tf_opts_t o = {.entry = entry, .image_len = len, .chip_flash = ti_chip_flash()};
    if (d.fw_running[0]) strlcpy(r->before, d.fw_running, sizeof(r->before));
    tf_result_t tr;
    memset(&tr, 0, sizeof(tr));
    bool ok = tf_flash(&s_tf_buf, &o, &tr, err, el);
    free(img);
    s_buf = NULL;
    r->entered = tr.entered;
    r->in_bootloader = !ok && tr.erased;
    r->left_running = !ok && !tr.erased;
    r->link_baud = 115200;  // Z-Stack's UART rate
    if (ok) {
        // the build date from Koenkk's name: ..._coordinator_YYYYMMDD.zip
        const char *date = NULL;
        size_t nl = strlen(name);
        if (nl >= 13 && name[nl - 13] == '_') date = name + nl - 12;
        snprintf(r->after, sizeof(r->after), "Z-Stack %.8s (CRC-32 verified on the chip)", date ? date : "image");
    } else if (entry == TF_ENTRY_NONE && !tr.erased) {
        size_t n = strlen(err);
        snprintf(err + n, el - n, " (hold the stick's BOOT button while plugging it in, then flash again)");
    }
    return ok;
}

// ---------------------------------------------------------------- dispatch

const char *xf_check(img_kind_t k, xf_read_fn rd, uint32_t size, const char *name, char *desc, size_t desc_len) {
    s_rd = rd;
    desc[0] = 0;
    switch (k) {
        case IMG_GCF: return check_gcf(size, name, desc, desc_len);
        case IMG_NRF: return check_nrf(size, desc, desc_len);
        case IMG_TI: return xf_ti_check(rd, size, name, desc, desc_len);
        default: return "not a firmware file the bridge knows";
    }
}

bool xf_flash(img_kind_t k, xf_read_fn rd, uint32_t size, const char *name, xf_progress_fn progress, xf_result_t *r,
              char *err, size_t errlen) {
    memset(r, 0, sizeof(*r));
    err[0] = 0;
    s_rd = rd;
    s_progress = progress;
    bool ok;
    switch (k) {
        case IMG_GCF: ok = flash_gcf(size, name, r, err, errlen); break;
        case IMG_NRF: ok = flash_nrf(size, r, err, errlen); break;
        case IMG_TI: ok = xf_ti_flash(rd, size, name, r, err, errlen); break;
        default:
            snprintf(err, errlen, "not a firmware file the bridge knows");
            ok = false;
    }
    s_progress = NULL;
    return ok;
}

#include "logstore.h"
#include "tasks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_app_desc.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "logstore_core.h"
#include "nvs.h"
#include "sdlog.h"
#include "spi_flash_chip_driver.h"
#include "watchdog.h"

static const char *TAG = "logstore";

#define MB (1024u * 1024u)
#define BUILT_FOR (16 * MB)            // CONFIG_ESPTOOLPY_FLASHSIZE_16MB
#define LOW_OFFSET 0xA20000u           // after dongle_fw (partitions.csv "zblog")
#define LOW_SIZE (2 * MB)
#define AREA_LABEL "zblog"
#define AREA_SUBTYPE 0x41
#define FLUSH_MS 2000
#define CLOCK_WAIT_S 120  // hold the first lines this long for the clock, so they get real times

static const esp_partition_t *s_part;
static SemaphoreHandle_t s_lock;
static ls_ring_t s_ring;
static bool s_ok;
static char s_where[80] = "not started";
static uint32_t s_phys, s_chip_id;
static uint32_t s_boot_no;
static int64_t s_boot_epoch_ms;
static bool s_clock_wait = true;
static uint32_t s_last_ms;
static uint32_t s_write_errors;

typedef struct {
    uint32_t offset, floor;
} floor_rec_t;

// ---------------------------------------------------------------- flash area

static int f_read(void *ctx, uint32_t off, void *buf, size_t n) { return esp_partition_read(ctx, off, buf, n) == ESP_OK ? 0 : -1; }
static int f_write(void *ctx, uint32_t off, const void *buf, size_t n) {
    return esp_partition_write(ctx, off, buf, n) == ESP_OK ? 0 : -1;
}
static int f_erase(void *ctx, uint32_t off, size_t n) { return esp_partition_erase_range(ctx, off, n) == ESP_OK ? 0 : -1; }

// The firmware is built for 16 MB (so the same image runs on any board), and ESP-IDF limits itself to that.
// A bigger chip that the driver can address with 4-byte commands gets its upper part used for the log, after
// proving that a write up there lands up there and not on the same spot in the lower 16 MB (a chip that
// ignores the top address bits would otherwise overwrite the firmware). The probe sector is the last one, whose
// lower twin (0xFFF000) is unused flash.
static bool upper_usable(esp_flash_t *ch, uint32_t phys) {
    uint32_t probe = phys - LS_SECTOR, twin = probe - BUILT_FOR;
    uint8_t pat[32], back[32], twin_before[32], twin_after[32];
    esp_fill_random(pat, sizeof(pat));
    if (esp_flash_read(ch, twin_before, twin, sizeof(twin_before)) != ESP_OK) return false;
    if (esp_flash_erase_region(ch, probe, LS_SECTOR) != ESP_OK) return false;
    if (esp_flash_write(ch, pat, probe, sizeof(pat)) != ESP_OK) return false;
    if (esp_flash_read(ch, back, probe, sizeof(back)) != ESP_OK || memcmp(back, pat, sizeof(pat)) != 0) {
        ESP_LOGW(TAG, "Flash above 16 MB doesn't read back what was written");
        return false;
    }
    if (esp_flash_read(ch, twin_after, twin, sizeof(twin_after)) != ESP_OK || memcmp(twin_after, twin_before, sizeof(twin_after)) != 0) {
        ESP_LOGW(TAG, "Flash above 16 MB wraps onto the lower 16 MB: not used");
        return false;
    }
    esp_flash_erase_region(ch, probe, LS_SECTOR);
    return true;
}

static void pick_area(void) {
    esp_flash_t *ch = esp_flash_default_chip;
    esp_flash_read_id(ch, &s_chip_id);
    if (esp_flash_get_physical_size(ch, &s_phys) != ESP_OK) s_phys = 0;
    bool addr32 = ch->chip_drv->get_chip_caps && (ch->chip_drv->get_chip_caps(ch) & SPI_FLASH_CHIP_CAP_32MB_SUPPORT);
    ESP_LOGI(TAG, "Flash chip %06lX: %lu MB (firmware built for %u MB), addresses above 16 MB: %s", (unsigned long)s_chip_id,
             (unsigned long)(s_phys / MB), BUILT_FOR / MB, addr32 ? "yes" : "no");
    if (s_phys > BUILT_FOR && ch->size == BUILT_FOR && addr32) {
        ch->size = s_phys;  // lets esp_flash and esp_partition reach it; checked by the probe right after
        if (upper_usable(ch, s_phys)) {
            uint32_t size = s_phys - BUILT_FOR - LS_SECTOR;
            if (esp_partition_register_external(ch, BUILT_FOR, size, AREA_LABEL "_hi", ESP_PARTITION_TYPE_DATA, AREA_SUBTYPE,
                                                &s_part) == ESP_OK) {
                snprintf(s_where, sizeof(s_where), "upper %lu MB of the %lu MB flash chip", (unsigned long)((size + LS_SECTOR) / MB),
                         (unsigned long)(s_phys / MB));
                return;
            }
            s_part = NULL;
        }
        ch->size = BUILT_FOR;
    }
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, AREA_SUBTYPE, AREA_LABEL);
    if (!s_part &&
        esp_partition_register_external(NULL, LOW_OFFSET, LOW_SIZE, AREA_LABEL, ESP_PARTITION_TYPE_DATA, AREA_SUBTYPE, &s_part) != ESP_OK)
        s_part = NULL;
    if (s_part) snprintf(s_where, sizeof(s_where), "%lu MB area of the %lu MB flash chip", (unsigned long)(s_part->size / MB),
                         (unsigned long)(s_phys / MB));
}

static uint32_t load_floor(uint32_t offset) {
    nvs_handle_t h;
    floor_rec_t f = {0};
    size_t len = sizeof(f);
    if (nvs_open("logs", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "floor", &f, &len) != ESP_OK || len != sizeof(f)) f.offset = 0;
        nvs_close(h);
    }
    return f.offset == offset ? f.floor : 0;
}

static void save_floor(uint32_t offset, uint32_t floor) {
    nvs_handle_t h;
    floor_rec_t f = {offset, floor};
    if (nvs_open("logs", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "floor", &f, sizeof(f));
    nvs_commit(h);
    nvs_close(h);
}

static uint32_t next_boot_no(void) {
    nvs_handle_t h;
    uint32_t n = 0;
    if (nvs_open("logs", NVS_READWRITE, &h) != ESP_OK) return 0;
    nvs_get_u32(h, "boot", &n);
    n++;
    nvs_set_u32(h, "boot", n);
    nvs_commit(h);
    nvs_close(h);
    return n;
}

// ---------------------------------------------------------------- writing

static int64_t epoch_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec > 1700000000 ? (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 : 0;
}

// To flash and card. day_ms = when the text is from (picks the card's day file).
static void put(const char *t, size_t n, int64_t day_ms) {
    if (!n) return;
    if (s_ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ls_append(&s_ring, t, n) != 0) s_write_errors++;
        xSemaphoreGive(s_lock);
    }
    sdlog_append(t, n, day_ms, s_boot_no);
}

static void note(int64_t boot_ms, uint32_t ms, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void note(int64_t boot_ms, uint32_t ms, const char *fmt, ...) {
    char text[200], line[240];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    size_t n = ls_note(boot_ms, ms, text, line, sizeof(line));
    put(line, n, boot_ms > 0 ? boot_ms + ms : 0);
}

// Stamps and stores text from one boot. Complete lines only, unless `all` (then a missing last '\n' is added).
// Returns the input bytes consumed.
static size_t store_lines(const char *in, size_t n, int64_t boot_ms, uint32_t *last_ms, char *out, size_t cap, bool all) {
    size_t done = 0;
    while (done < n) {
        size_t used;
        size_t k = ls_stamp_lines(in + done, n - done, boot_ms, last_ms, out, cap, &used);
        if (used == 0) {
            // No newline in what's left. Normal at the end (the rest of the line comes later), but a long
            // run without one (cut-off output) is stored as it is so it can't block the log.
            size_t rest = n - done;
            if (!all && rest < 1024) break;
            if (rest > cap - 40) rest = cap - 40;
            char *tmp = malloc(rest + 1);
            if (!tmp) break;
            memcpy(tmp, in + done, rest);
            tmp[rest] = '\n';
            k = ls_stamp_lines(tmp, rest + 1, boot_ms, last_ms, out, cap, &used);
            free(tmp);
            used = rest;
        }
        put(out, k, boot_ms > 0 ? boot_ms + *last_ms : 0);
        done += used;
    }
    return done;
}

static unsigned first_ms(const char *t, size_t n) {
    unsigned ms = 0;
    char head[24];
    size_t k = n < sizeof(head) - 1 ? n : sizeof(head) - 1;
    memcpy(head, t, k);
    head[k] = 0;
    if (sscanf(head, "%*c (%u)", &ms) != 1) ms = 0;
    return ms;
}

static const char *reset_text(void) {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_EXT: return "reset pin";
        case ESP_RST_SW: return "software restart";
        case ESP_RST_PANIC: return "CRASH (panic)";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout (power dip)";
        default: return "unknown";
    }
}

static void writer_task(void *arg) {
    (void)arg;
    char *in = malloc(4096), *out = malloc(8192);
    if (!in || !out) {
        ESP_LOGE(TAG, "No memory for the log writer");
        free(in);
        free(out);
        vTaskDelete(NULL);
    }
    // Wait (a while) for the clock, so this boot's first lines get real times.
    for (int s = 0; s < CLOCK_WAIT_S && !epoch_ms(); s++) {
        sdlog_poll();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    int64_t now = epoch_ms();
    if (now) {
        s_boot_epoch_ms = now - (int64_t)esp_log_timestamp();
        s_clock_wait = false;
    }
    logbuf_set_boot(s_boot_no, s_boot_epoch_ms);

    // The previous boot's last lines that were still in memory when it reset (a crash, a watchdog, a restart).
    char *prev;
    int64_t prev_epoch;
    uint32_t prev_boot, lost;
    size_t pn = logbuf_take_prev(&prev, &prev_epoch, &prev_boot, &lost);
    if (pn) {
        uint32_t ms = first_ms(prev, pn), last = ms;
        if (lost) note(prev_epoch, ms, "Boot %lu: %lu bytes before this were never saved", (unsigned long)prev_boot, (unsigned long)lost);
        note(prev_epoch, ms, "Boot %lu: its last lines, kept in memory through the reset (not saved before it)", (unsigned long)prev_boot);
        store_lines(prev, pn, prev_epoch, &last, out, 8192, true);
        free(prev);
    }
    const char *wd = watchdog_last_reason();
    note(s_boot_epoch_ms, 0, "Boot %lu: firmware %s, reset: %s%s%s%s", (unsigned long)s_boot_no, esp_app_get_description()->version,
         reset_text(), wd[0] ? " (" : "", wd, wd[0] ? ")" : "");
    if (s_clock_wait) note(0, esp_log_timestamp(), "Clock not set yet: times are seconds since boot until it is");
    ESP_LOGI(TAG, "Stored log in the %s: %lu KB used, boot %lu", s_where, (unsigned long)(ls_used(&s_ring) / 1024), (unsigned long)s_boot_no);

    TickType_t last_flush = xTaskGetTickCount();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (s_clock_wait && (now = epoch_ms()) != 0) {
            s_boot_epoch_ms = now - (int64_t)esp_log_timestamp();
            s_clock_wait = false;
            logbuf_set_boot(s_boot_no, s_boot_epoch_ms);
            note(s_boot_epoch_ms, esp_log_timestamp(), "Clock set");
        }
        uint32_t from = logbuf_flushed(), total = logbuf_total();
        bool due = xTaskGetTickCount() - last_flush >= pdMS_TO_TICKS(FLUSH_MS);
        if (total - from < 8192 && !due) continue;
        sdlog_poll();
        last_flush = xTaskGetTickCount();
        if (total - from > LOGBUF_SIZE) {
            uint32_t gone = total - from - LOGBUF_SIZE;
            from = total - LOGBUF_SIZE;
            note(s_boot_epoch_ms, s_last_ms, "%lu bytes of log were lost (written faster than they could be saved)", (unsigned long)gone);
        }
        while (from < total) {
            uint32_t next;
            size_t n = logbuf_read(from, in, 4096, &next);
            if (!n) break;
            size_t used = store_lines(in, n, s_boot_epoch_ms, &s_last_ms, out, 8192, false);
            if (!used) break;  // a partial line: the rest comes later
            from += used;
            logbuf_set_flushed(from);
        }
    }
}

// ---------------------------------------------------------------- start + API

void logstore_start(void) {
    s_lock = xSemaphoreCreateMutex();
    s_boot_no = next_boot_no();
    pick_area();
    if (!s_part) {
        ESP_LOGE(TAG, "No flash area for the stored log");
        snprintf(s_where, sizeof(s_where), "no flash area");
    } else {
        ls_flash_t fl = {f_read, f_write, f_erase, (void *)s_part};
        int64_t t0 = esp_timer_get_time();
        s_ok = ls_mount(&s_ring, &fl, s_part->size, load_floor(s_part->address)) == 0;
        ESP_LOGI(TAG, "Log area at 0x%lx, %lu KB, scanned in %lld ms", (unsigned long)s_part->address,
                 (unsigned long)(s_part->size / 1024), (esp_timer_get_time() - t0) / 1000);
    }
    // Without a flash area the writer still feeds the card (put() skips the flash part).
    task_start(writer_task, "logstore", 6144, NULL, 2, false);  // writes the log partition
}

// The stamp of the first stamped line in the oldest stored text (0 if none).
static int64_t oldest_stamp(void) {
    char buf[600];
    ls_cursor_t c;
    ls_cursor_oldest(&s_ring, &c);
    size_t n = ls_read(&s_ring, &c, buf, sizeof(buf) - 1);
    for (size_t i = 0; i < n; i++) {
        if (i == 0 || buf[i - 1] == '\n') {
            int64_t t = ls_parse_stamp(buf + i, n - i);
            if (t) return t;
        }
    }
    return 0;
}

void logstore_add_json(cJSON *root) {
    cJSON *f = cJSON_AddObjectToObject(root, "flash");
    cJSON_AddBoolToObject(f, "ok", s_ok);
    cJSON_AddStringToObject(f, "where", s_where);
    char id[8];
    snprintf(id, sizeof(id), "%06lX", (unsigned long)s_chip_id);
    cJSON_AddStringToObject(f, "chip_id", id);
    cJSON_AddNumberToObject(f, "chip_size", s_phys);
    cJSON_AddNumberToObject(f, "boot", s_boot_no);
    cJSON_AddBoolToObject(f, "clock_wait", s_clock_wait);
    if (s_ok) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        cJSON_AddNumberToObject(f, "offset", s_part->address);
        cJSON_AddNumberToObject(f, "size", s_ring.sectors * LS_TEXT);
        cJSON_AddNumberToObject(f, "used", ls_used(&s_ring));
        cJSON_AddNumberToObject(f, "oldest", (double)oldest_stamp());
        cJSON_AddNumberToObject(f, "write_errors", s_write_errors);
        xSemaphoreGive(s_lock);
        cJSON_AddNumberToObject(f, "pending", logbuf_total() - logbuf_flushed());
    }
    sdlog_add_json(root);
}

esp_err_t logstore_send(httpd_req_t *req, uint32_t tail, bool download) {
    if (!s_ok) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "the stored log isn't running");
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (download) httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"zigbee-bridge-log.txt\"");
    char *buf = malloc(4096);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    ls_cursor_t c;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (tail) ls_cursor_tail(&s_ring, tail, &c);
    else ls_cursor_oldest(&s_ring, &c);
    xSemaphoreGive(s_lock);
    esp_err_t e = ESP_OK;
    bool first = true;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t n = ls_read(&s_ring, &c, buf, 4096);
        xSemaphoreGive(s_lock);
        if (!n) break;
        size_t skip = 0;
        if (first && tail) {  // the tail starts mid-sector: begin at a whole line
            const char *nl = memchr(buf, '\n', n);
            skip = nl ? (size_t)(nl - buf) + 1 : n;
        }
        first = false;
        if (n > skip && (e = httpd_resp_send_chunk(req, buf + skip, n - skip)) != ESP_OK) break;
    }
    free(buf);
    if (e == ESP_OK) e = httpd_resp_send_chunk(req, NULL, 0);
    return e;
}

bool logstore_clear(void) {
    if (!s_ok) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t floor = ls_clear(&s_ring);
    xSemaphoreGive(s_lock);
    save_floor(s_part->address, floor);
    ESP_LOGW(TAG, "Stored log cleared from the dashboard");
    return true;
}

#include "sdlog.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sd_protocol_defs.h"
#include "sdmmc_cmd.h"
#include "logstore_core.h"

static const char *TAG = "sdcard";

#define MOUNT "/sd"
#define DIR_PATH MOUNT "/zblog"
#define PIN_POWER GPIO_NUM_45  // P-MOSFET gate: low = card powered
#define LDO_CHAN 4             // feeds VDDPST_5, the I/O bank of GPIO39-48
#define RETRY_S 30
#define CHECK_S 30
#define SPACE_CHECK_S 600
#define MAX_FILES_LISTED 60

typedef enum { SD_NONE, SD_OK, SD_ERROR } sd_state_t;

static SemaphoreHandle_t s_lock;
static sd_state_t s_state = SD_NONE;
static char s_detail[96] = "Looking for a card";
static sdmmc_card_t *s_card;
static sd_pwr_ctrl_handle_t s_ldo;
static uint32_t s_next_try, s_next_check, s_next_space;
static bool s_powered, s_cycle_power;
static uint64_t s_written;  // bytes written since the card was mounted
static uint32_t s_mounted_at;

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

static const char *const NOISY[] = {"sdmmc_init", "sdmmc_common", "sdmmc_sd", "sdmmc_cmd", "sdmmc_req", "sdmmc_periph",
                                    "vfs_fat_sdmmc", "vfs_fat"};
// Looking for a card every 30 s with no card in would fill the log with the driver's errors.
static void driver_logs(esp_log_level_t lv) {
    for (size_t i = 0; i < sizeof(NOISY) / sizeof(NOISY[0]); i++) esp_log_level_set(NOISY[i], lv);
}

static void power(bool on) {
    if (!s_powered) {
        gpio_config_t g = {.pin_bit_mask = 1ULL << PIN_POWER, .mode = GPIO_MODE_OUTPUT};
        gpio_config(&g);
        s_powered = true;
    }
    gpio_set_level(PIN_POWER, on ? 0 : 1);
}

static void unmount(void) {
    if (s_card) esp_vfs_fat_sdcard_unmount(MOUNT, s_card);
    s_card = NULL;
    driver_logs(ESP_LOG_NONE);
}

static void lost(const char *why) {
    ESP_LOGW(TAG, "%s: card no longer used", why);
    unmount();
    s_state = SD_NONE;
    snprintf(s_detail, sizeof(s_detail), "%s", why);
    s_cycle_power = true;
    s_next_try = now_s() + RETRY_S;
}

static void try_mount(void) {
    if (!s_ldo) {
        sd_pwr_ctrl_ldo_config_t c = {.ldo_chan_id = LDO_CHAN};
        if (sd_pwr_ctrl_new_on_chip_ldo(&c, &s_ldo) != ESP_OK) {
            s_state = SD_ERROR;
            snprintf(s_detail, sizeof(s_detail), "Couldn't power the card slot's pins (LDO %d)", LDO_CHAN);
            s_ldo = NULL;
            return;
        }
    }
    if (s_cycle_power) {  // a card that stopped answering gets a clean power-up
        power(false);
        vTaskDelay(pdMS_TO_TICKS(250));
        s_cycle_power = false;
    }
    power(true);
    vTaskDelay(pdMS_TO_TICKS(20));

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.pwr_ctrl_handle = s_ldo;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = GPIO_NUM_43;
    slot.cmd = GPIO_NUM_44;
    slot.d0 = GPIO_NUM_39;
    slot.d1 = GPIO_NUM_40;
    slot.d2 = GPIO_NUM_41;
    slot.d3 = GPIO_NUM_42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t mc = {.format_if_mount_failed = false, .max_files = 3, .allocation_unit_size = 16 * 1024};

    driver_logs(ESP_LOG_NONE);
    esp_err_t e = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mc, &s_card);
    if (e != ESP_OK) {
        s_card = NULL;
        sd_state_t was = s_state;
        if (e == ESP_FAIL) {
            s_state = SD_ERROR;
            snprintf(s_detail, sizeof(s_detail), "The card isn't FAT32: format it as FAT32 on a computer");
        } else {
            s_state = SD_NONE;
            snprintf(s_detail, sizeof(s_detail), "No card");
        }
        if (was != s_state || (s_state == SD_ERROR && was == SD_NONE)) ESP_LOGI(TAG, "%s", s_detail);
        s_next_try = now_s() + RETRY_S;
        return;
    }
    driver_logs(ESP_LOG_WARN);
    mkdir(DIR_PATH, 0777);
    struct stat st;
    if (stat(DIR_PATH, &st) != 0 || !S_ISDIR(st.st_mode)) {
        unmount();
        s_state = SD_ERROR;
        snprintf(s_detail, sizeof(s_detail), "Couldn't make the zblog folder on the card (write-protected?)");
        s_next_try = now_s() + RETRY_S;
        ESP_LOGW(TAG, "%s", s_detail);
        return;
    }
    s_state = SD_OK;
    s_written = 0;
    s_mounted_at = now_s();
    s_next_check = now_s() + CHECK_S;
    s_next_space = 0;
    uint64_t bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
    snprintf(s_detail, sizeof(s_detail), "%s %.1f GB", (s_card->ocr & SD_OCR_SDHC_CAP) ? "SDHC/SDXC" : "SDSC", bytes / 1e9);
    ESP_LOGI(TAG, "Card in: %s '%s', %lu kHz, logging to " DIR_PATH, s_detail, s_card->cid.name,
             (unsigned long)s_card->real_freq_khz);
}

// Day files, newest first. Returns how many names were put in out (each up to 31 chars).
static int list_files(char (*out)[32], int max, uint64_t *total_bytes, int *count) {
    DIR *d = opendir(DIR_PATH);
    int n = 0;
    *total_bytes = 0;
    *count = 0;
    if (!d) return 0;
    struct dirent *e;
    char path[300];
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5 || len > 31 || strcmp(e->d_name + len - 4, ".log") != 0) continue;
        (*count)++;
        struct stat st;
        snprintf(path, sizeof(path), DIR_PATH "/%s", e->d_name);
        if (stat(path, &st) == 0) *total_bytes += st.st_size;
        // keep the `max` newest (names sort by date; boot-N files after them)
        int pos = n;
        while (pos > 0 && strcmp(out[pos - 1], e->d_name) < 0) pos--;
        if (pos >= max) continue;
        int last = n < max ? n : max - 1;
        for (int i = last; i > pos; i--) memcpy(out[i], out[i - 1], 32);
        memcpy(out[pos], e->d_name, len + 1);
        if (n < max) n++;
    }
    closedir(d);
    return n;
}

// Deletes the oldest day file while the card is under 5% (or 64 MB) free.
static void make_room(void) {
    for (int guard = 0; guard < 50; guard++) {
        uint64_t total = 0, free_b = 0;
        if (esp_vfs_fat_info(MOUNT, &total, &free_b) != ESP_OK || total == 0) return;
        if (free_b >= total / 20 && free_b >= 64ull * 1024 * 1024) return;
        DIR *d = opendir(DIR_PATH);
        if (!d) return;
        char oldest[32] = "";
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t len = strlen(e->d_name);
            if (len != 14 || strcmp(e->d_name + 10, ".log") != 0) continue;  // YYYY-MM-DD.log only
            if (!oldest[0] || strcmp(e->d_name, oldest) < 0) memcpy(oldest, e->d_name, len + 1);
        }
        closedir(d);
        if (!oldest[0]) return;
        char path[300];
        snprintf(path, sizeof(path), DIR_PATH "/%s", oldest);
        ESP_LOGW(TAG, "Card nearly full (%llu MB free): deleting %s", (unsigned long long)(free_b >> 20), oldest);
        if (unlink(path) != 0) return;
    }
}

static void ensure_lock(void) {
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

void sdlog_poll(void) {
    ensure_lock();
    uint32_t now = now_s();
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100)) != pdTRUE) return;
    if (s_state != SD_OK) {
        if (now >= s_next_try) try_mount();
    } else {
        if (now >= s_next_check) {
            s_next_check = now + CHECK_S;
            if (sdmmc_get_status(s_card) != ESP_OK) lost("Card removed or not answering");
        }
        if (s_state == SD_OK && now >= s_next_space) {
            s_next_space = now + SPACE_CHECK_S;
            make_room();
        }
    }
    xSemaphoreGive(s_lock);
}

void sdlog_append(const char *text, size_t n, int64_t epoch_ms, uint32_t boot_no) {
    if (s_state != SD_OK || !n) return;
    ensure_lock();
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(15000)) != pdTRUE) return;
    if (s_state == SD_OK) {
        char path[300];
        if (epoch_ms > 0) {
            char st[32];
            ls_stamp(epoch_ms, 0, st);  // "2026-11-24T..." -> the date part
            snprintf(path, sizeof(path), DIR_PATH "/%.10s.log", st);
        } else {
            snprintf(path, sizeof(path), DIR_PATH "/boot-%lu.log", (unsigned long)boot_no);
        }
        FILE *f = fopen(path, "a");
        bool ok = f != NULL;
        if (ok) ok = fwrite(text, 1, n, f) == n;
        if (ok) ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
        if (f) fclose(f);
        if (ok) s_written += n;
        else lost("Writing to the card failed (removed?)");
    }
    xSemaphoreGive(s_lock);
}

void sdlog_add_json(cJSON *root) {
    ensure_lock();
    cJSON *sd = cJSON_AddObjectToObject(root, "sd");
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        cJSON_AddStringToObject(sd, "state", "busy");
        return;
    }
    cJSON_AddStringToObject(sd, "state", s_state == SD_OK ? "ok" : s_state == SD_ERROR ? "error" : "none");
    cJSON_AddStringToObject(sd, "detail", s_detail);
    if (s_state == SD_OK) {
        uint64_t total = 0, free_b = 0;
        esp_vfs_fat_info(MOUNT, &total, &free_b);
        cJSON_AddNumberToObject(sd, "total", (double)total);
        cJSON_AddNumberToObject(sd, "free", (double)free_b);
        cJSON_AddStringToObject(sd, "card", s_card->cid.name);
        cJSON_AddNumberToObject(sd, "khz", s_card->real_freq_khz);
        cJSON_AddNumberToObject(sd, "written", (double)s_written);
        cJSON_AddNumberToObject(sd, "mounted_s", now_s() - s_mounted_at);
        static char names[MAX_FILES_LISTED][32];
        uint64_t bytes;
        int count;
        int n = list_files(names, MAX_FILES_LISTED, &bytes, &count);
        cJSON_AddNumberToObject(sd, "file_count", count);
        cJSON_AddNumberToObject(sd, "file_bytes", (double)bytes);
        cJSON *arr = cJSON_AddArrayToObject(sd, "files");
        char path[300];
        for (int i = 0; i < n; i++) {
            struct stat st;
            snprintf(path, sizeof(path), DIR_PATH "/%s", names[i]);
            cJSON *f = cJSON_CreateObject();
            cJSON_AddStringToObject(f, "name", names[i]);
            cJSON_AddNumberToObject(f, "size", stat(path, &st) == 0 ? (double)st.st_size : 0);
            cJSON_AddItemToArray(arr, f);
        }
    } else {
        cJSON_AddNumberToObject(sd, "retry_in", s_next_try > now_s() ? s_next_try - now_s() : 0);
    }
    xSemaphoreGive(s_lock);
}

static bool name_ok(const char *s) {
    size_t n = strlen(s);
    if (n < 5 || n > 31 || s[0] == '.' || strcmp(s + n - 4, ".log") != 0) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == '_'))
            return false;
    }
    return true;
}

esp_err_t sdlog_send_file(httpd_req_t *req, const char *name, bool download) {
    static char disp[96];  // httpd keeps header pointers until the response is sent
    ensure_lock();
    if (!name_ok(name)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "bad file name");
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5000)) != pdTRUE) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "card busy");
    }
    esp_err_t e;
    char path[300];
    snprintf(path, sizeof(path), DIR_PATH "/%s", name);
    FILE *f = s_state == SD_OK ? fopen(path, "r") : NULL;
    if (!f) {
        xSemaphoreGive(s_lock);
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, s_state == SD_OK ? "no such file" : "no card");
    }
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (download) {
        snprintf(disp, sizeof(disp), "attachment; filename=\"zigbee-bridge-%s\"", name);
        httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }
    char *buf = malloc(4096);
    e = buf ? ESP_OK : ESP_ERR_NO_MEM;
    size_t k;
    while (e == ESP_OK && (k = fread(buf, 1, 4096, f)) > 0) e = httpd_resp_send_chunk(req, buf, k);
    fclose(f);
    free(buf);
    xSemaphoreGive(s_lock);
    if (e == ESP_OK) e = httpd_resp_send_chunk(req, NULL, 0);
    return e;
}

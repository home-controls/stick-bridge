#include "updater.h"
#include "tasks.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fwcheck.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha256.h"
#include "net.h"
#include "nvs.h"
#include "settings.h"
#include "stats.h"

static const char *TAG = "updater";

// Test builds can point at a local release server (tools/ota_test_server.py) that serves broken
// releases on purpose: checked every 20 s and installed without a click.
#if defined(CONFIG_BRIDGE_TEST_HOOKS) && defined(CONFIG_BRIDGE_TEST_UPDATE_URL)
#define TEST_URL CONFIG_BRIDGE_TEST_UPDATE_URL
#else
#define TEST_URL ""
#endif
#define TEST_LOOP (TEST_URL[0] != 0)

// Before main.c confirms a new firmware (30 s): one that crashes while checking still rolls back.
#define FIRST_CHECK_S 15
#define CHECK_EVERY_S (TEST_LOOP ? 20 : 6 * 3600)
#define MANIFEST_MAX 8192
// A whole download, however slowly the server drips it (each read also times out after 15 s).
#define DOWNLOAD_DEADLINE_S (TEST_LOOP ? 40 : 300)

typedef enum { UP_IDLE, UP_CHECKING, UP_UPTODATE, UP_AVAILABLE, UP_DOWNLOADING, UP_REBOOTING, UP_ERROR } up_state_t;
static const char *const STATE_NAMES[] = {"idle", "checking", "uptodate", "available", "downloading", "rebooting", "error"};

typedef struct {
    up_state_t state;
    char latest[33];
    char notes[600];
    char date[24];
    char url[256];      // the image, resolved against the manifest's URL
    char sha256[65];    // lowercase hex
    char md5[33];       // lowercase hex
    int size;
    uint32_t checked_up_s;
    int64_t checked_epoch;
    char error[120];
    int done;           // bytes downloaded
} up_t;

static up_t s_up;
// A firmware that was installed but didn't start properly, so the bootloader went back (or "").
static char s_rolled_back[33];
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
#define NOTIFY_CHECK BIT0
#define NOTIFY_INSTALL BIT1

static int64_t epoch_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec > 1700000000 ? tv.tv_sec : 0;
}

static void set_error(const char *fmt, const char *detail) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_up.state = UP_ERROR;
    snprintf(s_up.error, sizeof(s_up.error), fmt, detail ? detail : "");
    ESP_LOGW(TAG, "%s", s_up.error);
    xSemaphoreGive(s_lock);
}

static void hex(const uint8_t *in, int n, char *out) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2] = h[in[i] >> 4];
        out[i * 2 + 1] = h[in[i] & 15];
    }
    out[n * 2] = 0;
}

static bool is_hex(const char *s, size_t n) {
    if (strlen(s) != n) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

static const char *update_url(const bridge_settings_t *cfg) { return TEST_LOOP ? TEST_URL : cfg->update_url; }

// GitHub release downloads redirect twice, the second time to a signed release-assets.githubusercontent.com
// address of ~900 characters: the address buffer and the header buffers must hold it (a 256-byte buffer cut it
// short, and the cut request came back as a bogus status).
#define URL_MAX 2048

// Redirects follow the Location header exactly as the server sent it. esp_http_client_get_url() rebuilds an address
// from scheme, host, port and path only and drops the query string, and GitHub's last hop is a signed
// release-assets address whose signature is the query: without it the asset store answers HTTP 618 (what the
// bridge kept seeing, while the same address worked from a browser).
typedef struct {
    char *buf;
    size_t cap;
} location_t;

static esp_err_t on_http_event(esp_http_client_event_t *e) {
    location_t *l = e->user_data;
    if (l && e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value && strcasecmp(e->header_key, "Location") == 0)
        strlcpy(l->buf, e->header_value, l->cap);
    return ESP_OK;
}

// The next address from a Location value: absolute as is, or "/path" on the current scheme and host. false when it is
// neither or doesn't fit.
static bool next_hop(const char *loc, char *url, size_t cap) {
    if (strncmp(loc, "https://", 8) == 0 || strncmp(loc, "http://", 7) == 0) return strlcpy(url, loc, cap) < cap;
    if (loc[0] != '/') return false;
    const char *s = strstr(url, "://");
    const char *end = s ? strchr(s + 3, '/') : NULL;
    size_t base = end ? (size_t)(end - url) : strlen(url);
    if (base + strlen(loc) + 1 > cap) return false;
    strcpy(url + base, loc);
    return true;
}

// Opens url and reads the response headers, following up to 4 redirects. Returns the client (the
// caller cleans it up) with *status set, or NULL with *why set. Each hop gets a fresh client: one
// reused after esp_http_client_set_redirection() read the new body wrongly.
static esp_http_client_handle_t open_url(const char *url, int *status, int64_t *len, const char **why) {
    char *next = malloc(URL_MAX);
    location_t loc = {.buf = malloc(URL_MAX), .cap = URL_MAX};
    if (!next || !loc.buf) {
        free(next);
        free(loc.buf);
        *why = "out of memory";
        return NULL;
    }
    strlcpy(next, url, URL_MAX);
    *status = 0;
    *len = -1;
    esp_http_client_handle_t ret = NULL;
    for (int hop = 0;; hop++) {
        esp_http_client_config_t cfg = {
            .url = next,
            .timeout_ms = 15000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .user_agent = "zigbee-eth-bridge",
            .buffer_size = 4096,     // a long Location header
            .buffer_size_tx = 2048,  // the long request line after the redirect
            .disable_auto_redirect = true,
            .event_handler = on_http_event,
            .user_data = &loc,
        };
        loc.buf[0] = 0;
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) {
            *why = "out of memory";
            break;
        }
        if (esp_http_client_open(c, 0) != ESP_OK) {
            *why = "can't connect";
        } else {
            *len = esp_http_client_fetch_headers(c);
            *status = esp_http_client_get_status_code(c);
            const char *h = strstr(next, "://");
            ESP_LOGI(TAG, "hop %d: %.40s -> HTTP %d, length %lld", hop + 1, h ? h + 3 : next, *status, (long long)*len);
            bool redirect = *status == 301 || *status == 302 || *status == 303 || *status == 307 || *status == 308;
            if (*status == 0 || *len < -1) *why = "no answer";  // -1 is a chunked response (no length), not an error
            else if (!redirect) {
                ret = c;
                break;
            } else if (hop >= 4) *why = "too many redirects";
            else if (!loc.buf[0] || !next_hop(loc.buf, next, URL_MAX))
                *why = "bad redirect";
            else if (!TEST_LOOP && strncmp(next, "https://", 8) != 0)
                *why = "redirected to a plain http address";  // the whole chain stays https (test builds' local server aside)
            else {
                esp_http_client_cleanup(c);
                continue;  // next hop, new connection
            }
        }
        esp_http_client_cleanup(c);
        break;
    }
    free(next);
    free(loc.buf);
    return ret;
}

// GitHub's asset store now and then answers a fresh release with a server error (seen: "HTTP 618" right after a
// release was published) and a moment later serves it fine. One more try after 2 s, from the start of the
// redirect chain (a new signed address), and what the server said goes to the log.
static void log_error_body(esp_http_client_handle_t c, int status, const char *url) {
    char eb[160];
    int er = esp_http_client_read(c, eb, sizeof(eb) - 1);
    eb[er > 0 ? er : 0] = 0;
    for (char *q = eb; *q; q++)
        if (*q < 0x20 || *q > 0x7E) *q = ' ';
    const char *h = strstr(url, "://");
    ESP_LOGW(TAG, "%.48s answered HTTP %d: %s", h ? h + 3 : url, status, eb);
}

static esp_http_client_handle_t open_url_retry(const char *url, int *status, int64_t *len, const char **why) {
    for (int attempt = 0;; attempt++) {
        esp_http_client_handle_t c = open_url(url, status, len, why);
        bool server_trouble = !c || *status >= 500;
        if (c && *status >= 400) log_error_body(c, *status, url);
        if (!server_trouble || attempt >= 1) return c;
        if (c) esp_http_client_cleanup(c);
        ESP_LOGW(TAG, "%s: trying once more in 2 s", c ? "server error" : *why ? *why : "no connection");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

// ---------------------------------------------------------------- check

static void do_install(void);

static void do_check(void) {
    bridge_settings_t cfg;
    settings_get(&cfg);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_up.state == UP_DOWNLOADING || s_up.state == UP_REBOOTING) {
        xSemaphoreGive(s_lock);
        return;
    }
    s_up.state = UP_CHECKING;
    s_up.error[0] = 0;
    xSemaphoreGive(s_lock);

    char *body = malloc(MANIFEST_MAX + 1);
    if (!body) return set_error("Update check failed: out of memory%s", NULL);
    int status = 0, n = 0;
    int64_t len;
    const char *why = NULL;
    esp_http_client_handle_t c = open_url_retry(update_url(&cfg), &status, &len, &why);
    if (c) {
        while (n < MANIFEST_MAX) {
            int r = esp_http_client_read(c, body + n, MANIFEST_MAX - n);
            if (r <= 0) break;
            n += r;
        }
        esp_http_client_cleanup(c);
    }
    if (!c || status != 200) {
        free(body);
        char w[48];
        if (c) snprintf(w, sizeof(w), "HTTP %d", status);
        else strlcpy(w, why, sizeof(w));
        return set_error("Update check failed: %s", w);
    }
    body[n] = 0;
    cJSON *j = cJSON_Parse(body);
    free(body);
    const cJSON *ver = cJSON_GetObjectItem(j, "version"), *file = cJSON_GetObjectItem(j, "file"),
                *sha = cJSON_GetObjectItem(j, "sha256"), *md5 = cJSON_GetObjectItem(j, "md5"),
                *size = cJSON_GetObjectItem(j, "size"), *notes = cJSON_GetObjectItem(j, "notes"),
                *date = cJSON_GetObjectItem(j, "date");
    if (!cJSON_IsString(ver) || !cJSON_IsString(file) || !cJSON_IsString(sha) || !cJSON_IsString(md5) ||
        !cJSON_IsNumber(size) || !is_hex(sha->valuestring, 64) || !is_hex(md5->valuestring, 32) ||
        size->valuedouble < 1024 || size->valuedouble > 4 * 1024 * 1024 || strlen(ver->valuestring) >= sizeof(s_up.latest) ||
        strlen(file->valuestring) >= sizeof(s_up.url) - 64) {
        cJSON_Delete(j);
        return set_error("Update check failed: the release manifest is incomplete%s", NULL);
    }
    char url[256];
    if (strncmp(file->valuestring, "http://", 7) == 0 && !TEST_LOOP) {
        cJSON_Delete(j);
        return set_error("Update check failed: the release manifest points at a plain http address%s", NULL);
    }
    if (strncmp(file->valuestring, "https://", 8) == 0 || strncmp(file->valuestring, "http://", 7) == 0) {
        strlcpy(url, file->valuestring, sizeof(url));
    } else {  // relative to the manifest
        strlcpy(url, update_url(&cfg), sizeof(url));
        char *slash = strrchr(url, '/');
        if (slash) slash[1] = 0;
        strlcat(url, file->valuestring, sizeof(url));
    }
    const char *current = esp_app_get_description()->version;
    char latest[sizeof(s_up.latest)];
    strlcpy(latest, ver->valuestring, sizeof(latest));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool newer = fw_version_cmp(latest, current) > 0;
    bool was_offered = s_up.state == UP_AVAILABLE || strcmp(s_up.latest, latest) == 0;
    strlcpy(s_up.latest, latest, sizeof(s_up.latest));
    strlcpy(s_up.url, url, sizeof(s_up.url));
    strlcpy(s_up.sha256, sha->valuestring, sizeof(s_up.sha256));
    strlcpy(s_up.md5, md5->valuestring, sizeof(s_up.md5));
    strlcpy(s_up.notes, cJSON_IsString(notes) ? notes->valuestring : "", sizeof(s_up.notes));
    strlcpy(s_up.date, cJSON_IsString(date) ? date->valuestring : "", sizeof(s_up.date));
    s_up.size = (int)size->valuedouble;
    s_up.state = newer ? UP_AVAILABLE : UP_UPTODATE;
    s_up.checked_up_s = stats_uptime_s();
    s_up.checked_epoch = epoch_now();
    xSemaphoreGive(s_lock);
    cJSON_Delete(j);  // ver, file, ... pointed into j: only the copies from here on
    ESP_LOGI(TAG, "Release on git: %s (running %s)%s", latest, current, newer ? ": update available" : "");
    if (newer && !was_offered) stats_event(EV_INFO, "Firmware update %s is available", latest);
    if (newer && TEST_LOOP) do_install();
}

// ---------------------------------------------------------------- install

static void do_install(void) {
    up_t r;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    r = s_up;
    s_up.state = UP_DOWNLOADING;
    s_up.done = 0;
    s_up.error[0] = 0;
    xSemaphoreGive(s_lock);
    if (!fw_write_lock()) return set_error("Update failed: %s", "a firmware upload is in progress");
    stats_event(EV_INFO, "Installing firmware %s from git", r.latest);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    uint8_t *buf = malloc(4096);
    mbedtls_sha256_context sha;
    mbedtls_md5_context md5;
    mbedtls_sha256_init(&sha);
    mbedtls_md5_init(&md5);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_md5_starts(&md5);
    esp_ota_handle_t oh = 0;
    bool begun = false;
    const char *fail = NULL;
    char ver[33] = "";
    int got = 0, have = 0, status = 0;
    int64_t len = -1, started = esp_timer_get_time();
    esp_http_client_handle_t c = NULL;
    if (!buf || !part) fail = "out of memory";
    else if (part->size < (size_t)r.size) fail = "the release doesn't fit the firmware slot";
    else if (!(c = open_url_retry(r.url, &status, &len, &fail))) {
        // fail says why
    } else if (status != 200) fail = "the download answered with an error";
    else if (len > 0 && len != r.size) fail = "the download's size doesn't match the release";
    while (!fail && got < r.size) {
        if (esp_timer_get_time() - started > DOWNLOAD_DEADLINE_S * 1000000LL) {
            fail = "the download took too long";
            break;
        }
        int n = esp_http_client_read(c, (char *)buf + have, 4096 - have);
        if (n <= 0) {
            fail = "the download stopped early";
            break;
        }
        mbedtls_sha256_update(&sha, buf + have, n);
        mbedtls_md5_update(&md5, buf + have, n);
        got += n;
        have += n;
        if (got > r.size) {
            fail = "the download is bigger than the release";
            break;
        }
        if (!begun) {
            if ((size_t)have < fw_header_len()) continue;
            fail = fw_check_header(buf, have, ver);
            if (!fail && strcmp(ver, r.latest) != 0) fail = "the image's version isn't the release's";
            if (!fail && esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &oh) != ESP_OK) fail = "esp_ota_begin failed";
            if (fail) break;
            begun = true;
        }
        if (esp_ota_write(oh, buf, have) != ESP_OK) {
            fail = "flash write failed";
            break;
        }
        have = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_up.done = got;
        xSemaphoreGive(s_lock);
    }
    // A server that sends more than the release says: read one more byte to find out.
    if (!fail && c && esp_http_client_read(c, (char *)buf, 1) > 0) fail = "the download is bigger than the release";
    if (c) esp_http_client_cleanup(c);
    free(buf);

    // Nothing is switched on until the whole image matches the release: size, MD5 and SHA-256 of
    // what was downloaded, the image's own hash (esp_ota_end), then MD5 again of what's in flash.
    uint8_t d_sha[32], d_md5[16];
    char h_sha[65], h_md5[33];
    mbedtls_sha256_finish(&sha, d_sha);
    mbedtls_md5_finish(&md5, d_md5);
    mbedtls_sha256_free(&sha);
    mbedtls_md5_free(&md5);
    hex(d_sha, 32, h_sha);
    hex(d_md5, 16, h_md5);
    if (!fail && got != r.size) fail = "the download's size doesn't match the release";
    if (!fail && strcmp(h_md5, r.md5) != 0) fail = "MD5 doesn't match the release";
    if (!fail && strcmp(h_sha, r.sha256) != 0) fail = "SHA-256 doesn't match the release";
    if (!fail) {
        esp_err_t e = esp_ota_end(oh);  // frees the handle either way
        begun = false;
        if (e == ESP_ERR_OTA_VALIDATE_FAILED) fail = "the image failed its own verification";
        else if (e != ESP_OK) fail = "esp_ota_end failed";
        else if ((fail = fw_readback_md5(part, r.size, r.md5)) == NULL && esp_ota_set_boot_partition(part) != ESP_OK)
            fail = "could not select the new firmware";
    }
    if (begun) esp_ota_abort(oh);  // the spare slot is dropped; the running firmware never changed
    fw_write_unlock();
    ESP_LOGI(TAG, "Download of %s: %d of %d bytes in %lld ms, lowest free heap %u", r.latest, got, r.size,
             (esp_timer_get_time() - started) / 1000, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
    if (fail) {
        ESP_LOGE(TAG, "Update to %s failed: %s", r.latest, fail);
        stats_event(EV_ERROR, "Firmware update %s failed: %s", r.latest, fail);
        set_error("Update failed: %s", fail);
        return;
    }
    ESP_LOGI(TAG, "Firmware %s verified (MD5 %s, SHA-256 %.16s..., read back from flash) and written to %s; rebooting",
             r.latest, h_md5, h_sha, part->label);
    stats_event(EV_INFO, "Firmware %s verified (MD5 and SHA-256 match, read back from flash) and installed; rebooting", r.latest);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_up.state = UP_REBOOTING;
    xSemaphoreGive(s_lock);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

// ---------------------------------------------------------------- task + API

// The other slot holds a firmware that was installed but never confirmed (crashed, hung or had no
// network on its trial boot), so the bootloader went back to this one. Noted once per version.
static void note_rollback(void) {
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st;
    esp_app_desc_t d;
    if (!other || esp_ota_get_state_partition(other, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_ABORTED && st != ESP_OTA_IMG_INVALID) return;
    if (esp_ota_get_partition_description(other, &d) != ESP_OK) return;
    strlcpy(s_rolled_back, d.version, sizeof(s_rolled_back));
    char seen[33] = "";
    size_t n = sizeof(seen);
    nvs_handle_t h;
    if (nvs_open("boot", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_str(h, "rolled_back", seen, &n);
    if (strcmp(seen, d.version) != 0) {
        ESP_LOGE(TAG, "Firmware %s didn't start properly and was rolled back to %s", d.version, esp_app_get_description()->version);
        stats_event(EV_ERROR, "Firmware %s didn't start properly, so the bridge went back to %s", d.version,
                    esp_app_get_description()->version);
        nvs_set_str(h, "rolled_back", d.version);
        nvs_commit(h);
    }
    nvs_close(h);
}

static void updater_task(void *arg) {
    (void)arg;
    note_rollback();
    if (TEST_LOOP) ESP_LOGW(TAG, "TEST BUILD: installing anything newer from %s", TEST_URL);
    uint32_t next_check = FIRST_CHECK_S;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(TEST_LOOP ? 2000 : 10000));
        if (bits & NOTIFY_INSTALL) {
            do_install();
            continue;
        }
        bridge_settings_t cfg;
        settings_get(&cfg);
        bool due = (cfg.update_auto || TEST_LOOP) && stats_uptime_s() >= next_check;
        if ((bits & NOTIFY_CHECK) || (due && net_has_ip())) {
            do_check();
            next_check = stats_uptime_s() + CHECK_EVERY_S;
        }
    }
}

void updater_start(void) {
    s_lock = xSemaphoreCreateMutex();
    s_task = task_start(updater_task, "updater", 9216, NULL, 4, false);  // TLS handshake runs here
}

void updater_check_now(void) { xTaskNotify(s_task, NOTIFY_CHECK, eSetBits); }

bool updater_install(char *why, int why_len) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    up_state_t st = s_up.state;
    bool newer = s_up.latest[0] && fw_version_cmp(s_up.latest, esp_app_get_description()->version) > 0;
    xSemaphoreGive(s_lock);
    if (st == UP_DOWNLOADING || st == UP_REBOOTING) {
        snprintf(why, why_len, "An update is already being installed");
        return false;
    }
    if (st != UP_AVAILABLE && !(st == UP_ERROR && newer)) {
        snprintf(why, why_len, "No newer firmware found: check for updates first");
        return false;
    }
    xTaskNotify(s_task, NOTIFY_INSTALL, eSetBits);
    return true;
}

void updater_available(char *out, int out_len) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, out_len, "%s", s_up.state == UP_AVAILABLE ? s_up.latest : "");
    xSemaphoreGive(s_lock);
}

void updater_add_json(cJSON *root) {
    bridge_settings_t cfg;
    settings_get(&cfg);
    up_t u;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    u = s_up;
    xSemaphoreGive(s_lock);
    cJSON *o = cJSON_AddObjectToObject(root, "update");
    cJSON_AddStringToObject(o, "state", STATE_NAMES[u.state]);
    cJSON_AddStringToObject(o, "current", esp_app_get_description()->version);
    cJSON_AddStringToObject(o, "latest", u.latest);
    cJSON_AddStringToObject(o, "notes", u.notes);
    cJSON_AddStringToObject(o, "date", u.date);
    cJSON_AddNumberToObject(o, "size", u.size);
    cJSON_AddNumberToObject(o, "done", u.done);
    cJSON_AddStringToObject(o, "md5", u.md5);
    cJSON_AddStringToObject(o, "sha256", u.sha256);
    cJSON_AddNumberToObject(o, "checked_ago", u.checked_up_s ? (double)(stats_uptime_s() - u.checked_up_s) : -1);
    cJSON_AddStringToObject(o, "error", u.error);
    cJSON_AddStringToObject(o, "source", update_url(&cfg));
    cJSON_AddBoolToObject(o, "auto", cfg.update_auto);
    cJSON_AddStringToObject(o, "rolled_back", s_rolled_back);
}

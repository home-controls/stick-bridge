#include "fwcat.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "dongle.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "fwcat_core.h"
#include "mbedtls/sha256.h"
#include "ncpflash.h"
#include "net.h"
#include "settings.h"
#include "stats.h"

static const char *TAG = "fwcat";

#define MAX_CAND 12
#define URL_MAX 2048          // GitHub's signed release-asset redirects are long
#define AUTO_FIRST_S 120      // first automatic check after boot
#define AUTO_EVERY_S 86400    // then daily (the API allows 60 unauthenticated requests per hour)
#define NOTIFY_CHECK BIT0
#define NOTIFY_DOWNLOAD BIT1

typedef struct {
    fc_asset_t a;
    fc_name_t n;
    int score;
} cand_t;

typedef enum { DL_IDLE = 0, DL_RUNNING, DL_DONE, DL_FAILED } dl_state_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static struct {
    bool checking;
    int64_t checked_epoch;  // 0 = never
    uint32_t checked_up;
    char error[112];
    char repo[64];
    char release[48];
    bool prerelease;
    char board[48];
    char model[48];
    char type[16];
    int ncand;
    cand_t cand[MAX_CAND];
    dl_state_t dl;
    char dl_name[112];
    uint32_t dl_done, dl_total;
    char dl_error[112];
} s;
static char s_want[112];  // asset requested for download

// ---------------------------------------------------------------- HTTP (own buffers: long URLs)

static esp_http_client_handle_t gh_open(const char *url, bool api, int *status, int64_t *len, const char **why) {
    char *next = malloc(URL_MAX);
    if (!next) {
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
            .timeout_ms = 20000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .user_agent = "zigbee-eth-bridge",
            .buffer_size = 4096,     // long Location headers
            .buffer_size_tx = 2048,  // long request lines after a redirect
            .disable_auto_redirect = true,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) {
            *why = "out of memory";
            break;
        }
        if (api) {
            esp_http_client_set_header(c, "Accept", "application/vnd.github+json");
            esp_http_client_set_header(c, "X-GitHub-Api-Version", "2022-11-28");
        }
        if (esp_http_client_open(c, 0) != ESP_OK) {
            *why = "can't connect";
        } else {
            *len = esp_http_client_fetch_headers(c);
            *status = esp_http_client_get_status_code(c);
            bool redirect = *status == 301 || *status == 302 || *status == 303 || *status == 307 || *status == 308;
            if (*status == 0 || *len < -1) *why = "no answer";
            else if (!redirect) {
                ret = c;
                break;
            } else if (hop >= 4) *why = "too many redirects";
            else if (esp_http_client_set_redirection(c) != ESP_OK || esp_http_client_get_url(c, next, URL_MAX) != ESP_OK)
                *why = "bad redirect";
            else {
                esp_http_client_cleanup(c);
                continue;
            }
        }
        esp_http_client_cleanup(c);
        break;
    }
    free(next);
    return ret;
}

// ---------------------------------------------------------------- downloadable dongle list

#define DB_MAX 32768
static char s_db_status[96];
static int s_db_count = -1;
static uint32_t s_db_up;

static char *dup_str(const cJSON *o, const char *k, size_t max) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (!cJSON_IsString(v) || !v->valuestring[0]) return NULL;
    size_t n = strnlen(v->valuestring, max);
    char *s = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s) s = malloc(n + 1);
    if (s) {
        memcpy(s, v->valuestring, n);
        s[n] = 0;
    }
    return s;
}

static int num_or_hex(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsNumber(v)) return v->valueint;
    if (cJSON_IsString(v)) return (int)strtol(v->valuestring, NULL, 0);
    return -1;
}

// Parses the list; entries that don't check out are skipped (counted in *skipped).
static dongle_model_t *db_parse(const char *json, int *n_out, int *skipped) {
    *n_out = *skipped = 0;
    cJSON *a = cJSON_Parse(json);
    if (!cJSON_IsArray(a)) {
        cJSON_Delete(a);
        return NULL;
    }
    int n = cJSON_GetArraySize(a);
    if (n > 64) n = 64;
    dongle_model_t *m = heap_caps_calloc(n + 1, sizeof(*m), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!m) m = calloc(n + 1, sizeof(*m));
    if (!m) {
        cJSON_Delete(a);
        return NULL;
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        const cJSON *o = cJSON_GetArrayItem(a, i);
        int vid = num_or_hex(o, "vid"), pid = num_or_hex(o, "pid"), baud = num_or_hex(o, "baud");
        int fam = family_from_name(cJSON_GetStringValue(cJSON_GetObjectItem(o, "family")));
        int pro = proto_from_name(cJSON_GetStringValue(cJSON_GetObjectItem(o, "protocol")));
        int rst = reset_from_name(cJSON_GetStringValue(cJSON_GetObjectItem(o, "reset")));
        dongle_model_t *d = &m[k];
        d->id = dup_str(o, "id", 47);
        d->name = dup_str(o, "name", 63);
        if (!d->id || !d->name || vid <= 0 || vid > 0xFFFF || pid < 0 || pid > 0xFFFF || fam < 0 || pro < 0 || rst < 0 ||
            (baud >= 0 && (baud < 9600 || baud > 921600))) {
            (*skipped)++;
            free((void *)d->id);
            free((void *)d->name);
            memset(d, 0, sizeof(*d));
            continue;
        }
        d->vid = (uint16_t)vid;
        d->pid = (uint16_t)pid;
        d->mfr = dup_str(o, "manufacturer", 63);
        d->product = dup_str(o, "product", 63);
        d->radio = dup_str(o, "radio", 31);
        if (!d->radio) d->radio = "";
        d->family = (family_t)fam;
        d->proto = (proto_t)pro;
        d->baud = baud > 0 ? (uint32_t)baud : 115200;
        d->hw_flow = cJSON_IsTrue(cJSON_GetObjectItem(o, "rtscts"));
        d->reset = (reset_t)rst;
        d->fw_board = dup_str(o, "fw_board", 47);
        d->note = dup_str(o, "note", 111);
        k++;
    }
    cJSON_Delete(a);
    *n_out = k;
    return m;
}

static uint32_t s_db_hash;
static void db_refresh(void) {
    bridge_settings_t cfg;
    settings_get(&cfg);
    char url[200];
    strlcpy(url, cfg.update_url, sizeof(url));
    char *slash = strrchr(url, '/');
    if (!slash || (size_t)(slash - url) + 14 >= sizeof(url)) return;
    strcpy(slash + 1, "dongles.json");
    int status;
    int64_t len;
    const char *why = NULL;
    esp_http_client_handle_t c = gh_open(url, false, &status, &len, &why);
    char *body = NULL;
    if (c && status == 200) {
        body = heap_caps_malloc(DB_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!body) body = malloc(DB_MAX + 1);
        int got = 0, r;
        while (body && got < DB_MAX && (r = esp_http_client_read(c, body + got, DB_MAX - got)) > 0) got += r;
        if (body) body[got] = 0;
        if (body && got >= DB_MAX) why = "dongles.json is too big";
    } else if (c && status == 404) {
        why = "none published";
    } else if (c) {
        why = "server error";
    }
    if (c) esp_http_client_cleanup(c);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_db_up = stats_uptime_s();
    xSemaphoreGive(s_lock);
    if (why || !body) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_db_status, sizeof(s_db_status), "%s", why ? why : "out of memory");
        xSemaphoreGive(s_lock);
        free(body);
        return;
    }
    uint32_t h = 2166136261u;  // FNV-1a: only re-parse when it changed
    for (const char *p = body; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
    if (h == s_db_hash) {
        free(body);
        return;
    }
    int n, skipped;
    dongle_model_t *m = db_parse(body, &n, &skipped);
    free(body);
    if (!m) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_db_status, sizeof(s_db_status), "dongles.json isn't a JSON array");
        xSemaphoreGive(s_lock);
        return;
    }
    s_db_hash = h;
    dongles_set_extra(m, n);  // the old list is left allocated: something may still point into it
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_db_count = n;
    snprintf(s_db_status, sizeof(s_db_status), "%d dongle%s%s", n, n == 1 ? "" : "s", skipped ? " (some entries skipped)" : "");
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Dongle list from git: %d entries, %d skipped", n, skipped);
    if (skipped) stats_event(EV_WARN, "Downloaded dongle list: %d entries skipped (bad fields)", skipped);
    dongle_rematch();
}

// ---------------------------------------------------------------- check

typedef struct {
    const char *board;
    const char *type;  // zigbee_ncp or openthread_rcp (by role); "coordinator" for TI
    bool ti;           // Koenkk Z-Stack names (<board>_coordinator_<date>.zip)
    uint32_t baud;
    bool wired;
    int rel;  // release index the kept candidates come from (-1 none yet)
    int n;
    cand_t c[MAX_CAND];
    int seen;  // assets scanned
} scan_t;

static void on_asset(void *ctx, const fc_release_t *rel, const fc_asset_t *a) {
    scan_t *sc = ctx;
    sc->seen++;
    fc_name_t n;
    if (!(sc->ti ? fc_parse_ti_name(a->name, &n) : fc_parse_name(a->name, &n))) return;
    if (strcasecmp(n.board, sc->board) != 0 || strcmp(n.type, sc->type) != 0) return;
    if (sc->rel >= 0 && rel->index > sc->rel) return;  // a newer release already had builds for this board
    if (rel->index < sc->rel || sc->rel < 0) {
        sc->rel = rel->index;
        sc->n = 0;
    }
    if (sc->n >= MAX_CAND) return;
    cand_t *c = &sc->c[sc->n++];
    c->a = *a;
    c->n = n;
    // usable variants score >= 0; hw_flow on a board without RTS/CTS is listed but scored -1
    c->score = sc->ti ? 100 : fc_score(&n, sc->board, sc->type, sc->baud, sc->wired);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s.release, rel->tag, sizeof(s.release));
    s.prerelease = rel->prerelease;
    xSemaphoreGive(s_lock);
}

static int64_t epoch_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec > 1700000000 ? tv.tv_sec : 0;
}

static void set_error(char *dst, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_error(char *dst, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    vsnprintf(dst, n, fmt, ap);
    xSemaphoreGive(s_lock);
    va_end(ap);
}

static int cmp_cand(const void *x, const void *y) { return ((const cand_t *)y)->score - ((const cand_t *)x)->score; }

static void do_check(void) {
    dongle_info_t d;
    dongle_info(&d);
    bridge_settings_t cfg;
    settings_get(&cfg);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.checking = true;
    s.error[0] = 0;
    strlcpy(s.repo, cfg.fw_repo, sizeof(s.repo));
    strlcpy(s.model, d.model->name, sizeof(s.model));
    strlcpy(s.board, d.model->fw_board ? d.model->fw_board : "", sizeof(s.board));
    xSemaphoreGive(s_lock);

    const char *why = NULL;
    scan_t *sc = calloc(1, sizeof(*sc));
    char *buf = malloc(4096);
    esp_http_client_handle_t c = NULL;
    if (!sc || !buf) {
        why = "out of memory";
    } else if (!d.model->fw_board || (d.model->family != FAM_SILABS && d.model->family != FAM_TI)) {
        set_error(s.error, sizeof(s.error), "No firmware list for %s (%s radio)%s", d.model->name,
                  family_name(d.model->family), d.model->family == FAM_UNKNOWN ? ": dongle not identified" : "");
    } else if (d.model->family == FAM_TI && cfg.role == BRIDGE_ROLE_THREAD) {
        set_error(s.error, sizeof(s.error), "TI dongles run Z-Stack (Zigbee) only: set the role to Zigbee");
    } else {
        sc->board = d.model->fw_board;
        sc->ti = d.model->family == FAM_TI;
        sc->type = sc->ti ? "coordinator" : cfg.role == BRIDGE_ROLE_THREAD ? "openthread_rcp" : "zigbee_ncp";
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s.type, sc->type, sizeof(s.type));
        xSemaphoreGive(s_lock);
        sc->baud = cfg.baud;
        sc->wired = d.model->hw_flow;
        sc->rel = -1;
        char url[160];
        if (sc->ti) {
            // coordinator and router releases alternate (and old ones have no files): look at a few
            snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=8", CONFIG_BRIDGE_TI_FW_REPO);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            strlcpy(s.repo, CONFIG_BRIDGE_TI_FW_REPO, sizeof(s.repo));
            xSemaphoreGive(s_lock);
        } else if (cfg.fw_prerelease) {
            snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=2", cfg.fw_repo);
        } else {
            snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", cfg.fw_repo);
        }
        int status;
        int64_t len;
        c = gh_open(url, true, &status, &len, &why);
        if (c && status != 200) {
            set_error(s.error, sizeof(s.error), "GitHub answered %d%s", status,
                      status == 403 || status == 429 ? " (rate limited: try again in an hour)" : status == 404 ? " (no such repo or no releases)" : "");
        } else if (c) {
            fc_json_t *j = malloc(sizeof(*j));
            if (!j) {
                why = "out of memory";
            } else {
                fc_json_init(j, on_asset, sc);
                int r;
                while ((r = esp_http_client_read(c, buf, 4096)) > 0) fc_json_feed(j, buf, (size_t)r);
                if (r < 0) why = "the download broke off";
                else if (j->bad || j->depth != 0) why = "GitHub's answer didn't parse";
                free(j);
            }
        }
    }
    if (why) set_error(s.error, sizeof(s.error), "%s", why);
    if (c) esp_http_client_cleanup(c);
    if (sc) {
        qsort(sc->c, sc->n, sizeof(cand_t), cmp_cand);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s.ncand = sc->n;
        memcpy(s.cand, sc->c, sizeof(cand_t) * sc->n);
        if (!s.error[0] && sc->n == 0 && sc->board)
            snprintf(s.error, sizeof(s.error), "No %s build for board '%s' in the newest release%s (%d files)", sc->type, sc->board,
                     cfg.fw_prerelease ? "s" : "", sc->seen);
        if (sc->n == 0) s.release[0] = 0;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "Checked %s for %s: %d files, %d for this board", cfg.fw_repo, sc->board ? sc->board : "?", sc->seen, sc->n);
    }
    free(buf);
    free(sc);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.checking = false;
    s.checked_epoch = epoch_now();
    s.checked_up = stats_uptime_s();
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------- download

static void do_download(void) {
    cand_t c;
    memset(&c, 0, sizeof(c));
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s.ncand; i++)
        if (strcmp(s.cand[i].a.name, s_want) == 0) {
            c = s.cand[i];
            found = true;
        }
    s.dl = DL_RUNNING;
    s.dl_done = 0;
    s.dl_total = found ? c.a.size : 0;
    s.dl_error[0] = 0;
    strlcpy(s.dl_name, s_want, sizeof(s.dl_name));
    xSemaphoreGive(s_lock);

    const char *why = NULL;
    char whybuf[112] = "";
    bool begun = false;
    esp_http_client_handle_t h = NULL;
    uint8_t *buf = NULL;
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    if (!found) {
        why = "that file isn't in the last check";
    } else if ((why = ncpflash_upload_begin(c.a.size, c.a.name)) == NULL) {
        begun = true;
        int status;
        int64_t len;
        h = gh_open(c.a.url, false, &status, &len, &why);
        if (h && status != 200) {
            snprintf(whybuf, sizeof(whybuf), "GitHub answered %d for the file", status);
            why = whybuf;
        } else if (h && len >= 0 && (uint64_t)len != c.a.size) {
            why = "the file's size isn't what the release says";
        } else if (h && !(buf = malloc(4096))) {
            why = "out of memory";
        }
        if (!why) {
            mbedtls_sha256_starts(&sha, 0);
            uint32_t got = 0;
            while (!why && got < c.a.size) {
                int r = esp_http_client_read(h, (char *)buf, 4096);
                if (r <= 0) {
                    why = "the download broke off";
                    break;
                }
                mbedtls_sha256_update(&sha, buf, (size_t)r);
                why = ncpflash_upload_write(buf, (size_t)r);
                got += (uint32_t)r;
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s.dl_done = got;
                xSemaphoreGive(s_lock);
            }
            if (!why && c.a.sha256[0]) {
                uint8_t dig[32];
                char hex[65];
                mbedtls_sha256_finish(&sha, dig);
                for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", dig[i]);
                if (strcasecmp(hex, c.a.sha256) != 0) why = "SHA-256 doesn't match GitHub's: download damaged";
            }
        }
    }
    if (begun) {
        const char *end = ncpflash_upload_end(why == NULL);
        if (!why && end) {
            strlcpy(whybuf, end, sizeof(whybuf));
            why = whybuf;
        }
    }
    mbedtls_sha256_free(&sha);
    free(buf);
    if (h) esp_http_client_cleanup(h);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s.dl = why ? DL_FAILED : DL_DONE;
    if (why) strlcpy(s.dl_error, why, sizeof(s.dl_error));
    xSemaphoreGive(s_lock);
    if (why) stats_event(EV_WARN, "Dongle firmware download failed: %s", why);
    else stats_event(EV_INFO, "Dongle firmware %s downloaded (SHA-256 checked): ready to flash", s_want);
}

// ---------------------------------------------------------------- task + API

static void fwcat_task(void *arg) {
    (void)arg;
    uint32_t next_auto = AUTO_FIRST_S, next_db = 30;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(10000));
        if (bits & NOTIFY_DOWNLOAD) do_download();
        if (stats_uptime_s() >= next_db && net_has_ip()) {
            db_refresh();
            next_db = stats_uptime_s() + AUTO_EVERY_S;
        }
        bool auto_due = false;
        if (stats_uptime_s() >= next_auto) {
            dongle_info_t d;
            dongle_info(&d);
            if (dongle_ready() && (d.model->family == FAM_SILABS || d.model->family == FAM_TI) && d.model->fw_board) auto_due = true;
            next_auto = stats_uptime_s() + (auto_due ? AUTO_EVERY_S : 300);
        }
        if ((bits & NOTIFY_CHECK) || auto_due) do_check();
    }
}

void fwcat_start(void) {
    s_lock = xSemaphoreCreateMutex();
    xTaskCreate(fwcat_task, "fwcat", 8192, NULL, 4, &s_task);
}

void fwcat_check_now(void) {
    if (s_task) xTaskNotify(s_task, NOTIFY_CHECK, eSetBits);
}

bool fwcat_download(const char *name, char *why, size_t why_len) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s.dl == DL_RUNNING || s.checking, have = false;
    for (int i = 0; i < s.ncand; i++)
        if (strcmp(s.cand[i].a.name, name) == 0) have = true;
    if (!busy && have) {
        strlcpy(s_want, name, sizeof(s_want));
        s.dl = DL_RUNNING;  // shows at once; the task fills in the rest
        s.dl_done = 0;
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        strlcpy(why, "A check or download is already running", why_len);
        return false;
    }
    if (!have) {
        strlcpy(why, "That file isn't in the last check: check again", why_len);
        return false;
    }
    if (ncpflash_busy()) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s.dl = DL_IDLE;
        xSemaphoreGive(s_lock);
        strlcpy(why, "The dongle is being flashed", why_len);
        return false;
    }
    xTaskNotify(s_task, NOTIFY_DOWNLOAD, eSetBits);
    return true;
}

static void stack_str(char *out, size_t n) {
    dongle_info_t d;
    dongle_info(&d);
    if (d.fw_version[0] && d.proto_detected != PROTO_EZSP) {  // Spinel / Z-Stack, read off the traffic
        strlcpy(out, d.fw_version, n);
        return;
    }
    stats_lock();
    uint8_t ezsp = g_stats.ezsp_version;
    uint16_t v = g_stats.stack_version;
    stats_unlock();
    if (ezsp) snprintf(out, n, "%u.%u.%u.%u", (v >> 12) & 15, (v >> 8) & 15, (v >> 4) & 15, v & 15);
    else out[0] = 0;
}

void fwcat_add_json(cJSON *root) {
    char cur[24];
    stack_str(cur, sizeof(cur));
    cJSON *o = cJSON_AddObjectToObject(root, "catalog");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddBoolToObject(o, "checking", s.checking);
    cJSON_AddNumberToObject(o, "checked", (double)s.checked_epoch);
    cJSON_AddNumberToObject(o, "checked_ago", s.checked_up ? (double)(stats_uptime_s() - s.checked_up) : -1);
    cJSON_AddStringToObject(o, "error", s.error);
    cJSON_AddStringToObject(o, "repo", s.repo);
    cJSON_AddStringToObject(o, "release", s.release);
    cJSON_AddBoolToObject(o, "prerelease", s.prerelease);
    cJSON_AddStringToObject(o, "board", s.board);
    cJSON_AddStringToObject(o, "type", s.type);
    cJSON_AddStringToObject(o, "model", s.model);
    cJSON_AddStringToObject(o, "current", cur);
    cJSON *a = cJSON_AddArrayToObject(o, "builds");
    for (int i = 0; i < s.ncand; i++) {
        const cand_t *c = &s.cand[i];
        cJSON *b = cJSON_CreateObject();
        cJSON_AddStringToObject(b, "name", c->a.name);
        cJSON_AddStringToObject(b, "version", c->n.version);
        cJSON_AddStringToObject(b, "sdk", c->n.sdk);
        cJSON_AddNumberToObject(b, "baud", c->n.baud);
        cJSON_AddStringToObject(b, "flow", c->n.flow);
        cJSON_AddNumberToObject(b, "size", c->a.size);
        cJSON_AddStringToObject(b, "sha256", c->a.sha256);
        cJSON_AddBoolToObject(b, "usable", c->score >= 0);
        cJSON_AddBoolToObject(b, "recommended", i == 0 && c->score >= 0);
        int cmp = cur[0] ? fc_vercmp(c->n.version, cur) : 0;
        cJSON_AddStringToObject(b, "vs_current", !cur[0] ? "unknown" : cmp > 0 ? "newer" : cmp < 0 ? "older" : "same");
        cJSON_AddItemToArray(a, b);
    }
    cJSON *db = cJSON_AddObjectToObject(o, "dongle_list");
    cJSON_AddStringToObject(db, "status", s_db_status);
    cJSON_AddNumberToObject(db, "count", s_db_count);
    cJSON_AddNumberToObject(db, "fetched_ago", s_db_up ? (double)(stats_uptime_s() - s_db_up) : -1);
    cJSON *dl = cJSON_AddObjectToObject(o, "download");
    static const char *const st[] = {"idle", "running", "done", "failed"};
    cJSON_AddStringToObject(dl, "state", st[s.dl]);
    cJSON_AddStringToObject(dl, "name", s.dl_name);
    cJSON_AddNumberToObject(dl, "done", s.dl_done);
    cJSON_AddNumberToObject(dl, "total", s.dl_total);
    cJSON_AddStringToObject(dl, "error", s.dl_error);
    xSemaphoreGive(s_lock);
}

#include "fwcat.h"
#include "tasks.h"

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
#define REUSE_S 600           // another radio's identical check younger than this is reused
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
// One check result per radio, for the stick that radio had when it was checked.
typedef struct {
    bool checking;
    int64_t checked_epoch;  // 0 = never
    uint32_t checked_up;    // 0 = never
    char error[112];
    char repo[64];
    char release[48];
    bool prerelease;
    bool fw_pre;  // the pre-release setting the check ran with (part of what makes two checks the same)
    uint32_t baud;  // the radio's baud and the stick's RTS/CTS wiring: builds are ranked by both
    bool wired;
    char board[48];
    char model[48];
    char type[16];
    uint16_t vid, pid;
    char serial[48];
    int ncand;
    cand_t *cand;  // [MAX_CAND], ~6.7 KB each: in PSRAM (fwcat_start)
} res_t;
static res_t s_res[RADIO_MAX];
static uint32_t s_check_req;  // bit i: check radio i (guarded by s_lock)
static struct {
    dl_state_t dl;
    int radio;  // whose check the download came from
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
            else if (strncmp(next, "https://", 8) != 0)
                *why = "redirected to a plain http address";  // downloads stay https all the way
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

static esp_http_client_handle_t gh_open_retry(const char *url, bool api, int *status, int64_t *len, const char **why) {
    for (int attempt = 0;; attempt++) {
        esp_http_client_handle_t c = gh_open(url, api, status, len, why);
        bool server_trouble = !c || *status >= 500;
        if (c && *status >= 400) log_error_body(c, *status, url);
        if (!server_trouble || attempt >= 1) return c;
        if (c) esp_http_client_cleanup(c);
        ESP_LOGW(TAG, "%s: trying once more in 2 s", c ? "server error" : *why ? *why : "no connection");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
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
    char whybuf[40];
    esp_http_client_handle_t c = gh_open_retry(url, false, &status, &len, &why);
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
        snprintf(whybuf, sizeof(whybuf), "the server answered %d", status);
        why = whybuf;
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
    snprintf(s_db_status, sizeof(s_db_status), "%d stick%s%s", n, n == 1 ? "" : "s", skipped ? " (some entries skipped)" : "");
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Stick list from git: %d entries, %d skipped", n, skipped);
    if (skipped) stats_event(EV_WARN, "Downloaded stick list: %d entries skipped (bad fields)", skipped);
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
    res_t *r;
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
    strlcpy(sc->r->release, rel->tag, sizeof(sc->r->release));
    sc->r->prerelease = rel->prerelease;
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

static void do_check(int radio) {
    res_t *R = &s_res[radio];
    dongle_info_t d;
    dx_info(dongle_at(radio), &d);
    bridge_settings_t cfg;
    settings_get(&cfg);
    uint8_t role = settings_radio_role(radio);
    bool ti = d.model->family == FAM_TI;
    const char *type = ti ? "coordinator" : role == BRIDGE_ROLE_THREAD ? "openthread_rcp" : "zigbee_ncp";
    const char *repo = ti ? CONFIG_BRIDGE_TI_FW_REPO : cfg.fw_repo;
    const char *board = d.model->fw_board ? d.model->fw_board : "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    R->checking = true;
    R->error[0] = 0;
    R->release[0] = 0;
    R->ncand = 0;
    R->fw_pre = cfg.fw_prerelease;
    R->baud = settings_radio_baud(radio);
    R->wired = d.model->hw_flow;
    R->vid = d.vid;
    R->pid = d.pid;
    strlcpy(R->serial, d.serial, sizeof(R->serial));
    strlcpy(R->repo, repo, sizeof(R->repo));
    strlcpy(R->model, d.model->name, sizeof(R->model));
    strlcpy(R->board, board, sizeof(R->board));
    strlcpy(R->type, type, sizeof(R->type));
    // The same check for another radio a few minutes ago (two identical sticks): reuse it, GitHub's API allows
    // 60 unauthenticated requests an hour.
    bool reused = false;
    uint32_t up = stats_uptime_s();
    for (int j = 0; j < RADIO_MAX && board[0]; j++) {
        res_t *o = &s_res[j];
        if (j == radio || o->checking || !o->checked_up || up - o->checked_up > REUSE_S || o->error[0]) continue;
        if (strcmp(o->board, board) || strcmp(o->type, type) || strcmp(o->repo, repo) || o->fw_pre != R->fw_pre ||
            o->baud != R->baud || o->wired != R->wired)
            continue;
        memcpy(R->cand, o->cand, sizeof(cand_t) * o->ncand);
        R->ncand = o->ncand;
        strlcpy(R->release, o->release, sizeof(R->release));
        R->prerelease = o->prerelease;
        R->checked_epoch = o->checked_epoch;
        R->checked_up = o->checked_up;
        R->checking = false;
        reused = true;
        break;
    }
    xSemaphoreGive(s_lock);
    if (reused) return;

    const char *why = NULL;
    scan_t *sc = calloc(1, sizeof(*sc));
    char *buf = malloc(4096);
    esp_http_client_handle_t c = NULL;
    if (!sc || !buf) {
        why = "out of memory";
    } else if (!d.model->fw_board || (d.model->family != FAM_SILABS && d.model->family != FAM_TI)) {
        set_error(R->error, sizeof(R->error), "No firmware updates through the bridge for %s (%s)%s", d.model->name,
                  family_name(d.model->family), d.model->family == FAM_UNKNOWN ? ": stick not identified" : "");
    } else if (ti && role == BRIDGE_ROLE_THREAD) {
        set_error(R->error, sizeof(R->error), "TI sticks run Z-Stack (Zigbee) only: set the role to Zigbee2MQTT or ZHA");
    } else {
        sc->r = R;
        sc->board = d.model->fw_board;
        sc->ti = ti;
        sc->type = type;
        sc->baud = settings_radio_baud(radio);  // the radio's link
        sc->wired = d.model->hw_flow;
        sc->rel = -1;
        char url[160];
        if (ti) {
            // coordinator and router releases alternate (and old ones have no files): look at a few
            snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=8", CONFIG_BRIDGE_TI_FW_REPO);
        } else if (cfg.fw_prerelease) {
            snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases?per_page=2", cfg.fw_repo);
        } else {
            snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", cfg.fw_repo);
        }
        int status;
        int64_t len;
        c = gh_open_retry(url, true, &status, &len, &why);
        if (c && status != 200) {
            set_error(R->error, sizeof(R->error), "GitHub answered %d%s", status,
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
    if (why) set_error(R->error, sizeof(R->error), "%s", why);
    if (c) esp_http_client_cleanup(c);
    if (sc) {
        qsort(sc->c, sc->n, sizeof(cand_t), cmp_cand);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        R->ncand = sc->n;
        memcpy(R->cand, sc->c, sizeof(cand_t) * sc->n);
        if (!R->error[0] && sc->n == 0 && sc->board)
            snprintf(R->error, sizeof(R->error), "No %s build for board '%s' in the newest release%s (%d files)", sc->type, sc->board,
                     cfg.fw_prerelease ? "s" : "", sc->seen);
        if (sc->n == 0) R->release[0] = 0;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "Radio %d: checked %s for %s: %d files, %d for this board", radio + 1, repo, board[0] ? board : "?",
                 sc->seen, sc->n);
    }
    free(buf);
    free(sc);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    R->checking = false;
    R->checked_epoch = epoch_now();
    R->checked_up = stats_uptime_s();
    if (!R->checked_up) R->checked_up = 1;
    xSemaphoreGive(s_lock);
}

// ---------------------------------------------------------------- download

static void do_download(void) {
    cand_t c;
    memset(&c, 0, sizeof(c));
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    res_t *R = &s_res[s.radio];
    for (int i = 0; i < R->ncand; i++)
        if (strcmp(R->cand[i].a.name, s_want) == 0) {
            c = R->cand[i];
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
        h = gh_open_retry(c.a.url, false, &status, &len, &why);
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
    if (why) stats_event(EV_WARN, "Radio %d: firmware download failed: %s", s.radio + 1, why);
    else stats_event(EV_INFO, "Radio %d: firmware %s downloaded (SHA-256 checked): ready to flash", s.radio + 1, s_want);
}

// ---------------------------------------------------------------- task + API

static void fwcat_task(void *arg) {
    (void)arg;
    uint32_t next_db = 30, next_auto[RADIO_MAX];
    for (int i = 0; i < RADIO_MAX; i++) next_auto[i] = AUTO_FIRST_S + 30 * i;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(10000));
        if (bits & NOTIFY_DOWNLOAD) do_download();
        if (stats_uptime_s() >= next_db && net_has_ip()) {
            db_refresh();
            next_db = stats_uptime_s() + AUTO_EVERY_S;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t req = s_check_req;
        s_check_req = 0;
        xSemaphoreGive(s_lock);
        for (int i = 0; i < RADIO_MAX; i++) {
            bool due = false;
            if (stats_uptime_s() >= next_auto[i]) {
                dongle_info_t d;
                dx_info(dongle_at(i), &d);
                bool can = dx_ready(dongle_at(i)) && (d.model->family == FAM_SILABS || d.model->family == FAM_TI) && d.model->fw_board;
                due = can && net_has_ip();
                next_auto[i] = stats_uptime_s() + (due ? AUTO_EVERY_S : 300);
            }
            if (((req >> i) & 1u) || due) do_check(i);
        }
    }
}

void fwcat_start(void) {
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < RADIO_MAX; i++) {
        s_res[i].cand = heap_caps_calloc(MAX_CAND, sizeof(cand_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_res[i].cand) s_res[i].cand = calloc(MAX_CAND, sizeof(cand_t));
        if (!s_res[i].cand) return;  // no catalogue rather than no bridge
    }
    s_task = task_start(fwcat_task, "fwcat", 8192, NULL, 4, false);  // downloads write the image area
}

void fwcat_check_now(int radio) {
    if (radio < 0 || radio >= RADIO_MAX || !s_task) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_check_req |= 1u << radio;
    xSemaphoreGive(s_lock);
    xTaskNotify(s_task, NOTIFY_CHECK, eSetBits);
}

void fwcat_forget(int radio) {
    if (radio < 0 || radio >= RADIO_MAX || !s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    res_t *R = &s_res[radio];
    if (!R->checking) {
        R->ncand = 0;
        R->checked_up = 0;
        R->checked_epoch = 0;
        R->error[0] = R->release[0] = 0;
    }
    xSemaphoreGive(s_lock);
}

bool fwcat_download(int radio, const char *name, char *why, size_t why_len) {
    if (radio < 0 || radio >= RADIO_MAX) radio = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    res_t *R = &s_res[radio];
    bool busy = s.dl == DL_RUNNING || R->checking, have = false;
    for (int i = 0; i < R->ncand; i++)
        if (strcmp(R->cand[i].a.name, name) == 0) have = true;
    if (!busy && have) {
        strlcpy(s_want, name, sizeof(s_want));
        s.radio = radio;
        s.dl = DL_RUNNING;  // shows at once; the task fills in the rest
        s.dl_done = 0;
    }
    xSemaphoreGive(s_lock);
    if (busy) {
        strlcpy(why, "A check or download is already running", why_len);
        return false;
    }
    if (!have) {
        strlcpy(why, "That file isn't in this radio's last check: check again", why_len);
        return false;
    }
    if (ncpflash_busy()) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s.dl = DL_IDLE;
        xSemaphoreGive(s_lock);
        strlcpy(why, "A stick is being flashed", why_len);
        return false;
    }
    xTaskNotify(s_task, NOTIFY_DOWNLOAD, eSetBits);
    return true;
}

// The version builds are compared against: what the traffic showed (any radio), else radio 0's EZSP decoder.
static void stack_str(int radio, char *out, size_t n) {
    dongle_info_t d;
    dx_info(dongle_at(radio), &d);
    if (d.fw_version[0]) {
        strlcpy(out, d.fw_version, n);
        return;
    }
    out[0] = 0;
    if (radio != 0) return;
    stats_lock();
    uint8_t ezsp = g_stats.ezsp_version;
    uint16_t v = g_stats.stack_version;
    stats_unlock();
    if (ezsp) snprintf(out, n, "%u.%u.%u.%u", (v >> 12) & 15, (v >> 8) & 15, (v >> 4) & 15, v & 15);
}

void fwcat_add_json(cJSON *root, int radio) {
    if (radio < 0 || radio >= RADIO_MAX) radio = 0;
    char cur[24];
    stack_str(radio, cur, sizeof(cur));
    dongle_info_t d;
    dx_info(dongle_at(radio), &d);
    cJSON *o = cJSON_AddObjectToObject(root, "catalog");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const res_t *R = &s_res[radio];
    // A check for the stick this radio had before (since replaced) doesn't count.
    bool mine = R->checking || (R->vid == d.vid && R->pid == d.pid && strcmp(R->serial, d.serial) == 0);
    int ncand = mine ? R->ncand : 0;
    cJSON_AddNumberToObject(o, "radio", radio);
    cJSON_AddBoolToObject(o, "checking", R->checking);
    cJSON_AddNumberToObject(o, "checked", mine ? (double)R->checked_epoch : 0);
    cJSON_AddNumberToObject(o, "checked_ago", mine && R->checked_up ? (double)(stats_uptime_s() - R->checked_up) : -1);
    cJSON_AddStringToObject(o, "error", mine ? R->error : "");
    cJSON_AddStringToObject(o, "repo", R->repo);
    cJSON_AddStringToObject(o, "release", mine ? R->release : "");
    cJSON_AddBoolToObject(o, "prerelease", mine && R->prerelease);
    cJSON_AddStringToObject(o, "board", R->board);
    cJSON_AddStringToObject(o, "type", R->type);
    cJSON_AddStringToObject(o, "model", R->model);
    cJSON_AddStringToObject(o, "current", cur);
    cJSON *a = cJSON_AddArrayToObject(o, "builds");
    for (int i = 0; i < ncand; i++) {
        const cand_t *c = &R->cand[i];
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
    cJSON_AddNumberToObject(dl, "radio", s.radio);
    cJSON_AddStringToObject(dl, "name", s.dl_name);
    cJSON_AddNumberToObject(dl, "done", s.dl_done);
    cJSON_AddNumberToObject(dl, "total", s.dl_total);
    cJSON_AddStringToObject(dl, "error", s.dl_error);
    xSemaphoreGive(s_lock);
}

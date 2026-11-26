#include "web.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "bridge.h"
#include "cJSON.h"
#include "dongle.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "mbedtls/md5.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "logbuf.h"
#include "logstore.h"
#include "sdlog.h"
#include "ncpflash.h"
#include "lwip/inet.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "settings.h"
#include "stats.h"
#include "fwcheck.h"
#include "fwcat.h"
#include "sysinfo.h"
#include "updater.h"
#include "watchdog.h"

static const char *TAG = "web";

extern const uint8_t index_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_gz_end[] asm("_binary_index_html_gz_end");

// ---------------------------------------------------------------- sessions + auth

// Signed-in browsers are remembered in flash (NVS "cfg"/"sessions", cleared by a factory reset), so a
// reboot or a firmware update doesn't sign anyone out. Only a SHA-256 of each token is stored. A browser
// stays signed in while it keeps coming back: the cookie lasts a year and is renewed on every visit.
// Eight browsers at most (the one unused longest makes room); a password change signs everyone out.
#define SESSIONS 8
#define SESSION_MAX_AGE_S (365 * 24 * 3600)
typedef struct {
    uint8_t hash[32];
    int64_t created;   // epoch s when the clock was synced at login, else 0
    uint32_t last_s;   // uptime s of the last use (0 after a reboot: fresh again)
    bool used;
} session_t;
typedef struct {  // what is kept in flash
    uint8_t hash[32];
    int64_t created;
} session_rec_t;
static session_t s_sessions[SESSIONS];
static SemaphoreHandle_t s_sess_lock;
static uint32_t s_fail_count, s_fail_window_s, s_locked_until_s;

static int64_t epoch_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec > 1700000000 ? tv.tv_sec : 0;
}

static void token_hash(const char *tok, uint8_t *out) { mbedtls_sha256((const unsigned char *)tok, strlen(tok), out, 0); }

// Caller holds s_sess_lock.
static void sessions_save(void) {
    session_rec_t recs[SESSIONS];
    int n = 0;
    for (int i = 0; i < SESSIONS; i++) {
        if (!s_sessions[i].used) continue;
        memcpy(recs[n].hash, s_sessions[i].hash, 32);
        recs[n].created = s_sessions[i].created;
        n++;
    }
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    if (n) nvs_set_blob(h, "sessions", recs, n * sizeof(session_rec_t));
    else nvs_erase_key(h, "sessions");
    nvs_commit(h);
    nvs_close(h);
}

static void sessions_load(void) {
    session_rec_t recs[SESSIONS];
    size_t len = sizeof(recs);
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_blob(h, "sessions", recs, &len) == ESP_OK) {
        int n = len / sizeof(session_rec_t);
        for (int i = 0; i < n && i < SESSIONS; i++) {
            memcpy(s_sessions[i].hash, recs[i].hash, 32);
            s_sessions[i].created = recs[i].created;
            s_sessions[i].last_s = 0;
            s_sessions[i].used = true;
        }
        ESP_LOGI(TAG, "%d signed-in browser(s) remembered", n);
    }
    nvs_close(h);
}

static void new_session(char *out) {
    uint8_t r[16];
    esp_fill_random(r, sizeof(r));
    for (int i = 0; i < 16; i++) sprintf(out + i * 2, "%02x", r[i]);
    out[32] = 0;
    uint32_t now = stats_uptime_s();
    xSemaphoreTake(s_sess_lock, portMAX_DELAY);
    int slot = 0;
    for (int i = 0; i < SESSIONS; i++) {
        if (!s_sessions[i].used) {
            slot = i;
            break;
        }
        if (s_sessions[i].last_s < s_sessions[slot].last_s) slot = i;
    }
    token_hash(out, s_sessions[slot].hash);
    s_sessions[slot].created = epoch_now();
    s_sessions[slot].last_s = now;
    s_sessions[slot].used = true;
    sessions_save();
    xSemaphoreGive(s_sess_lock);
}

static bool session_ok(const char *tok, bool drop) {
    if (strlen(tok) != 32) return false;
    uint8_t want[32];
    token_hash(tok, want);
    int64_t epoch = epoch_now();
    bool ok = false;
    xSemaphoreTake(s_sess_lock, portMAX_DELAY);
    for (int i = 0; i < SESSIONS; i++) {
        session_t *s = &s_sessions[i];
        if (!s->used) continue;
        uint8_t diff = 0;
        for (int b = 0; b < 32; b++) diff |= s->hash[b] ^ want[b];
        if (diff) continue;
        if (drop || (epoch && s->created && epoch - s->created > SESSION_MAX_AGE_S)) {
            memset(s, 0, sizeof(*s));
            sessions_save();
            break;
        }
        s->last_s = stats_uptime_s();
        if (!s->created && epoch) s->created = epoch;  // signed in before the clock synced
        ok = true;
        break;
    }
    xSemaphoreGive(s_sess_lock);
    return ok;
}

static void drop_all_sessions(void) {
    xSemaphoreTake(s_sess_lock, portMAX_DELAY);
    memset(s_sessions, 0, sizeof(s_sessions));
    sessions_save();
    xSemaphoreGive(s_sess_lock);
}

// Brute-force brake: 8 bad passwords in 5 minutes lock logins for 5 minutes.
static bool login_locked(void) { return stats_uptime_s() < s_locked_until_s; }

static void note_login(bool ok) {
    uint32_t now = stats_uptime_s();
    if (ok) {
        s_fail_count = 0;
        return;
    }
    if (now - s_fail_window_s > 300) {
        s_fail_window_s = now;
        s_fail_count = 0;
    }
    if (++s_fail_count >= 8) {
        s_locked_until_s = now + 300;
        s_fail_count = 0;
        stats_event(EV_WARN, "Too many wrong passwords: logins locked for 5 minutes");
    }
}

static bool check_basic(httpd_req_t *req) {
    char hdr[200];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) return false;
    if (strncmp(hdr, "Basic ", 6) != 0) return false;
    unsigned char dec[150];
    size_t dl = 0;
    if (mbedtls_base64_decode(dec, sizeof(dec) - 1, &dl, (const unsigned char *)hdr + 6, strlen(hdr + 6)) != 0)
        return false;
    dec[dl] = 0;
    char *colon = strchr((char *)dec, ':');
    if (colon == NULL) return false;
    *colon = 0;
    if (strcmp((char *)dec, "admin") != 0 || login_locked()) return false;
    bool ok = settings_check_password(colon + 1);
    note_login(ok);
    return ok;
}

// How the request authenticated: browser session, Basic (scripts) or Bearer API token (Zigbee Studio).
typedef enum { AUTH_NONE = 0, AUTH_SESSION, AUTH_BASIC, AUTH_TOKEN } auth_kind_t;

static bool check_bearer(httpd_req_t *req) {
    char hdr[96];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) return false;
    if (strncmp(hdr, "Bearer ", 7) != 0) return false;
    return settings_check_api_token(hdr + 7);
}

static auth_kind_t auth_kind(httpd_req_t *req) {
    if (!settings_has_password()) return AUTH_NONE;
    char tok[48];
    size_t tl = sizeof(tok);
    if (httpd_req_get_cookie_val(req, "zb_session", tok, &tl) == ESP_OK && session_ok(tok, false)) return AUTH_SESSION;
    if (check_bearer(req)) return AUTH_TOKEN;
    if (check_basic(req)) return AUTH_BASIC;
    return AUTH_NONE;
}

static bool authed(httpd_req_t *req) { return auth_kind(req) != AUTH_NONE; }

// Cross-site pages can't add custom headers without a CORS preflight, which we never answer.
// Header-authenticated callers (token, Basic) carry no ambient browser credentials, so they skip it.
static bool csrf_ok(httpd_req_t *req) {
    char v[8];
    return httpd_req_get_hdr_value_str(req, "X-Bridge", v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0;
}

// ---------------------------------------------------------------- helpers

static esp_err_t send_json(httpd_req_t *req, cJSON *root) {
    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (s == NULL) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(req, s);
    free(s);
    return e;
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg) {
    httpd_resp_set_status(req, status);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", msg);
    return send_json(req, r);
}

static esp_err_t send_ok(httpd_req_t *req) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return send_json(req, r);
}

static esp_err_t deny(httpd_req_t *req) {
    if (!settings_has_password()) return send_error(req, "403 Forbidden", "setup required");
    return send_error(req, "401 Unauthorized", "login required");
}

#define REQUIRE_AUTH(req)                      \
    do {                                       \
        if (!authed(req)) return deny(req);    \
    } while (0)
#define REQUIRE_CHANGE(req)                                                                \
    do {                                                                                   \
        auth_kind_t _k = auth_kind(req);                                                   \
        if (_k == AUTH_NONE) return deny(req);                                             \
        if (_k == AUTH_SESSION && !csrf_ok(req))                                           \
            return send_error(req, "403 Forbidden", "missing X-Bridge");                   \
    } while (0)
// Password and token management need the dashboard (a logged-in browser), not the API token.
#define REQUIRE_SESSION(req)                                                               \
    do {                                                                                   \
        auth_kind_t _k = auth_kind(req);                                                   \
        if (_k == AUTH_NONE) return deny(req);                                             \
        if (_k == AUTH_TOKEN) return send_error(req, "403 Forbidden", "not with the API token"); \
        if (_k == AUTH_SESSION && !csrf_ok(req))                                           \
            return send_error(req, "403 Forbidden", "missing X-Bridge");                   \
    } while (0)

static cJSON *read_json(httpd_req_t *req) {
    if (req->content_len <= 0 || req->content_len > 2048) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    int got = 0, quiet = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        // A client that goes quiet mid-body must not hold the server's only task: one receive timeout (5 s) and out.
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++quiet < 1) continue;
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        quiet = 0;
        got += r;
    }
    buf[got] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static void ip_str(uint32_t ip, char *out) { inet_ntoa_r(*(struct in_addr *)&ip, out, 16); }

// ---------------------------------------------------------------- streamed JSON
// The big answers (history, devices, events, log) are written straight to the socket in chunks:
// a cJSON tree of the 15-minute history alone takes ~400 KB, about all the free memory.

typedef struct {
    httpd_req_t *req;
    size_t n;
    esp_err_t err;
    char buf[1024];
} jw_t;

static void jw_flush(jw_t *w) {
    if (w->n && w->err == ESP_OK) w->err = httpd_resp_send_chunk(w->req, w->buf, w->n);
    w->n = 0;
}

static void jw_put(jw_t *w, const char *s, size_t len) {
    while (len) {
        size_t room = sizeof(w->buf) - w->n;
        size_t k = len < room ? len : room;
        memcpy(w->buf + w->n, s, k);
        w->n += k;
        s += k;
        len -= k;
        if (w->n == sizeof(w->buf)) jw_flush(w);
    }
}

static void jw_printf(jw_t *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jw_printf(jw_t *w, const char *fmt, ...) {
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) jw_put(w, tmp, n < (int)sizeof(tmp) ? (size_t)n : sizeof(tmp) - 1);
}

static void jw_str(jw_t *w, const char *s) {
    jw_put(w, "\"", 1);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            jw_put(w, e, 2);
        } else if (c == '\n') {
            jw_put(w, "\\n", 2);
        } else if (c < 0x20 || c == 0x7F) {
            char e[8];
            snprintf(e, sizeof(e), "\\u%04x", c);
            jw_put(w, e, 6);
        } else {  // UTF-8 (°C in the log) passes through as is: escaping each byte garbled it
            jw_put(w, (const char *)&c, 1);
        }
    }
    jw_put(w, "\"", 1);
}

static void jw_begin(jw_t *w, httpd_req_t *req) {
    w->req = req;
    w->n = 0;
    w->err = ESP_OK;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
}

static esp_err_t jw_end(jw_t *w) {
    jw_flush(w);
    if (w->err == ESP_OK) w->err = httpd_resp_send_chunk(w->req, NULL, 0);
    return w->err;
}

static void set_session_cookie(httpd_req_t *req, const char *tok) {
    static char cookie[128];  // httpd keeps the pointer until the response is sent
    snprintf(cookie, sizeof(cookie), "zb_session=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=31536000", tok);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
}

static void schedule_reboot(int ms) {
    static esp_timer_handle_t t;
    if (t == NULL) {
        const esp_timer_create_args_t a = {.callback = (esp_timer_cb_t)esp_restart, .name = "reboot"};
        esp_timer_create(&a, &t);
    }
    esp_timer_start_once(t, (uint64_t)ms * 1000);
}

// ---------------------------------------------------------------- page + session endpoints

static esp_err_t h_index(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    return httpd_resp_send(req, (const char *)index_gz_start, index_gz_end - index_gz_start);
}

static esp_err_t h_state(httpd_req_t *req) {
    bridge_settings_t cfg;
    settings_get(&cfg);
    // Each visit renews the cookie for another year, so a browser that keeps coming back stays signed in.
    char tok[48];
    size_t tl = sizeof(tok);
    if (settings_has_password() && httpd_req_get_cookie_val(req, "zb_session", tok, &tl) == ESP_OK && session_ok(tok, false))
        set_session_cookie(req, tok);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "setup_required", !settings_has_password());
    cJSON_AddBoolToObject(r, "authed", authed(req));
    cJSON_AddBoolToObject(r, "locked", login_locked());
    cJSON_AddBoolToObject(r, "api_token_set", settings_has_api_token());
    cJSON_AddStringToObject(r, "api", "zbbridge/1");
    cJSON_AddStringToObject(r, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(r, "hostname", cfg.hostname);
    return send_json(req, r);
}

static bool password_ok(const char *pw, char *why, size_t n) {
    size_t l = pw ? strlen(pw) : 0;
    if (l < 8) {
        snprintf(why, n, "Use at least 8 characters");
        return false;
    }
    if (l > 64) {
        snprintf(why, n, "Use at most 64 characters");
        return false;
    }
    return true;
}

static esp_err_t h_setup(httpd_req_t *req) {
    if (settings_has_password()) return send_error(req, "409 Conflict", "already set up");
    if (!csrf_ok(req)) return send_error(req, "403 Forbidden", "missing X-Bridge");
    cJSON *j = read_json(req);
    const cJSON *pw = j ? cJSON_GetObjectItem(j, "password") : NULL;
    char why[64];
    if (!cJSON_IsString(pw) || !password_ok(pw->valuestring, why, sizeof(why))) {
        cJSON_Delete(j);
        return send_error(req, "400 Bad Request", cJSON_IsString(pw) ? why : "password missing");
    }
    esp_err_t e = settings_set_password(pw->valuestring);
    cJSON_Delete(j);
    if (e != ESP_OK) return send_error(req, "500 Internal Server Error", esp_err_to_name(e));
    char tok[33];
    new_session(tok);
    set_session_cookie(req, tok);
    stats_event(EV_INFO, "Admin password created (first-boot setup)");
    return send_ok(req);
}

static esp_err_t h_login(httpd_req_t *req) {
    if (!settings_has_password()) return send_error(req, "403 Forbidden", "setup required");
    if (!csrf_ok(req)) return send_error(req, "403 Forbidden", "missing X-Bridge");
    if (login_locked()) return send_error(req, "429 Too Many Requests", "Too many attempts; try again in 5 minutes");
    cJSON *j = read_json(req);
    const cJSON *pw = j ? cJSON_GetObjectItem(j, "password") : NULL;
    bool ok = cJSON_IsString(pw) && settings_check_password(pw->valuestring);
    cJSON_Delete(j);
    note_login(ok);
    if (!ok) return send_error(req, "401 Unauthorized", "Wrong password");
    char tok[33];
    new_session(tok);
    set_session_cookie(req, tok);
    return send_ok(req);
}

static esp_err_t h_logout(httpd_req_t *req) {
    char tok[48];
    size_t tl = sizeof(tok);
    if (httpd_req_get_cookie_val(req, "zb_session", tok, &tl) == ESP_OK) session_ok(tok, true);
    httpd_resp_set_hdr(req, "Set-Cookie", "zb_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return send_ok(req);
}

// ---------------------------------------------------------------- status

static void add_ash(cJSON *parent, const char *name, const ash_dir_stats_t *a) {
    cJSON *o = cJSON_AddObjectToObject(parent, name);
    cJSON_AddNumberToObject(o, "bytes", a->bytes);
    cJSON_AddNumberToObject(o, "frames", a->frames);
    cJSON_AddNumberToObject(o, "data", a->data);
    cJSON_AddNumberToObject(o, "ack", a->ack);
    cJSON_AddNumberToObject(o, "nak", a->nak);
    cJSON_AddNumberToObject(o, "rst", a->rst);
    cJSON_AddNumberToObject(o, "rstack", a->rstack);
    cJSON_AddNumberToObject(o, "error", a->error);
    cJSON_AddNumberToObject(o, "retx", a->retx);
    cJSON_AddNumberToObject(o, "crc_err", a->crc_err);
    cJSON_AddNumberToObject(o, "cancel", a->cancel);
    cJSON_AddNumberToObject(o, "substitute", a->substitute);
    cJSON_AddNumberToObject(o, "oversize", a->oversize);
}

static esp_err_t h_status(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    bridge_settings_t cfg;
    settings_get(&cfg);
    bridge_stats_t g;
    stats_lock();
    g = g_stats;
    stats_unlock();
    char reason[64];
    int health = stats_health(reason, sizeof(reason));
    char ip[16];
    uint32_t up = stats_uptime_s();

    cJSON *r = cJSON_CreateObject();
    sysinfo_add_json(r);
    char avail[33];
    updater_available(avail, sizeof(avail));
    cJSON_AddStringToObject(r, "update_available", avail);
    cJSON_AddNumberToObject(r, "uptime", up);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    cJSON_AddNumberToObject(r, "epoch", tv.tv_sec > 1700000000 ? (double)tv.tv_sec : 0);
    cJSON_AddNumberToObject(r, "stats_since", g.stats_since_s);

    cJSON *h = cJSON_AddObjectToObject(r, "health");
    cJSON_AddNumberToObject(h, "score", health);
    cJSON_AddStringToObject(h, "reason", reason);

    cJSON *eth = cJSON_AddObjectToObject(r, "eth");
    cJSON_AddBoolToObject(eth, "link", g.eth_link);
    cJSON_AddBoolToObject(eth, "has_ip", g.eth_has_ip);
    cJSON_AddNumberToObject(eth, "speed", g.eth_speed);
    cJSON_AddBoolToObject(eth, "full_duplex", g.eth_full_duplex);
    ip_str(g.ip, ip);
    cJSON_AddStringToObject(eth, "ip", ip);
    ip_str(g.mask, ip);
    cJSON_AddStringToObject(eth, "mask", ip);
    ip_str(g.gw, ip);
    cJSON_AddStringToObject(eth, "gw", ip);
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", g.mac[0], g.mac[1], g.mac[2], g.mac[3], g.mac[4],
             g.mac[5]);
    cJSON_AddStringToObject(eth, "mac", mac);
    cJSON_AddStringToObject(eth, "mode", cfg.use_static ? "static" : "dhcp");
    cJSON_AddStringToObject(eth, "hostname", cfg.hostname);

    cJSON *d = cJSON_AddObjectToObject(r, "dongle");
    cJSON_AddBoolToObject(d, "present", g.dongle_present);
    cJSON_AddBoolToObject(d, "open", g.dongle_open);
    char vp[10];
    snprintf(vp, sizeof(vp), "%04x:%04x", g.dongle_vid, g.dongle_pid);
    cJSON_AddStringToObject(d, "usb_id", vp);
    cJSON_AddStringToObject(d, "manufacturer", g.dongle_mfr);
    cJSON_AddStringToObject(d, "product", g.dongle_product);
    cJSON_AddStringToObject(d, "serial", g.dongle_serial);
    cJSON_AddNumberToObject(d, "since", g.dongle_since_s);
    cJSON_AddNumberToObject(d, "baud", cfg.baud);
    {
        dongle_info_t di;
        dongle_info(&di);
        const dongle_model_t *m = di.model;
        proto_t pr = di.proto_detected != PROTO_UNKNOWN ? di.proto_detected : m->proto;
        cJSON_AddStringToObject(d, "usb_chip", us_chip_name(di.chip));
        cJSON_AddStringToObject(d, "model", m->id);
        cJSON_AddStringToObject(d, "model_name", m->name);
        cJSON_AddStringToObject(d, "radio", m->radio);
        cJSON_AddStringToObject(d, "family", family_name(m->family));
        cJSON_AddStringToObject(d, "protocol", proto_name(pr));
        cJSON_AddBoolToObject(d, "protocol_detected", di.proto_detected != PROTO_UNKNOWN);
        cJSON_AddStringToObject(d, "z2m_adapter", proto_z2m_adapter(pr));
        cJSON_AddNumberToObject(d, "model_baud", m->baud);
        cJSON_AddBoolToObject(d, "rtscts_wired", m->hw_flow);
        cJSON_AddStringToObject(d, "bootloader_entry", reset_name(m->reset));
        cJSON_AddStringToObject(d, "fw_board", m->fw_board ? m->fw_board : "");
        cJSON_AddBoolToObject(d, "flashable", m->family == FAM_SILABS || m->family == FAM_UNKNOWN);
        cJSON_AddStringToObject(d, "note", m->note ? m->note : "");
        cJSON_AddBoolToObject(d, "model_downloaded", dongle_is_extra(m));
        cJSON_AddStringToObject(d, "fw_running", di.fw_running);
    }

    cJSON *c = cJSON_AddObjectToObject(r, "client");
    cJSON_AddBoolToObject(c, "connected", g.client_connected);
    ip_str(g.client_ip, ip);
    cJSON_AddStringToObject(c, "ip", g.client_connected ? ip : "");
    cJSON_AddNumberToObject(c, "port", g.client_port);
    cJSON_AddNumberToObject(c, "since", g.client_since_s);
    cJSON_AddStringToObject(c, "allowed", cfg.allow);
    cJSON_AddNumberToObject(c, "tcp_port", cfg.tcp_port);

    cJSON *n = cJSON_AddObjectToObject(r, "ncp");
    cJSON_AddNumberToObject(n, "ezsp", g.ezsp_version);
    char sv[24];
    uint16_t v = g.stack_version;
    if (v) snprintf(sv, sizeof(sv), "%u.%u.%u.%u", (v >> 12) & 15, (v >> 8) & 15, (v >> 4) & 15, v & 15);
    else sv[0] = 0;
    cJSON_AddStringToObject(n, "stack", sv);
    cJSON_AddNumberToObject(n, "reset_code", g.last_reset_code);
    cJSON_AddNumberToObject(n, "error_code", g.last_error_code);
    cJSON_AddStringToObject(n, "network", !g.network_up_known ? "unknown" : g.network_up ? "up" : "down");

    cJSON *k = cJSON_AddObjectToObject(r, "counters");
    add_ash(k, "host", &g.host);
    add_ash(k, "ncp", &g.ncp);
    cJSON_AddNumberToObject(k, "ezsp_cmds", g.ezsp_cmds);
    cJSON_AddNumberToObject(k, "ezsp_responses", g.ezsp_responses);
    cJSON_AddNumberToObject(k, "ezsp_callbacks", g.ezsp_callbacks);
    cJSON_AddNumberToObject(k, "zb_rx", g.zb_rx);
    cJSON_AddNumberToObject(k, "zb_unicast", g.zb_unicast);
    cJSON_AddNumberToObject(k, "zb_broadcast", g.zb_broadcast);
    cJSON_AddNumberToObject(k, "zb_multicast", g.zb_multicast);
    cJSON_AddNumberToObject(k, "zb_sent_ok", g.zb_sent_ok);
    cJSON_AddNumberToObject(k, "zb_sent_fail", g.zb_sent_fail);
    cJSON_AddNumberToObject(k, "route_errors", g.route_errors);
    cJSON_AddNumberToObject(k, "net_status", g.net_status_events);
    cJSON_AddNumberToObject(k, "joins", g.joins);
    cJSON_AddNumberToObject(k, "leaves", g.leaves);
    cJSON_AddNumberToObject(k, "id_conflicts", g.id_conflicts);
    cJSON_AddNumberToObject(k, "rtt_avg_ms", g.rtt_n ? (double)g.rtt_sum_us / g.rtt_n / 1000.0 : 0);
    cJSON_AddNumberToObject(k, "rtt_max_ms", g.rtt_max_us / 1000.0);
    cJSON_AddNumberToObject(k, "rtt_n", g.rtt_n);
    cJSON *rh = cJSON_AddArrayToObject(k, "rtt_hist");
    for (int i = 0; i < RTT_BUCKETS; i++) cJSON_AddItemToArray(rh, cJSON_CreateNumber(g.rtt_hist[i]));
    cJSON_AddNumberToObject(k, "ack_avg_ms", g.ack_n ? (double)g.ack_sum_us / g.ack_n / 1000.0 : 0);
    cJSON_AddNumberToObject(k, "ack_max_ms", g.ack_max_us / 1000.0);
    {
        dongle_flow_t f;
        dongle_flow_stats(&f);
        cJSON *fl = cJSON_AddObjectToObject(k, "flow");
        cJSON_AddNumberToObject(fl, "xon_rx", f.st.xon_rx);
        cJSON_AddNumberToObject(fl, "xoff_rx", f.st.xoff_rx);
        cJSON_AddBoolToObject(fl, "xoff_now", f.xoff_now);
        cJSON_AddNumberToObject(fl, "xoff_pauses", f.st.pauses);
        cJSON_AddNumberToObject(fl, "xoff_timeouts", f.st.pause_timeouts);
        cJSON_AddNumberToObject(fl, "xoff_pause_avg_ms", f.st.pauses ? (double)f.st.pause_us_sum / f.st.pauses / 1000.0 : 0);
        cJSON_AddNumberToObject(fl, "xoff_pause_max_ms", f.st.pause_max_us / 1000.0);
        cJSON_AddNumberToObject(fl, "writes", f.st.writes);
        cJSON_AddNumberToObject(fl, "chunks", f.st.chunks);
        cJSON_AddNumberToObject(fl, "paced", f.st.paced);
        cJSON_AddNumberToObject(fl, "pace_avg_us", f.st.paced ? (double)f.st.pace_us_sum / f.st.paced : 0);
        cJSON_AddNumberToObject(fl, "pace_max_us", f.st.pace_max_us);
        cJSON_AddNumberToObject(fl, "write_max_ms", f.st.write_max_us / 1000.0);
        cJSON_AddNumberToObject(fl, "write_errors", f.st.write_errors);
        cJSON_AddNumberToObject(fl, "queue_hwm", f.st.queue_hwm);
        cJSON_AddNumberToObject(fl, "chunk", f.cfg.chunk);
        cJSON_AddNumberToObject(fl, "gap_us", f.cfg.gap_us);
        cJSON_AddBoolToObject(fl, "xoff_honor", f.cfg.xoff_honor);
        cJSON_AddNumberToObject(fl, "xoff_max_ms", f.cfg.xoff_max_us / 1000);
        const dongle_line_t *L = &f.line;
        cJSON *ln = cJSON_AddObjectToObject(k, "line");
        cJSON_AddBoolToObject(ln, "known", L->known);
        cJSON_AddBoolToObject(ln, "cts_now", L->cts_now);
        cJSON_AddBoolToObject(ln, "hw_flow", L->hw_flow);
        cJSON_AddNumberToObject(ln, "samples", L->samples);
        cJSON_AddNumberToObject(ln, "cts_off", L->cts_off);
        cJSON_AddNumberToObject(ln, "cts_drops", L->cts_drops);
        cJSON_AddNumberToObject(ln, "status_polls", L->status_polls);
        cJSON_AddNumberToObject(ln, "hold_cts", L->hold_cts);
        cJSON_AddNumberToObject(ln, "hold_xon", L->hold_xon);
        cJSON_AddNumberToObject(ln, "err_framing", L->err_framing);
        cJSON_AddNumberToObject(ln, "err_hw_overrun", L->err_hw_overrun);
        cJSON_AddNumberToObject(ln, "err_queue_overrun", L->err_queue_overrun);
        cJSON_AddNumberToObject(ln, "err_parity", L->err_parity);
        cJSON_AddNumberToObject(ln, "err_break", L->err_break);
        cJSON_AddNumberToObject(ln, "in_queue_max", L->in_queue_max);
        cJSON_AddNumberToObject(ln, "out_queue_max", L->out_queue_max);
        cJSON_AddNumberToObject(ln, "poll_fail", L->poll_fail);
        cJSON_AddNumberToObject(ln, "hw_auto_off", L->hw_auto_off);
    }
    cJSON_AddNumberToObject(k, "clients_accepted", g.clients_accepted);
    cJSON_AddNumberToObject(k, "clients_refused", g.clients_refused);
    cJSON_AddNumberToObject(k, "client_drops", g.client_drops);
    cJSON_AddNumberToObject(k, "dropped_no_client", g.dropped_no_client);
    cJSON_AddNumberToObject(k, "dropped_overflow", g.dropped_overflow);
    cJSON_AddNumberToObject(k, "dongle_tx_errors", g.dongle_tx_errors);
    cJSON_AddNumberToObject(k, "dongle_attach", g.dongle_attach);
    cJSON_AddNumberToObject(k, "dongle_detach", g.dongle_detach);
    return send_json(req, r);
}

static uint32_t query_u32(httpd_req_t *req, const char *key);

// ?res=1 (default): the per-second ring (15 min). ?res=10[&n=<points>]: the 10-second ring (up to 24 h).
static esp_err_t h_history(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    bool lng = query_u32(req, "res") == 10;
    int cap = lng ? stats_long_history_len() : HIST_LEN;
    uint32_t want = query_u32(req, "n");
    if (want && (int)want < cap) cap = (int)want;
    if (cap <= 0) cap = 1;
    hist_t *hs = heap_caps_malloc(sizeof(hist_t) * cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hs) hs = malloc(sizeof(hist_t) * cap);
    if (!hs) return send_error(req, "500 Internal Server Error", "out of memory");
    int n = lng ? stats_copy_long_history(hs, cap) : stats_copy_history(hs, cap);
    static const char *const names[] = {"t", "in", "out", "rx", "ok", "fail", "cmds", "rtt", "rttmax", "err"};
    jw_t *w = malloc(sizeof(jw_t));
    if (!w) {
        free(hs);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    jw_begin(w, req);
    jw_put(w, "{", 1);
    for (int k = 0; k < 10; k++) {
        jw_printf(w, "%s\"%s\":[", k ? "," : "", names[k]);
        for (int i = 0; i < n && w->err == ESP_OK; i++) {
            const hist_t *h = &hs[i];
            const char *sep = i ? "," : "";
            switch (k) {
                case 0: jw_printf(w, "%s%lu", sep, (unsigned long)h->t); break;
                case 1: jw_printf(w, "%s%u", sep, h->bps_in); break;
                case 2: jw_printf(w, "%s%u", sep, h->bps_out); break;
                case 3: jw_printf(w, "%s%u", sep, h->zb_rx); break;
                case 4: jw_printf(w, "%s%u", sep, h->zb_tx_ok); break;
                case 5: jw_printf(w, "%s%u", sep, h->zb_tx_fail); break;
                case 6: jw_printf(w, "%s%u", sep, h->ezsp_cmds); break;
                case 7: jw_printf(w, "%s%u.%u", sep, h->rtt_avg_x10 / 10, h->rtt_avg_x10 % 10); break;
                case 8: jw_printf(w, "%s%u.%u", sep, h->rtt_max_x10 / 10, h->rtt_max_x10 % 10); break;
                default: jw_printf(w, "%s%u", sep, h->ash_err); break;
            }
        }
        jw_put(w, "]", 1);
    }
    jw_put(w, "}", 1);
    free(hs);
    esp_err_t e = jw_end(w);
    free(w);
    return e;
}

static uint32_t query_u32(httpd_req_t *req, const char *key) {
    char q[64], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return 0;
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) return 0;
    return strtoul(v, NULL, 10);
}

static esp_err_t h_events(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    uint32_t since = query_u32(req, "since");
    event_t *ev = malloc(sizeof(event_t) * EV_LEN);
    if (!ev) return send_error(req, "500 Internal Server Error", "out of memory");
    int n = stats_copy_events(ev, EV_LEN, since);
    jw_t *w = malloc(sizeof(jw_t));
    if (!w) {
        free(ev);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    jw_begin(w, req);
    jw_put(w, "{\"events\":[", 11);
    for (int i = 0; i < n; i++) {
        jw_printf(w, "%s{\"id\":%lu,\"up\":%lu,\"epoch\":%lld,\"level\":%u,\"text\":", i ? "," : "",
                  (unsigned long)ev[i].id, (unsigned long)ev[i].up_s, (long long)ev[i].epoch, ev[i].level);
        jw_str(w, ev[i].text);
        jw_put(w, "}", 1);
    }
    jw_printf(w, "],\"uptime\":%lu}", (unsigned long)stats_uptime_s());
    free(ev);
    esp_err_t e = jw_end(w);
    free(w);
    return e;
}

static esp_err_t h_devices(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    zb_device_t *dv = malloc(sizeof(zb_device_t) * DEV_MAX);
    if (!dv) return send_error(req, "500 Internal Server Error", "out of memory");
    int n = stats_copy_devices(dv, DEV_MAX);
    jw_t *w = malloc(sizeof(jw_t));
    if (!w) {
        free(dv);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    jw_begin(w, req);
    jw_printf(w, "{\"uptime\":%lu,\"devices\":[", (unsigned long)stats_uptime_s());
    for (int i = 0; i < n && w->err == ESP_OK; i++) {
        const zb_device_t *d = &dv[i];
        bool has = false;
        for (int b = 0; b < 8; b++) has |= d->ieee[b] != 0;
        char ieee[20] = "";
        if (has) {
            snprintf(ieee, sizeof(ieee), "0x%02x%02x%02x%02x%02x%02x%02x%02x", d->ieee[7], d->ieee[6], d->ieee[5],
                     d->ieee[4], d->ieee[3], d->ieee[2], d->ieee[1], d->ieee[0]);
        }
        jw_printf(w, "%s{\"short\":\"0x%04x\",\"ieee\":\"%s\",\"rx\":%lu,\"tx_ok\":%lu,\"tx_fail\":%lu,", i ? "," : "",
                  d->short_id, ieee, (unsigned long)d->rx, (unsigned long)d->tx_ok, (unsigned long)d->tx_fail);
        jw_printf(w, "\"route_err\":%lu,\"lqi\":%u,\"rssi\":%d,\"lqi_avg\":%d,\"rssi_avg\":%d,", (unsigned long)d->route_err,
                  d->last_lqi, d->last_rssi, (int)(d->lqi_avg + 0.5f), (int)(d->rssi_avg - 0.5f));
        jw_printf(w, "\"first\":%lu,\"last\":%lu,\"last_fail\":%lu,\"last_fail_status\":%lu}", (unsigned long)d->first_seen_s,
                  (unsigned long)d->last_seen_s, (unsigned long)d->last_fail_s, (unsigned long)d->last_fail_status);
    }
    jw_put(w, "]}", 2);
    free(dv);
    esp_err_t e = jw_end(w);
    free(w);
    return e;
}

static esp_err_t h_log(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    uint32_t since = query_u32(req, "since");
    char *buf = malloc(16 * 1024 + 1);
    jw_t *w = malloc(sizeof(jw_t));
    if (!buf || !w) {
        free(buf);
        free(w);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    uint32_t next;
    size_t n = logbuf_read(since, buf, 16 * 1024, &next);
    buf[n] = 0;
    jw_begin(w, req);
    jw_printf(w, "{\"next\":%lu,\"text\":", (unsigned long)next);
    jw_str(w, buf);
    jw_put(w, "}", 1);
    free(buf);
    esp_err_t e = jw_end(w);
    free(w);
    return e;
}

// ---------------------------------------------------------------- stored log (flash + SD card)

static esp_err_t h_logs_info(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    cJSON *r = cJSON_CreateObject();
    logstore_add_json(r);
    return send_json(req, r);
}

// ?src=flash[&tail=<bytes>] or ?src=sd&name=<file>; &download=1 to save it as a file.
static esp_err_t h_logs_text(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    char q[160] = "", src[8] = "flash", name[40] = "", v[16] = "";
    httpd_req_get_url_query_str(req, q, sizeof(q));
    httpd_query_key_value(q, "src", src, sizeof(src));
    bool download = httpd_query_key_value(q, "download", v, sizeof(v)) == ESP_OK && v[0] == '1';
    if (strcmp(src, "sd") == 0) {
        httpd_query_key_value(q, "name", name, sizeof(name));
        return sdlog_send_file(req, name, download);
    }
    uint32_t tail = 0;
    if (httpd_query_key_value(q, "tail", v, sizeof(v)) == ESP_OK) tail = strtoul(v, NULL, 10);
    return logstore_send(req, tail, download);
}

static esp_err_t h_logs_clear(httpd_req_t *req) {
    REQUIRE_SESSION(req);
    if (!logstore_clear()) return send_error(req, "503 Service Unavailable", "the stored log isn't running");
    stats_event(EV_INFO, "Stored log cleared");
    return send_ok(req);
}

// ---------------------------------------------------------------- settings

static cJSON *config_json(void) {
    bridge_settings_t c;
    settings_get(&c);
    char ip[16];
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "hostname", c.hostname);
    cJSON_AddStringToObject(r, "mode", c.use_static ? "static" : "dhcp");
    ip_str(c.ip, ip);
    cJSON_AddStringToObject(r, "ip", ip);
    ip_str(c.mask, ip);
    cJSON_AddStringToObject(r, "mask", ip);
    ip_str(c.gw, ip);
    cJSON_AddStringToObject(r, "gw", ip);
    ip_str(c.dns, ip);
    cJSON_AddStringToObject(r, "dns", ip);
    cJSON_AddStringToObject(r, "allow", c.allow);
    cJSON_AddNumberToObject(r, "tcp_port", c.tcp_port);
    cJSON_AddNumberToObject(r, "baud", c.baud);
    cJSON_AddNumberToObject(r, "tx_chunk", c.tx_chunk);
    cJSON_AddNumberToObject(r, "tx_gap_us", c.tx_gap_us);
    cJSON_AddBoolToObject(r, "xoff_honor", c.xoff_honor);
    cJSON_AddNumberToObject(r, "xoff_max_ms", c.xoff_max_ms);
    cJSON_AddBoolToObject(r, "hw_flow", c.hw_flow);
    cJSON_AddStringToObject(r, "fw_repo", c.fw_repo);
    cJSON_AddBoolToObject(r, "fw_prerelease", c.fw_prerelease);
    cJSON_AddStringToObject(r, "role", c.role == BRIDGE_ROLE_THREAD ? "thread" : "zigbee");
    cJSON_AddStringToObject(r, "update_url", c.update_url);
    cJSON_AddBoolToObject(r, "update_auto", c.update_auto);
    return r;
}

static esp_err_t h_config_get(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    return send_json(req, config_json());
}

static bool get_ip(const cJSON *j, const char *k, uint32_t *out) {
    const cJSON *v = cJSON_GetObjectItem(j, k);
    if (v == NULL) return true;  // keep
    if (!cJSON_IsString(v)) return false;
    if (v->valuestring[0] == 0) {
        *out = 0;
        return true;
    }
    uint32_t a = ipaddr_addr(v->valuestring);
    if (a == IPADDR_NONE) return false;
    *out = a;
    return true;
}

static esp_err_t h_config_post(httpd_req_t *req) {
    REQUIRE_CHANGE(req);
    cJSON *j = read_json(req);
    if (!j) return send_error(req, "400 Bad Request", "bad JSON");
    bridge_settings_t c, old;
    settings_get(&c);
    old = c;
    const cJSON *v;
    const char *bad = NULL;
    // Every field is checked as it comes: a value is never cut or wrapped into a different valid one.
    if ((v = cJSON_GetObjectItem(j, "hostname"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof(c.hostname)) bad = "Hostname: lowercase letters, digits and '-' (1-31)";
        else strlcpy(c.hostname, v->valuestring, sizeof(c.hostname));
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "mode"))) {
        if (!cJSON_IsString(v) || (strcmp(v->valuestring, "static") != 0 && strcmp(v->valuestring, "dhcp") != 0)) bad = "mode is static or dhcp";
        else c.use_static = strcmp(v->valuestring, "static") == 0;
    }
    if (!bad && !(get_ip(j, "ip", &c.ip) && get_ip(j, "mask", &c.mask) && get_ip(j, "gw", &c.gw) && get_ip(j, "dns", &c.dns)))
        bad = "An IP address field is not a valid IPv4 address";
    if (!bad && (v = cJSON_GetObjectItem(j, "allow"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof(c.allow)) bad = "Z2M host: use a.b.c.d, a.b.c.d/nn or leave empty";
        else strlcpy(c.allow, v->valuestring, sizeof(c.allow));
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "tcp_port"))) {
        if (!cJSON_IsNumber(v) || v->valuedouble < 1 || v->valuedouble > 65535 || v->valuedouble != (int)v->valuedouble) bad = "TCP port must be 1-65535 and not 80";
        else c.tcp_port = (uint16_t)v->valueint;
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "update_url"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof(c.update_url)) bad = "Update source: an http(s):// address up to 191 characters";
        else strlcpy(c.update_url, v->valuestring, sizeof(c.update_url));
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "update_auto"))) {
        if (!cJSON_IsBool(v)) bad = "update_auto is true or false";
        else c.update_auto = cJSON_IsTrue(v);
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "baud"))) {
        if (!cJSON_IsNumber(v) || v->valuedouble < 9600 || v->valuedouble > 921600) bad = "Baud rate must be 9600-921600";
        else c.baud = (uint32_t)v->valuedouble;
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "tx_chunk"))) {
        if (!cJSON_IsNumber(v) || v->valuedouble != (int)v->valuedouble || v->valuedouble > 64 ||
            (v->valuedouble != 0 && v->valuedouble < 8))
            bad = "USB chunk must be 8-64 bytes, or 0 for unpaced";
        else c.tx_chunk = (uint16_t)v->valueint;
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "tx_gap_us"))) {
        if (!cJSON_IsNumber(v) || v->valuedouble != (int)v->valuedouble || v->valuedouble < 0 || v->valuedouble > 5000)
            bad = "Chunk gap must be 0-5000 us";
        else c.tx_gap_us = (uint16_t)v->valueint;
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "xoff_honor"))) {
        if (!cJSON_IsBool(v)) bad = "xoff_honor is true or false";
        else c.xoff_honor = cJSON_IsTrue(v);
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "xoff_max_ms"))) {
        if (!cJSON_IsNumber(v) || v->valuedouble != (int)v->valuedouble || v->valuedouble < 1 || v->valuedouble > 200)
            bad = "XOFF hold limit must be 1-200 ms";
        else c.xoff_max_ms = (uint16_t)v->valueint;
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "fw_repo"))) {
        if (!cJSON_IsString(v) || strlen(v->valuestring) >= sizeof(c.fw_repo)) bad = "Firmware source: a GitHub owner/name up to 63 characters";
        else strlcpy(c.fw_repo, v->valuestring, sizeof(c.fw_repo));
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "role"))) {
        if (!cJSON_IsString(v) || (strcmp(v->valuestring, "zigbee") && strcmp(v->valuestring, "thread"))) bad = "role is zigbee or thread";
        else c.role = strcmp(v->valuestring, "thread") == 0 ? BRIDGE_ROLE_THREAD : BRIDGE_ROLE_ZIGBEE;
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "fw_prerelease"))) {
        if (!cJSON_IsBool(v)) bad = "fw_prerelease is true or false";
        else c.fw_prerelease = cJSON_IsTrue(v);
    }
    if (!bad && (v = cJSON_GetObjectItem(j, "hw_flow"))) {
        if (!cJSON_IsBool(v)) bad = "hw_flow is true or false";
        else c.hw_flow = cJSON_IsTrue(v);
    }
    cJSON_Delete(j);
    if (bad) return send_error(req, "400 Bad Request", bad);
    bool reboot = false;
    char err[96] = "";
    if (c.role != old.role)
        stats_event(EV_INFO, "Dongle role set to %s", c.role == BRIDGE_ROLE_THREAD ? "Thread RCP (OpenThread Border Router)" : "Zigbee coordinator (Zigbee2MQTT)");
    if (c.hw_flow != old.hw_flow) {
        // Applied before saving: a refusal (dongle not driving CTS) saves nothing.
        if (dongle_set_hw_flow(c.hw_flow, err, sizeof(err)) != ESP_OK && c.hw_flow) {
            char ignore[8];
            dongle_set_hw_flow(false, ignore, sizeof(ignore));  // drop the remembered wish
            return send_error(req, "400 Bad Request", err);
        }
        stats_event(EV_INFO, "Hardware flow control (CTS) %s", c.hw_flow ? "on" : "off");
    }
    if (settings_save(&c, &reboot, err, sizeof(err)) != ESP_OK) return send_error(req, "400 Bad Request", err);
    if (c.tx_chunk != old.tx_chunk || c.tx_gap_us != old.tx_gap_us || c.xoff_honor != old.xoff_honor ||
        c.xoff_max_ms != old.xoff_max_ms) {
        dongle_set_flow(c.tx_chunk, c.tx_gap_us, c.xoff_honor, c.xoff_max_ms);
        stats_event(EV_INFO, "Dongle pacing set: chunk %u, gap %u us, XOFF %s (max %u ms)", c.tx_chunk,
                    c.tx_gap_us, c.xoff_honor ? "honoured" : "counted only", c.xoff_max_ms);
    }
    if (strcmp(old.allow, c.allow) != 0) {
        stats_event(EV_INFO, "Z2M host set to '%s'", c.allow[0] ? c.allow : "anyone");
        // Drop a client the new rule no longer allows.
        stats_lock();
        bool conn = g_stats.client_connected;
        uint32_t cip = g_stats.client_ip;
        stats_unlock();
        if (conn && !settings_allow_peer(cip)) bridge_kick_client("no longer allowed by the Z2M host setting");
    }
    if (reboot) stats_event(EV_INFO, "Network/port/baud settings saved; reboot to apply");
    cJSON *r = config_json();
    cJSON_AddBoolToObject(r, "reboot_required", reboot);
    return send_json(req, r);
}

static esp_err_t h_token_new(httpd_req_t *req) {
    REQUIRE_SESSION(req);
    char tok[65];
    if (settings_new_api_token(tok) != ESP_OK) return send_error(req, "500 Internal Server Error", "save failed");
    stats_event(EV_INFO, "New API token created (the previous one no longer works)");
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "token", tok);
    return send_json(req, r);
}

static esp_err_t h_token_revoke(httpd_req_t *req) {
    REQUIRE_SESSION(req);
    settings_revoke_api_token();
    stats_event(EV_INFO, "API token revoked");
    return send_ok(req);
}

static esp_err_t h_password(httpd_req_t *req) {
    REQUIRE_SESSION(req);
    cJSON *j = read_json(req);
    const cJSON *cur = j ? cJSON_GetObjectItem(j, "current") : NULL;
    const cJSON *nw = j ? cJSON_GetObjectItem(j, "new") : NULL;
    char why[64];
    esp_err_t res;
    if (!cJSON_IsString(cur) || !settings_check_password(cur->valuestring)) {
        res = send_error(req, "403 Forbidden", "Current password is wrong");
    } else if (!cJSON_IsString(nw) || !password_ok(nw->valuestring, why, sizeof(why))) {
        res = send_error(req, "400 Bad Request", cJSON_IsString(nw) ? why : "new password missing");
    } else if (settings_set_password(nw->valuestring) != ESP_OK) {
        res = send_error(req, "500 Internal Server Error", "save failed");
    } else {
        drop_all_sessions();
        char tok[33];
        new_session(tok);
        set_session_cookie(req, tok);
        stats_event(EV_INFO, "Admin password changed");
        res = send_ok(req);
    }
    cJSON_Delete(j);
    return res;
}

static esp_err_t h_update_get(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    cJSON *r = cJSON_CreateObject();
    updater_add_json(r);
    cJSON *u = cJSON_DetachItemFromObject(r, "update");
    cJSON_Delete(r);
    return send_json(req, u);
}

static esp_err_t h_update_check(httpd_req_t *req) {
    REQUIRE_CHANGE(req);
    updater_check_now();
    return send_ok(req);
}

static esp_err_t h_update_install(httpd_req_t *req) {
    REQUIRE_CHANGE(req);
    if (ncpflash_busy()) return send_error(req, "409 Conflict", "The dongle's firmware is being flashed: wait for it to finish");
    char why[80];
    if (!updater_install(why, sizeof(why))) return send_error(req, "409 Conflict", why);
    return send_ok(req);
}

static esp_err_t h_reboot(httpd_req_t *req) {
    REQUIRE_CHANGE(req);
    if (ncpflash_busy()) return send_error(req, "409 Conflict", "The dongle's firmware is being flashed: wait for it to finish");
    stats_event(EV_INFO, "Reboot requested from the dashboard");
    schedule_reboot(800);
    return send_ok(req);
}

static esp_err_t h_stats_reset(httpd_req_t *req) {
    REQUIRE_CHANGE(req);
    stats_reset_counters();
    dongle_flow_reset_stats();
    return send_ok(req);
}

static esp_err_t h_factory(httpd_req_t *req) {
    REQUIRE_SESSION(req);
    if (ncpflash_busy()) return send_error(req, "409 Conflict", "The dongle's firmware is being flashed: wait for it to finish");
    cJSON *j = read_json(req);
    const cJSON *c = j ? cJSON_GetObjectItem(j, "confirm") : NULL;
    bool ok = cJSON_IsString(c) && strcmp(c->valuestring, "RESET") == 0;
    cJSON_Delete(j);
    if (!ok) return send_error(req, "400 Bad Request", "Send {\"confirm\":\"RESET\"}");
    settings_factory_reset();
    schedule_reboot(800);
    return send_ok(req);
}

// ---------------------------------------------------------------- firmware update

static esp_err_t h_ota(httpd_req_t *req) {
    REQUIRE_CHANGE(req);
    if (ncpflash_busy()) return send_error(req, "409 Conflict", "The dongle's firmware is being flashed: wait for it to finish");
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    int total = req->content_len;
    if (part == NULL) return send_error(req, "500 Internal Server Error", "no OTA partition");
    if (total < 1024 || (size_t)total > part->size) return send_error(req, "400 Bad Request", "Firmware size is wrong");

    if (!fw_write_lock()) return send_error(req, "409 Conflict", "A firmware update is already being installed");
    uint8_t *buf = malloc(4096);
    if (!buf) {
        fw_write_unlock();
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    mbedtls_md5_context md5;
    mbedtls_md5_init(&md5);
    mbedtls_md5_starts(&md5);
    int got = 0, have = 0;
    esp_ota_handle_t oh = 0;
    bool begun = false;
    const char *fail = NULL;
    char new_ver[33] = "";
    const size_t hdr_need = fw_header_len();
    stats_event(EV_INFO, "Firmware upload started (%d bytes)", total);

    int quiet = 0;
    while (got < total && fail == NULL) {
        int r = httpd_req_recv(req, (char *)buf + have, (4096 - have) < (total - got) ? (4096 - have) : (total - got));
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++quiet < 1) continue;
        if (r <= 0) {
            fail = r == HTTPD_SOCK_ERR_TIMEOUT ? "upload stalled" : "upload interrupted";
            break;
        }
        quiet = 0;
        mbedtls_md5_update(&md5, buf + have, r);
        got += r;
        have += r;
        if (!begun) {
            if ((size_t)have < hdr_need && got < total) continue;  // gather the headers first
            fail = fw_check_header(buf, have, new_ver);
            if (fail) break;
            esp_err_t e = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &oh);
            if (e != ESP_OK) {
                fail = "esp_ota_begin failed";
                break;
            }
            begun = true;
        }
        if (esp_ota_write(oh, buf, have) != ESP_OK) {
            fail = "flash write failed";
            break;
        }
        have = 0;
    }
    free(buf);
    uint8_t d_md5[16];
    char h_md5[33];
    mbedtls_md5_finish(&md5, d_md5);
    mbedtls_md5_free(&md5);
    for (int i = 0; i < 16; i++) snprintf(h_md5 + i * 2, 3, "%02x", d_md5[i]);
    if (fail == NULL) {
        esp_err_t e = esp_ota_end(oh);
        begun = false;
        if (e == ESP_ERR_OTA_VALIDATE_FAILED) fail = "Image failed verification";
        else if (e != ESP_OK) fail = "esp_ota_end failed";
        else if ((fail = fw_readback_md5(part, total, h_md5)) == NULL && esp_ota_set_boot_partition(part) != ESP_OK)
            fail = "could not select the new firmware";
    }
    if (begun) esp_ota_abort(oh);
    fw_write_unlock();
    if (fail) {
        stats_event(EV_ERROR, "Firmware upload failed: %s", fail);
        return send_error(req, "400 Bad Request", fail);
    }
    stats_event(EV_INFO, "Firmware %s written to %s and read back (MD5 %s); rebooting", new_ver, part->label, h_md5);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "version", new_ver);
    schedule_reboot(1500);
    return send_json(req, r);
}

// ---------------------------------------------------------------- the dongle's own firmware

static esp_err_t send_dongle_state(httpd_req_t *req) {
    cJSON *r = cJSON_CreateObject();
    ncpflash_add_json(r);
    cJSON *d = cJSON_DetachItemFromObject(r, "dongle_fw");
    if (d) fwcat_add_json(d);
    cJSON_Delete(r);
    return d ? send_json(req, d) : send_error(req, "500 Internal Server Error", "out of memory");
}

// The dongles the bridge knows (static table).
static esp_err_t h_dongles(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    cJSON *a = cJSON_CreateArray();
    const char *last = "";
    const dongle_model_t *ex = dongles_extra();
    int nex = dongles_extra_count();
    for (int i = 0; ex && i < nex; i++) {
        const dongle_model_t *m = &ex[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", m->id);
        cJSON_AddStringToObject(o, "name", m->name);
        char vp[10];
        snprintf(vp, sizeof(vp), "%04x:%04x", m->vid, m->pid);
        cJSON_AddStringToObject(o, "usb_id", vp);
        cJSON_AddStringToObject(o, "usb_chip", us_chip_name(us_chip_for(m->vid, m->pid, true)));
        cJSON_AddStringToObject(o, "radio", m->radio);
        cJSON_AddStringToObject(o, "family", family_name(m->family));
        cJSON_AddStringToObject(o, "protocol", proto_name(m->proto));
        cJSON_AddNumberToObject(o, "baud", m->baud);
        cJSON_AddBoolToObject(o, "rtscts_wired", m->hw_flow);
        cJSON_AddStringToObject(o, "bootloader_entry", reset_name(m->reset));
        cJSON_AddBoolToObject(o, "flashable", m->family == FAM_SILABS);
        cJSON_AddStringToObject(o, "note", m->note ? m->note : "");
        cJSON_AddBoolToObject(o, "downloaded", true);
        cJSON_AddItemToArray(a, o);
    }
    for (const dongle_model_t *m = dongle_models(); m->id; m++) {
        if (strcmp(m->id, last) == 0 && a->child) continue;  // same model, another USB ID
        last = m->id;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", m->id);
        cJSON_AddStringToObject(o, "name", m->name);
        char vp[10];
        snprintf(vp, sizeof(vp), "%04x:%04x", m->vid, m->pid);
        cJSON_AddStringToObject(o, "usb_id", vp);
        cJSON_AddStringToObject(o, "usb_chip", us_chip_name(us_chip_for(m->vid, m->pid, true)));
        cJSON_AddStringToObject(o, "radio", m->radio);
        cJSON_AddStringToObject(o, "family", family_name(m->family));
        cJSON_AddStringToObject(o, "protocol", proto_name(m->proto));
        cJSON_AddNumberToObject(o, "baud", m->baud);
        cJSON_AddBoolToObject(o, "rtscts_wired", m->hw_flow);
        cJSON_AddStringToObject(o, "bootloader_entry", reset_name(m->reset));
        cJSON_AddBoolToObject(o, "flashable", m->family == FAM_SILABS);
        cJSON_AddStringToObject(o, "note", m->note ? m->note : "");
        cJSON_AddItemToArray(a, o);
    }
    return send_json(req, a);
}

static esp_err_t h_dongle_get(httpd_req_t *req) {
    REQUIRE_AUTH(req);
    return send_dongle_state(req);
}

// "%41b" -> "Ab", and only printable ASCII kept.
static void url_decode(char *s) {
    char *o = s;
    for (char *p = s; *p; p++) {
        char c = *p;
        if (c == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char h[3] = {p[1], p[2], 0};
            c = (char)strtol(h, NULL, 16);
            p += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') *o++ = c;
    }
    *o = 0;
}

// Raw .gbl body; ?name=<file name>. Stored in the dongle image area, checked, kept only if it's fine.
static esp_err_t h_dongle_image(httpd_req_t *req) {
    REQUIRE_SESSION(req);
    char q[160], name[96] = "";
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "name", name, sizeof(name)) == ESP_OK)
        url_decode(name);
    int total = req->content_len;
    const char *fail = ncpflash_upload_begin(total > 0 ? (uint32_t)total : 0, name);
    if (fail) return send_error(req, ncpflash_busy() ? "409 Conflict" : "400 Bad Request", fail);
    uint8_t *buf = malloc(4096);
    if (!buf) {
        ncpflash_upload_end(false);
        return send_error(req, "500 Internal Server Error", "out of memory");
    }
    int got = 0, quiet = 0;
    while (got < total && fail == NULL) {
        int r = httpd_req_recv(req, (char *)buf, total - got < 4096 ? total - got : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++quiet < 1) continue;
        if (r <= 0) {
            fail = r == HTTPD_SOCK_ERR_TIMEOUT ? "upload stalled" : "upload interrupted";
            break;
        }
        quiet = 0;
        fail = ncpflash_upload_write(buf, r);
        got += r;
    }
    free(buf);
    const char *end = ncpflash_upload_end(fail == NULL && got == total);
    if (fail == NULL) fail = end;
    if (fail) return send_error(req, "400 Bad Request", fail);
    return send_dongle_state(req);
}

static esp_err_t h_dongle_delete(httpd_req_t *req) {
    REQUIRE_SESSION(req);
    const char *fail = ncpflash_delete();
    if (fail) return send_error(req, "409 Conflict", fail);
    return send_dongle_state(req);
}

// {"md5": "<the stored image's MD5>"}: flashes the dongle (Zigbee2MQTT is cut off meanwhile).
static esp_err_t h_fw_check(httpd_req_t *req) {
    REQUIRE_SESSION(req);
    fwcat_check_now();
    vTaskDelay(pdMS_TO_TICKS(50));  // so the answer already says "checking"
    return send_dongle_state(req);
}

static esp_err_t h_fw_download(httpd_req_t *req) {
    REQUIRE_SESSION(req);
    cJSON *j = read_json(req);
    const cJSON *n = j ? cJSON_GetObjectItem(j, "name") : NULL;
    char why[96];
    bool ok = cJSON_IsString(n) && fwcat_download(n->valuestring, why, sizeof(why));
    if (!cJSON_IsString(n)) strlcpy(why, "name missing", sizeof(why));
    cJSON_Delete(j);
    if (!ok) return send_error(req, "409 Conflict", why);
    return send_dongle_state(req);
}

static esp_err_t h_dongle_flash(httpd_req_t *req) {
    REQUIRE_SESSION(req);
    cJSON *j = read_json(req);
    const cJSON *m = j ? cJSON_GetObjectItem(j, "md5") : NULL;
    char why[96];
    bool ok = ncpflash_flash(cJSON_IsString(m) ? m->valuestring : NULL, why, sizeof(why));
    cJSON_Delete(j);
    if (!ok) return send_error(req, "409 Conflict", why);
    stats_event(EV_INFO, "Dongle flash requested from the dashboard");
    return send_dongle_state(req);
}

// ---------------------------------------------------------------- Prometheus

static esp_err_t h_metrics(httpd_req_t *req) {
    if (!authed(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"zigbee-eth-bridge\"");
        return httpd_resp_sendstr(req, "login required\n");
    }
    bridge_stats_t g;
    stats_lock();
    g = g_stats;
    stats_unlock();
    char reason[64];
    int health = stats_health(reason, sizeof(reason));
    dongle_flow_t fl;
    dongle_flow_stats(&fl);
    const int cap = 6144;
    char *b = malloc(cap);
    if (!b) return httpd_resp_send_500(req);
    int n = 0;
    // n never passes cap: a snprintf that doesn't fit is cut off and the rest are skipped
#define M(name, type, val) if (n < cap) n += snprintf(b + n, cap - n, "# TYPE zbbridge_" name " " type "\nzbbridge_" name " %.6g\n", (double)(val))
    M("uptime_seconds", "gauge", stats_uptime_s());
    M("health_score", "gauge", health);
    M("eth_link", "gauge", g.eth_link);
    M("dongle_connected", "gauge", g.dongle_open);
    M("client_connected", "gauge", g.client_connected);
    M("zigbee_network_up", "gauge", g.network_up_known ? g.network_up : -1);
    M("bytes_to_dongle_total", "counter", g.host.bytes);
    M("bytes_from_dongle_total", "counter", g.ncp.bytes);
    M("ash_crc_errors_total", "counter", g.host.crc_err + g.ncp.crc_err);
    M("ash_naks_total", "counter", g.host.nak + g.ncp.nak);
    M("ash_retransmits_total", "counter", g.host.retx + g.ncp.retx);
    M("ash_errors_total", "counter", g.ncp.error);
    M("ezsp_commands_total", "counter", g.ezsp_cmds);
    M("ezsp_callbacks_total", "counter", g.ezsp_callbacks);
    M("ezsp_rtt_avg_ms", "gauge", g.rtt_n ? (double)g.rtt_sum_us / g.rtt_n / 1000.0 : 0);
    M("ezsp_rtt_max_ms", "gauge", g.rtt_max_us / 1000.0);
    M("zigbee_rx_messages_total", "counter", g.zb_rx);
    M("zigbee_sent_ok_total", "counter", g.zb_sent_ok);
    M("zigbee_sent_failed_total", "counter", g.zb_sent_fail);
    M("zigbee_route_errors_total", "counter", g.route_errors);
    M("zigbee_joins_total", "counter", g.joins);
    M("zigbee_leaves_total", "counter", g.leaves);
    M("client_drops_total", "counter", g.client_drops);
    M("clients_refused_total", "counter", g.clients_refused);
    M("free_heap_bytes", "gauge", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    M("free_psram_bytes", "gauge", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    M("chip_temperature_celsius", "gauge", sysinfo_temperature());
    M("ncp_xoff_total", "counter", fl.st.xoff_rx);
    M("ncp_xon_total", "counter", fl.st.xon_rx);
    M("usb_out_xoff_pauses_total", "counter", fl.st.pauses);
    M("usb_out_xoff_timeouts_total", "counter", fl.st.pause_timeouts);
    M("usb_out_xoff_pause_max_ms", "gauge", fl.st.pause_max_us / 1000.0);
    M("usb_out_chunks_total", "counter", fl.st.chunks);
    M("usb_out_paced_total", "counter", fl.st.paced);
    M("usb_out_write_max_ms", "gauge", fl.st.write_max_us / 1000.0);
    M("usb_out_queue_hwm_bytes", "gauge", fl.st.queue_hwm);
    M("dongle_cts", "gauge", fl.line.cts_now);
    M("dongle_cts_drops_total", "counter", fl.line.cts_drops);
    M("cp2102n_uart_errors_total", "counter",
      fl.line.err_framing + fl.line.err_hw_overrun + fl.line.err_queue_overrun + fl.line.err_parity + fl.line.err_break);
    M("cp2102n_out_queue_max_bytes", "gauge", fl.line.out_queue_max);
#undef M
    if (n > cap - 1) n = cap - 1;
    httpd_resp_set_type(req, "text/plain; version=0.0.4");
    esp_err_t e = httpd_resp_send(req, b, n);
    free(b);
    return e;
}

#ifdef CONFIG_BRIDGE_TEST_HOOKS
static esp_err_t h_debug_watchdog(httpd_req_t *req) {
    REQUIRE_SESSION(req);
    char q[32], kind[8];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK || httpd_query_key_value(q, "kind", kind, sizeof(kind)) != ESP_OK ||
        !watchdog_test(kind))
        return send_error(req, "400 Bad Request", "kind is task, heap or net");
    return send_ok(req);
}
#endif

// ---------------------------------------------------------------- server

static bool s_web_running;
bool web_running(void) { return s_web_running; }

void web_start(void) {
    s_sess_lock = xSemaphoreCreateMutex();
    sessions_load();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 40;
    cfg.stack_size = 10240;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 12;
    cfg.recv_wait_timeout = 5;
    cfg.send_wait_timeout = 10;
    httpd_handle_t srv;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }
    const struct {
        const char *uri;
        httpd_method_t m;
        esp_err_t (*h)(httpd_req_t *);
    } routes[] = {
        {"/", HTTP_GET, h_index},
        {"/index.html", HTTP_GET, h_index},
        {"/api/state", HTTP_GET, h_state},
        {"/api/setup", HTTP_POST, h_setup},
        {"/api/login", HTTP_POST, h_login},
        {"/api/logout", HTTP_POST, h_logout},
        {"/api/status", HTTP_GET, h_status},
        {"/api/history", HTTP_GET, h_history},
        {"/api/events", HTTP_GET, h_events},
        {"/api/devices", HTTP_GET, h_devices},
        {"/api/log", HTTP_GET, h_log},
        {"/api/logs", HTTP_GET, h_logs_info},
        {"/api/logs/text", HTTP_GET, h_logs_text},
        {"/api/logs/clear", HTTP_POST, h_logs_clear},
        {"/api/config", HTTP_GET, h_config_get},
        {"/api/config", HTTP_POST, h_config_post},
        {"/api/password", HTTP_POST, h_password},
        {"/api/token", HTTP_POST, h_token_new},
        {"/api/token/revoke", HTTP_POST, h_token_revoke},
        {"/api/reboot", HTTP_POST, h_reboot},
        {"/api/stats/reset", HTTP_POST, h_stats_reset},
        {"/api/factory-reset", HTTP_POST, h_factory},
        {"/api/ota", HTTP_POST, h_ota},
        {"/api/update", HTTP_GET, h_update_get},
        {"/api/update/check", HTTP_POST, h_update_check},
        {"/api/update/install", HTTP_POST, h_update_install},
        {"/api/dongle", HTTP_GET, h_dongle_get},
        {"/api/dongles", HTTP_GET, h_dongles},
        {"/api/dongle/image", HTTP_POST, h_dongle_image},
        {"/api/dongle/image/delete", HTTP_POST, h_dongle_delete},
        {"/api/dongle/flash", HTTP_POST, h_dongle_flash},
        {"/api/dongle/firmware/check", HTTP_POST, h_fw_check},
        {"/api/dongle/firmware/download", HTTP_POST, h_fw_download},
        {"/metrics", HTTP_GET, h_metrics},
#ifdef CONFIG_BRIDGE_TEST_HOOKS
        {"/api/debug/watchdog", HTTP_POST, h_debug_watchdog},
#endif
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_uri_t u = {.uri = routes[i].uri, .method = routes[i].m, .handler = routes[i].h};
        httpd_register_uri_handler(srv, &u);
    }
    s_web_running = true;
    ESP_LOGI(TAG, "Dashboard on port 80 (%s)", settings_has_password() ? "login" : "first-boot setup");
}

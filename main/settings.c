#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "settings";
static const char *NS = "cfg";

#define PW_ITER 4000
#define PW_SALT_LEN 16
#define PW_HASH_LEN 32

static bridge_settings_t s_cfg;
static bridge_settings_t s_boot;  // what this boot runs with (network, port, baud apply at boot)
static uint32_t s_allow_addr, s_allow_mask;
static uint8_t s_pw_salt[PW_SALT_LEN], s_pw_hash[PW_HASH_LEN];
static bool s_has_pw;
static uint8_t s_tok_hash[32];
static bool s_has_tok;
static SemaphoreHandle_t s_lock;

static void defaults(bridge_settings_t *c) {
    memset(c, 0, sizeof(*c));
    strlcpy(c->hostname, CONFIG_BRIDGE_DEFAULT_HOSTNAME, sizeof(c->hostname));
#ifdef CONFIG_BRIDGE_DEFAULT_STATIC  // a bool option set to n is left undefined
    c->use_static = true;
#endif
    c->ip = ipaddr_addr(CONFIG_BRIDGE_DEFAULT_IP);
    c->mask = ipaddr_addr(CONFIG_BRIDGE_DEFAULT_MASK);
    c->gw = ipaddr_addr(CONFIG_BRIDGE_DEFAULT_GW);
    c->dns = ipaddr_addr(CONFIG_BRIDGE_DEFAULT_DNS);
    strlcpy(c->allow, CONFIG_BRIDGE_DEFAULT_ALLOW, sizeof(c->allow));
    c->tcp_port = CONFIG_BRIDGE_DEFAULT_TCP_PORT;
    c->baud = CONFIG_BRIDGE_DEFAULT_BAUD;
    c->tx_chunk = CONFIG_BRIDGE_DEFAULT_TX_CHUNK;
    c->tx_gap_us = CONFIG_BRIDGE_DEFAULT_TX_GAP_US;
#ifdef CONFIG_BRIDGE_DEFAULT_XOFF_HONOR
    c->xoff_honor = true;
#endif
    c->xoff_max_ms = CONFIG_BRIDGE_DEFAULT_XOFF_MAX_MS;
    strlcpy(c->fw_repo, CONFIG_BRIDGE_DEFAULT_FW_REPO, sizeof(c->fw_repo));
    c->fw_prerelease = true;
    strlcpy(c->update_url, CONFIG_BRIDGE_DEFAULT_UPDATE_URL, sizeof(c->update_url));
    c->update_auto = true;
}

bool settings_parse_allow(const char *s, uint32_t *addr, uint32_t *mask) {
    if (s == NULL || s[0] == '\0') {
        *addr = 0;
        *mask = 0;
        return true;
    }
    unsigned a, b, c, d, n = 32;
    char tail;
    int got = sscanf(s, "%u.%u.%u.%u/%u%c", &a, &b, &c, &d, &n, &tail);
    if (got == 4 && strchr(s, '/') != NULL) return false;
    if ((got != 4 && got != 5) || a > 255 || b > 255 || c > 255 || d > 255 || n > 32) return false;
    uint32_t host_mask = n == 0 ? 0 : 0xFFFFFFFFu << (32 - n);
    uint32_t host_addr = ((a << 24) | (b << 16) | (c << 8) | d) & host_mask;
    *addr = htonl(host_addr);
    *mask = htonl(host_mask);
    return true;
}

bool settings_allow_peer(uint32_t peer) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = (peer & s_allow_mask) == s_allow_addr;
    xSemaphoreGive(s_lock);
    return ok;
}

static void load(void) {
    defaults(&s_cfg);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_cfg.hostname);
        nvs_get_str(h, "hostname", s_cfg.hostname, &len);
        uint8_t st;
        if (nvs_get_u8(h, "static", &st) == ESP_OK) s_cfg.use_static = st != 0;
        nvs_get_u32(h, "ip", &s_cfg.ip);
        nvs_get_u32(h, "mask", &s_cfg.mask);
        nvs_get_u32(h, "gw", &s_cfg.gw);
        nvs_get_u32(h, "dns", &s_cfg.dns);
        len = sizeof(s_cfg.allow);
        nvs_get_str(h, "allow", s_cfg.allow, &len);
        nvs_get_u16(h, "port", &s_cfg.tcp_port);
        nvs_get_u32(h, "baud", &s_cfg.baud);
        nvs_get_u16(h, "tx_chunk", &s_cfg.tx_chunk);
        nvs_get_u16(h, "tx_gap", &s_cfg.tx_gap_us);
        uint8_t xh;
        if (nvs_get_u8(h, "xoff_on", &xh) == ESP_OK) s_cfg.xoff_honor = xh != 0;
        nvs_get_u16(h, "xoff_max", &s_cfg.xoff_max_ms);
        uint8_t hf;
        if (nvs_get_u8(h, "hw_flow", &hf) == ESP_OK) s_cfg.hw_flow = hf != 0;
        len = sizeof(s_cfg.fw_repo);
        nvs_get_str(h, "fw_repo", s_cfg.fw_repo, &len);
        uint8_t fp;
        if (nvs_get_u8(h, "fw_pre", &fp) == ESP_OK) s_cfg.fw_prerelease = fp != 0;
        nvs_get_u8(h, "role", &s_cfg.role);
        len = sizeof(s_cfg.update_url);
        nvs_get_str(h, "upd_url", s_cfg.update_url, &len);
        uint8_t ua;
        if (nvs_get_u8(h, "upd_auto", &ua) == ESP_OK) s_cfg.update_auto = ua != 0;
        len = sizeof(s_pw_salt);
        size_t hl = sizeof(s_pw_hash);
        s_has_pw = nvs_get_blob(h, "pw_salt", s_pw_salt, &len) == ESP_OK && len == PW_SALT_LEN &&
                   nvs_get_blob(h, "pw_hash", s_pw_hash, &hl) == ESP_OK && hl == PW_HASH_LEN;
        size_t tl = sizeof(s_tok_hash);
        s_has_tok = nvs_get_blob(h, "tok_hash", s_tok_hash, &tl) == ESP_OK && tl == sizeof(s_tok_hash);
        nvs_close(h);
    }
    if (!settings_parse_allow(s_cfg.allow, &s_allow_addr, &s_allow_mask)) {
        ESP_LOGW(TAG, "Saved Z2M host '%s' is invalid; allowing anyone", s_cfg.allow);
        s_cfg.allow[0] = '\0';
        s_allow_addr = s_allow_mask = 0;
    }
}

void settings_init(void) {
    s_lock = xSemaphoreCreateMutex();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    load();
    s_boot = s_cfg;
    ESP_LOGI(TAG, "Hostname %s, %s, Z2M host '%s', TCP %u, %lu baud, password %s", s_cfg.hostname,
             s_cfg.use_static ? "static IP" : "DHCP", s_cfg.allow[0] ? s_cfg.allow : "anyone", s_cfg.tcp_port,
             (unsigned long)s_cfg.baud, s_has_pw ? "set" : "NOT SET (first-boot setup)");
}

void settings_get(bridge_settings_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

static bool valid_hostname(const char *h) {
    size_t n = strlen(h);
    if (n == 0 || n > 31 || h[0] == '-' || h[n - 1] == '-') return false;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
    }
    return true;
}

static bool valid_mask(uint32_t m) {
    uint32_t h = ntohl(m);
    return h != 0 && ((~h + 1) & ~h) == 0;  // contiguous ones from the top
}

esp_err_t settings_save(const bridge_settings_t *in, bool *reboot_needed, char *err, size_t err_len) {
    uint32_t aa, am;
    if (!valid_hostname(in->hostname)) {
        snprintf(err, err_len, "Hostname: lowercase letters, digits and '-' (1-31)");
        return ESP_ERR_INVALID_ARG;
    }
    if (!settings_parse_allow(in->allow, &aa, &am)) {
        snprintf(err, err_len, "Z2M host: use a.b.c.d, a.b.c.d/nn or leave empty");
        return ESP_ERR_INVALID_ARG;
    }
    if (in->use_static) {
        if (in->ip == 0 || in->ip == IPADDR_NONE || !valid_mask(in->mask) || in->gw == IPADDR_NONE) {
            snprintf(err, err_len, "Static IP: check address, subnet mask and gateway");
            return ESP_ERR_INVALID_ARG;
        }
        if (in->gw != 0 && (in->gw & in->mask) != (in->ip & in->mask)) {
            snprintf(err, err_len, "Gateway is not in the static IP's subnet");
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (in->tcp_port < 1 || in->tcp_port == 80) {
        snprintf(err, err_len, "TCP port must be 1-65535 and not 80");
        return ESP_ERR_INVALID_ARG;
    }
    if (strncmp(in->update_url, "https://", 8) != 0 && strncmp(in->update_url, "http://", 7) != 0) {
        snprintf(err, err_len, "Update source must be an http(s):// address");
        return ESP_ERR_INVALID_ARG;
    }
    if (in->baud < 9600 || in->baud > 921600) {
        snprintf(err, err_len, "Baud rate must be 9600-921600");
        return ESP_ERR_INVALID_ARG;
    }
    if ((in->tx_chunk != 0 && in->tx_chunk < 8) || in->tx_chunk > 64) {
        snprintf(err, err_len, "USB chunk must be 8-64 bytes, or 0 for unpaced");
        return ESP_ERR_INVALID_ARG;
    }
    if (in->tx_gap_us > 5000) {
        snprintf(err, err_len, "Chunk gap must be 0-5000 us");
        return ESP_ERR_INVALID_ARG;
    }
    {
        // GitHub owner/name: letters, digits, '-', '_', '.', exactly one '/'
        int slash = 0;
        bool ok = in->fw_repo[0] != 0 && in->fw_repo[0] != '/';
        for (const char *q = in->fw_repo; *q && ok; q++) {
            if (*q == '/') slash++;
            else if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9') || *q == '-' ||
                       *q == '_' || *q == '.'))
                ok = false;
        }
        size_t rl = strlen(in->fw_repo);
        if (!ok || slash != 1 || in->fw_repo[rl - 1] == '/') {
            snprintf(err, err_len, "Firmware source: a GitHub owner/name like Nerivec/silabs-firmware-builder");
            return ESP_ERR_INVALID_ARG;
        }
    }
    if (in->role > BRIDGE_ROLE_THREAD) {
        snprintf(err, err_len, "Role is zigbee or thread");
        return ESP_ERR_INVALID_ARG;
    }
    if (in->xoff_max_ms < 1 || in->xoff_max_ms > 200) {
        snprintf(err, err_len, "XOFF hold limit must be 1-200 ms");
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        snprintf(err, err_len, "NVS open failed: %s", esp_err_to_name(e));
        return e;
    }
    nvs_set_str(h, "hostname", in->hostname);
    nvs_set_u8(h, "static", in->use_static ? 1 : 0);
    nvs_set_u32(h, "ip", in->ip);
    nvs_set_u32(h, "mask", in->mask);
    nvs_set_u32(h, "gw", in->gw);
    nvs_set_u32(h, "dns", in->dns);
    nvs_set_str(h, "allow", in->allow);
    nvs_set_u16(h, "port", in->tcp_port);
    nvs_set_u32(h, "baud", in->baud);
    nvs_set_u16(h, "tx_chunk", in->tx_chunk);
    nvs_set_u16(h, "tx_gap", in->tx_gap_us);
    nvs_set_u8(h, "xoff_on", in->xoff_honor ? 1 : 0);
    nvs_set_u16(h, "xoff_max", in->xoff_max_ms);
    nvs_set_u8(h, "hw_flow", in->hw_flow ? 1 : 0);
    nvs_set_str(h, "fw_repo", in->fw_repo);
    nvs_set_u8(h, "fw_pre", in->fw_prerelease ? 1 : 0);
    nvs_set_u8(h, "role", in->role);
    nvs_set_str(h, "upd_url", in->update_url);
    nvs_set_u8(h, "upd_auto", in->update_auto ? 1 : 0);
    e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        snprintf(err, err_len, "NVS commit failed: %s", esp_err_to_name(e));
        return e;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bridge_settings_t *b = &s_boot;
    bool reboot = strcmp(in->hostname, b->hostname) != 0 || in->use_static != b->use_static ||
                  (in->use_static && (in->ip != b->ip || in->mask != b->mask || in->gw != b->gw ||
                                      in->dns != b->dns)) ||
                  in->tcp_port != b->tcp_port || in->baud != b->baud;
    s_cfg = *in;
    // The Z2M host applies to the next connection without a reboot.
    s_allow_addr = aa;
    s_allow_mask = am;
    xSemaphoreGive(s_lock);
    *reboot_needed = reboot;
    ESP_LOGI(TAG, "Settings saved%s", reboot ? " (reboot to apply network/port/baud)" : "");
    return ESP_OK;
}

esp_err_t settings_save_link(uint32_t baud, bool hw_flow, char *err, size_t err_len) {
    bridge_settings_t c;
    settings_get(&c);
    c.baud = baud;
    c.hw_flow = hw_flow;
    bool reboot = false;
    esp_err_t e = settings_save(&c, &reboot, err, err_len);
    if (e != ESP_OK) return e;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_boot.baud = baud;  // already running at it
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

void settings_factory_reset(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "Factory reset: settings and password cleared");
}

static void sha256(const char *s, uint8_t *out) { mbedtls_sha256((const unsigned char *)s, strlen(s), out, 0); }

esp_err_t settings_new_api_token(char *out) {
    uint8_t r[32], hash[32];
    esp_fill_random(r, sizeof(r));
    for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", r[i]);
    out[64] = 0;
    sha256(out, hash);
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_blob(h, "tok_hash", hash, sizeof(hash));
    e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return e;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_tok_hash, hash, sizeof(hash));
    s_has_tok = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t settings_revoke_api_token(void) {
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_erase_key(h, "tok_hash");
    e = nvs_commit(h);
    nvs_close(h);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_has_tok = false;
    memset(s_tok_hash, 0, sizeof(s_tok_hash));
    xSemaphoreGive(s_lock);
    return e;
}

bool settings_has_api_token(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_has_tok;
    xSemaphoreGive(s_lock);
    return v;
}

bool settings_check_api_token(const char *tok) {
    if (tok == NULL || strlen(tok) != 64) return false;
    uint8_t got[32], want[32];
    sha256(tok, got);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has = s_has_tok;
    memcpy(want, s_tok_hash, sizeof(want));
    xSemaphoreGive(s_lock);
    if (!has) return false;
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= got[i] ^ want[i];
    return diff == 0;
}

bool settings_has_password(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_has_pw;
    xSemaphoreGive(s_lock);
    return v;
}

static void derive(const char *pw, const uint8_t *salt, uint8_t *out) {
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pw, strlen(pw), salt, PW_SALT_LEN,
                                  PW_ITER, PW_HASH_LEN, out);
}

esp_err_t settings_set_password(const char *pw) {
    uint8_t salt[PW_SALT_LEN], hash[PW_HASH_LEN];
    esp_fill_random(salt, sizeof(salt));
    derive(pw, salt, hash);
    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    nvs_set_blob(h, "pw_salt", salt, sizeof(salt));
    nvs_set_blob(h, "pw_hash", hash, sizeof(hash));
    e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) return e;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_pw_salt, salt, sizeof(salt));
    memcpy(s_pw_hash, hash, sizeof(hash));
    s_has_pw = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Admin password set");
    return ESP_OK;
}

bool settings_check_password(const char *pw) {
    uint8_t salt[PW_SALT_LEN], want[PW_HASH_LEN], got[PW_HASH_LEN];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool has = s_has_pw;
    memcpy(salt, s_pw_salt, sizeof(salt));
    memcpy(want, s_pw_hash, sizeof(want));
    xSemaphoreGive(s_lock);
    if (!has) return false;
    derive(pw, salt, got);
    uint8_t diff = 0;
    for (int i = 0; i < PW_HASH_LEN; i++) diff |= got[i] ^ want[i];
    return diff == 0;
}

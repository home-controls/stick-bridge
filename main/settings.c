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
static uint32_t s_allow_addr[RADIO_MAX], s_allow_mask[RADIO_MAX];  // network order
static radio_ident_t s_bound[RADIO_MAX];  // each radio's saved stick (NVS rb0..rb3)
static bool s_su[RADIO_MAX];              // set up: a client has connected to it (NVS r%d_su)
static uint32_t s_gen[RADIO_MAX];         // bumped when a radio is adopted or freed
static uint32_t s_present;                // bit i: radio i has its stick right now (dongle.c)
static uint8_t s_pw_salt[PW_SALT_LEN], s_pw_hash[PW_HASH_LEN];
static bool s_has_pw;
static volatile uint32_t s_pw_gen;  // bumped whenever the password changes (cached Basic logins drop)
static uint8_t s_tok_hash[32];
static bool s_has_tok;
static SemaphoreHandle_t s_lock;   // short: guards the fields above
static SemaphoreHandle_t s_wlock;  // one writer at a time (save, adopt, free, set up); taken before s_lock

static void apply_allow(const bridge_settings_t *c);

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
    for (int i = 0; i < RADIO_MAX; i++) radio_link_default(i, &c->radios[i]);
    c->usb_fs_only = true;
}

// radios[0] is the flat link above under another name.
static void sync_radio0(bridge_settings_t *c) {
    radio_link_t *r = &c->radios[0];
    r->tcp_port = c->tcp_port;
    r->baud = c->baud;
    r->role = c->role;
    strlcpy(r->allow, c->allow, sizeof(r->allow));
}

static void key(char *out, int radio, const char *what) { snprintf(out, 16, "r%d_%s", radio, what); }

bool settings_parse_allow(const char *s, uint32_t *addr, uint32_t *mask) {
    uint32_t a, m;
    if (!radio_parse_allow(s, &a, &m)) return false;
    *addr = htonl(a);
    *mask = htonl(m);
    return true;
}

bool settings_allow_peer_radio(int radio, uint32_t peer) {
    if (radio < 0 || radio >= RADIO_MAX) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = (peer & s_allow_mask[radio]) == s_allow_addr[radio];
    xSemaphoreGive(s_lock);
    return ok;
}

bool settings_allow_peer(uint32_t peer) { return settings_allow_peer_radio(0, peer); }

bool settings_radio_open_to_all(int radio) {
    if (radio < 0 || radio >= RADIO_MAX) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool open = s_allow_mask[radio] == 0;
    xSemaphoreGive(s_lock);
    return open;
}

void settings_radio_slots(radio_slot_t out[RADIO_MAX]) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < RADIO_MAX; i++) out[i].bound = s_bound[i];
    xSemaphoreGive(s_lock);
}

// Caller holds s_lock.
static uint32_t shown_mask_locked(void) {
    uint32_t m = 1u | s_present;
    for (int i = 0; i < RADIO_MAX; i++)
        if (s_su[i]) m |= 1u << i;
    return m;
}

uint32_t settings_radio_shown_mask(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t m = shown_mask_locked();
    xSemaphoreGive(s_lock);
    return m;
}

void settings_radio_present(int radio, bool present) {
    if (radio < 0 || radio >= RADIO_MAX) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (present) s_present |= 1u << radio;
    else s_present &= ~(1u << radio);
    xSemaphoreGive(s_lock);
}

uint32_t settings_radio_gen(int radio) {
    if (radio < 0 || radio >= RADIO_MAX) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t g = s_gen[radio];
    xSemaphoreGive(s_lock);
    return g;
}

bool settings_radio_is_set_up(int radio) {
    if (radio < 0 || radio >= RADIO_MAX) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool v = s_su[radio];
    xSemaphoreGive(s_lock);
    return v;
}

static void nvs_link(nvs_handle_t h, int i, const radio_link_t *r) {
    char k[16];
    snprintf(k, sizeof(k), "r%d_name", i);
    nvs_set_str(h, k, r->name);
    if (i == 0) return;  // the rest of radio 0 is the flat link
    snprintf(k, sizeof(k), "r%d_port", i);
    nvs_set_u16(h, k, r->tcp_port);
    snprintf(k, sizeof(k), "r%d_allow", i);
    nvs_set_str(h, k, r->allow);
    snprintf(k, sizeof(k), "r%d_baud", i);
    nvs_set_u32(h, k, r->baud);
    snprintf(k, sizeof(k), "r%d_role", i);
    nvs_set_u8(h, k, r->role);
}

static void nvs_su(nvs_handle_t h, int i, bool su) {
    char k[16];
    snprintf(k, sizeof(k), "r%d_su", i);
    nvs_set_u8(h, k, su ? 1 : 0);
}

static void nvs_bound(nvs_handle_t h, int i, const radio_ident_t *id) {
    char k[16];
    snprintf(k, sizeof(k), "rb%d", i);
    if (radio_ident_empty(id)) nvs_erase_key(h, k);
    else nvs_set_blob(h, k, id, sizeof(*id));
}

void settings_radio_mark_set_up(int radio) {
    if (radio < 0 || radio >= RADIO_MAX) return;
    xSemaphoreTake(s_wlock, portMAX_DELAY);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool bound = !radio_ident_empty(&s_bound[radio]);  // freed meanwhile (its stick left): nothing to set up
    xSemaphoreGive(s_lock);
    if (bound && !settings_radio_is_set_up(radio)) {
        nvs_handle_t h;
        if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_su(h, radio, true);
            nvs_commit(h);
            nvs_close(h);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_su[radio] = true;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "Radio %d is set up (a client connected)", radio + 1);
    }
    xSemaphoreGive(s_wlock);
}

void settings_radio_adopt(int radio, const radio_ident_t *id, radio_kind_t kind, uint32_t model_baud) {
    if (radio < 0 || radio >= RADIO_MAX) return;
    xSemaphoreTake(s_wlock, portMAX_DELAY);
    radio_link_t links[RADIO_MAX], fresh;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(links, s_cfg.radios, sizeof(links));
    uint32_t shown = shown_mask_locked() & ~(1u << radio);
    xSemaphoreGive(s_lock);
    if (radio != 0) radio_fresh_link(radio, kind, model_baud, links, shown, &fresh);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_bound(h, radio, id);
        nvs_su(h, radio, false);
        if (radio != 0) nvs_link(h, radio, &fresh);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_bound[radio] = *id;
    s_su[radio] = false;
    s_gen[radio]++;
    if (radio != 0) s_cfg.radios[radio] = fresh;
    apply_allow(&s_cfg);
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wlock);
}

void settings_radio_free(int radio) {
    if (radio < 0 || radio >= RADIO_MAX) return;
    radio_ident_t none;
    memset(&none, 0, sizeof(none));
    xSemaphoreTake(s_wlock, portMAX_DELAY);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_bound(h, radio, &none);
        nvs_su(h, radio, false);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_bound[radio] = none;
    s_su[radio] = false;
    s_gen[radio]++;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wlock);
}

void settings_radio_bind(int radio, const radio_ident_t *id) {
    if (radio < 0 || radio >= RADIO_MAX) return;
    char k[16];
    snprintf(k, sizeof(k), "rb%d", radio);
    (void)k;
    xSemaphoreTake(s_wlock, portMAX_DELAY);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_bound(h, radio, id);
        nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_bound[radio] = *id;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wlock);
}

uint8_t settings_radio_role(int radio) {
    if (radio < 0 || radio >= RADIO_MAX) radio = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t r = radio == 0 ? s_cfg.role : s_cfg.radios[radio].role;
    xSemaphoreGive(s_lock);
    return r;
}

void settings_radio_name(int radio, char *out, size_t n) {
    if (radio < 0 || radio >= RADIO_MAX) radio = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(out, s_cfg.radios[radio].name, n);
    xSemaphoreGive(s_lock);
}

uint16_t settings_radio_port(int radio) {
    if (radio < 0 || radio >= RADIO_MAX) radio = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint16_t p = radio == 0 ? s_cfg.tcp_port : s_cfg.radios[radio].tcp_port;
    xSemaphoreGive(s_lock);
    return p;
}

uint32_t settings_radio_baud(int radio) {
    if (radio < 0 || radio >= RADIO_MAX) radio = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t b = radio == 0 ? s_cfg.baud : s_cfg.radios[radio].baud;
    xSemaphoreGive(s_lock);
    return b;
}

// Allowed hosts of every radio from c into the lookup used for each connection (caller holds s_lock).
static void apply_allow(const bridge_settings_t *c) {
    for (int i = 0; i < RADIO_MAX; i++) {
        const char *a = i == 0 ? c->allow : c->radios[i].allow;
        if (!settings_parse_allow(a, &s_allow_addr[i], &s_allow_mask[i])) s_allow_addr[i] = s_allow_mask[i] = 0;
    }
}

// 1.x -> 2.0, once: radios turn on by themselves now, so a radio with a saved stick was in use (it counts as
// set up and waits for its stick), the old on/off keys go, and a port that clashed with a radio that was off
// moves.
static void migrate_v2(void) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t v = 0;
    if (nvs_get_u8(h, "rsv", &v) == ESP_OK && v >= 2) {
        nvs_close(h);
        return;
    }
    bool bound[RADIO_MAX], was_on[RADIO_MAX];
    char k[16];
    for (int i = 0; i < RADIO_MAX; i++) {
        bound[i] = !radio_ident_empty(&s_bound[i]);
        uint8_t on = 0;
        key(k, i, "on");
        was_on[i] = i == 0 || (nvs_get_u8(h, k, &on) == ESP_OK && on != 0);
    }
    uint32_t moved = radio_migrate_v2(s_cfg.radios, bound, was_on, s_su);
    for (int i = 0; i < RADIO_MAX; i++) {
        nvs_su(h, i, s_su[i]);
        key(k, i, "on");
        nvs_erase_key(h, k);
        if ((moved >> i) & 1u) {
            nvs_link(h, i, &s_cfg.radios[i]);
            ESP_LOGW(TAG, "Radio %d: link adjusted for 2.0: TCP %u, allowed host '%s'", i + 1, s_cfg.radios[i].tcp_port,
                     s_cfg.radios[i].allow);
        }
    }
    nvs_set_u8(h, "rsv", 2);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Radio settings moved to 2.0 (set up: %s%s%s%s)", s_su[0] ? "1 " : "", s_su[1] ? "2 " : "",
             s_su[2] ? "3 " : "", s_su[3] ? "4" : "");
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
        uint8_t fs;
        if (nvs_get_u8(h, "usb_fs", &fs) == ESP_OK) s_cfg.usb_fs_only = fs != 0;
        char k[16];
        for (int i = 0; i < RADIO_MAX; i++) {
            radio_link_t *r = &s_cfg.radios[i];
            key(k, i, "name");
            len = sizeof(r->name);
            nvs_get_str(h, k, r->name, &len);
            snprintf(k, sizeof(k), "rb%d", i);
            len = sizeof(s_bound[i]);
            if (nvs_get_blob(h, k, &s_bound[i], &len) != ESP_OK || len != sizeof(s_bound[i]))
                memset(&s_bound[i], 0, sizeof(s_bound[i]));
            s_bound[i].serial[sizeof(s_bound[i].serial) - 1] = 0;
            uint8_t su;
            key(k, i, "su");
            s_su[i] = nvs_get_u8(h, k, &su) == ESP_OK && su != 0;
            if (i == 0) continue;  // the rest of radio 0 is the flat link
            key(k, i, "port");
            nvs_get_u16(h, k, &r->tcp_port);
            key(k, i, "allow");
            len = sizeof(r->allow);
            nvs_get_str(h, k, r->allow, &len);
            key(k, i, "baud");
            nvs_get_u32(h, k, &r->baud);
            key(k, i, "role");
            nvs_get_u8(h, k, &r->role);
        }
        len = sizeof(s_pw_salt);
        size_t hl = sizeof(s_pw_hash);
        s_has_pw = nvs_get_blob(h, "pw_salt", s_pw_salt, &len) == ESP_OK && len == PW_SALT_LEN &&
                   nvs_get_blob(h, "pw_hash", s_pw_hash, &hl) == ESP_OK && hl == PW_HASH_LEN;
        size_t tl = sizeof(s_tok_hash);
        s_has_tok = nvs_get_blob(h, "tok_hash", s_tok_hash, &tl) == ESP_OK && tl == sizeof(s_tok_hash);
        nvs_close(h);
    }
    // The repository was renamed (zigbee-bridge -> stick-bridge, 2.1.3). GitHub redirects the old address, but a
    // bridge that saved the old default keeps it for good: move exactly that one to the new default.
    if (strcmp(s_cfg.update_url, "https://github.com/home-controls/zigbee-bridge/releases/latest/download/manifest.json") == 0) {
        ESP_LOGI(TAG, "Update source: the repository's old address, moved to the new default");
        strlcpy(s_cfg.update_url, CONFIG_BRIDGE_DEFAULT_UPDATE_URL, sizeof(s_cfg.update_url));
    }
    if (strncmp(s_cfg.update_url, "https://", 8) != 0) {  // saved by an older version that allowed http://
        ESP_LOGW(TAG, "Update source '%s' isn't https: back to the default", s_cfg.update_url);
        strlcpy(s_cfg.update_url, CONFIG_BRIDGE_DEFAULT_UPDATE_URL, sizeof(s_cfg.update_url));
    }
    // A saved rule that no longer reads (damaged settings) shuts the radio rather than opening it to the whole LAN:
    // the raw radio port gives whoever connects the network key. Set it again on Settings, Radios.
    uint32_t a, m;
    if (!settings_parse_allow(s_cfg.allow, &a, &m)) {
        ESP_LOGE(TAG, "Saved radio 1 allowed host '%s' is unreadable: nobody may connect until it is set again", s_cfg.allow);
        strlcpy(s_cfg.allow, ALLOW_NOBODY, sizeof(s_cfg.allow));
    }
    for (int i = 1; i < RADIO_MAX; i++) {
        if (!settings_parse_allow(s_cfg.radios[i].allow, &a, &m)) {
            ESP_LOGE(TAG, "Radio %d: saved allowed host '%s' is unreadable: nobody may connect until it is set again", i + 1,
                     s_cfg.radios[i].allow);
            strlcpy(s_cfg.radios[i].allow, ALLOW_NOBODY, sizeof(s_cfg.radios[i].allow));
        }
        if (s_cfg.radios[i].role > RADIO_ROLE_MAX) s_cfg.radios[i].role = RADIO_ROLE_ZHA;
    }
    if (s_cfg.role > RADIO_ROLE_MAX || s_cfg.role == RADIO_ROLE_ZHA) s_cfg.role = RADIO_ROLE_ZIGBEE;
    sync_radio0(&s_cfg);
    migrate_v2();
    uint32_t g0 = esp_random() | 1u;  // differs across boots: a page loaded before a reboot can't match by chance
    for (int i = 0; i < RADIO_MAX; i++) s_gen[i] = g0 + (uint32_t)i * 0x10000u;
    apply_allow(&s_cfg);
}

void settings_init(void) {
    s_lock = xSemaphoreCreateMutex();
    s_wlock = xSemaphoreCreateMutex();
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
    memcpy(out->gen, s_gen, sizeof(out->gen));
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

esp_err_t settings_save(const bridge_settings_t *in_raw, bool *reboot_needed, char *err, size_t err_len) {
    bridge_settings_t in_copy = *in_raw;
    bridge_settings_t *in = &in_copy;
    sync_radio0(in);
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
    // https only: over plain http anyone on the path could hand the bridge a manifest, and with it the image's hashes
    if (strncmp(in->update_url, "https://", 8) != 0) {
        snprintf(err, err_len, "Update source must be an https:// address");
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
    if (in->role > RADIO_ROLE_MAX) {
        snprintf(err, err_len, "Role is zigbee, thread or zwave");
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_wlock, portMAX_DELAY);
    // A radio a stick took (or that was freed) since the caller read the settings keeps what it has now.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 1; i < RADIO_MAX; i++)
        if (in->gen[i] != s_gen[i]) in->radios[i] = s_cfg.radios[i];
    memcpy(in->gen, s_gen, sizeof(in->gen));
    uint32_t shown = shown_mask_locked();
    xSemaphoreGive(s_lock);
    if (!radio_check_links(in->radios, shown, err, err_len)) {
        xSemaphoreGive(s_wlock);
        return ESP_ERR_INVALID_ARG;
    }
    if (in->xoff_max_ms < 1 || in->xoff_max_ms > 200) {
        snprintf(err, err_len, "XOFF hold limit must be 1-200 ms");
        xSemaphoreGive(s_wlock);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t e = nvs_open(NS, NVS_READWRITE, &h);
    if (e != ESP_OK) {
        snprintf(err, err_len, "NVS open failed: %s", esp_err_to_name(e));
        xSemaphoreGive(s_wlock);
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
    nvs_set_u8(h, "usb_fs", in->usb_fs_only ? 1 : 0);
    for (int i = 0; i < RADIO_MAX; i++) nvs_link(h, i, &in->radios[i]);
    e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        snprintf(err, err_len, "NVS commit failed: %s", esp_err_to_name(e));
        xSemaphoreGive(s_wlock);
        return e;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bridge_settings_t *b = &s_boot;
    bool reboot = strcmp(in->hostname, b->hostname) != 0 || in->use_static != b->use_static ||
                  (in->use_static && (in->ip != b->ip || in->mask != b->mask || in->gw != b->gw ||
                                      in->dns != b->dns)) ||
                  in->usb_fs_only != b->usb_fs_only;
    s_cfg = *in;
    // Radio links apply live (web.c rebinds ports and switches bauds); allowed hosts on the next connection.
    apply_allow(&s_cfg);
    (void)aa;
    (void)am;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_wlock);
    *reboot_needed = reboot;
    ESP_LOGI(TAG, "Settings saved%s", reboot ? " (reboot to apply the network settings)" : "");
    return ESP_OK;
}

esp_err_t settings_save_link_radio(int radio, uint32_t baud, bool hw_flow, char *err, size_t err_len) {
    if (radio == 0) return settings_save_link(baud, hw_flow, err, err_len);
    if (radio < 0 || radio >= RADIO_MAX) return ESP_ERR_INVALID_ARG;
    bridge_settings_t c;
    settings_get(&c);
    c.radios[radio].baud = baud;
    bool reboot = false;
    (void)hw_flow;
    return settings_save(&c, &reboot, err, err_len);
}

esp_err_t settings_save_link(uint32_t baud, bool hw_flow, char *err, size_t err_len) {
    bridge_settings_t c;
    settings_get(&c);
    c.baud = baud;
    c.hw_flow = hw_flow;
    bool reboot = false;
    return settings_save(&c, &reboot, err, err_len);
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
    s_pw_gen++;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "Admin password set");
    return ESP_OK;
}

uint32_t settings_password_gen(void) { return s_pw_gen; }

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

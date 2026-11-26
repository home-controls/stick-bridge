#pragma once
// Persistent settings (NVS namespace "cfg") and the admin password.
// There is no default password: until the user creates one on the dashboard, only the
// first-boot setup endpoint answers. A factory reset (5 quick power cycles) brings that back.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define BRIDGE_ROLE_ZIGBEE 0  // Zigbee coordinator for Zigbee2MQTT (zigbee_ncp builds)
#define BRIDGE_ROLE_THREAD 1  // OpenThread RCP for an OpenThread Border Router (openthread_rcp builds)

typedef struct {
    char hostname[32];
    bool use_static;
    uint32_t ip, mask, gw, dns;  // lwIP/esp_netif order (as in esp_ip4_addr_t.addr)
    char allow[24];              // Z2M host: "a.b.c.d", "a.b.c.d/nn" or "" (anyone)
    uint16_t tcp_port;
    uint32_t baud;
    // USB OUT pacing + software flow control (dongle_set_flow): apply at once, no reboot
    uint16_t tx_chunk;     // 0 (unpaced) or 8-64
    uint16_t tx_gap_us;    // 0-5000
    bool xoff_honor;
    uint16_t xoff_max_ms;  // 1-200
    bool hw_flow;          // CTS handshake in the CP2102N (dongle_set_hw_flow)
    char fw_repo[64];      // GitHub owner/name with dongle firmware releases (fwcat.c)
    bool fw_prerelease;    // include pre-releases (Nerivec publishes new SDKs as pre-releases first)
    uint8_t role;          // BRIDGE_ROLE_*: what the dongle is for (which firmware is offered/accepted)
    char update_url[192];  // release manifest on git (updater.c)
    bool update_auto;      // check for releases by itself
} bridge_settings_t;

void settings_init(void);
void settings_get(bridge_settings_t *out);
// Validates, saves and returns which parts need a reboot to take effect.
esp_err_t settings_save(const bridge_settings_t *s, bool *reboot_needed, char *err, size_t err_len);
void settings_factory_reset(void);
// Baud and CTS handshake after flashing dongle firmware that needs them: saved, and counted as applied
// (the bridge switched over live), so no reboot is asked for.
esp_err_t settings_save_link(uint32_t baud, bool hw_flow, char *err, size_t err_len);

// API token for other software (Zigbee Studio): only its SHA-256 is stored.
// settings_new_api_token writes a fresh 64-hex-char token into out (>= 65 bytes).
esp_err_t settings_new_api_token(char *out);
esp_err_t settings_revoke_api_token(void);
bool settings_has_api_token(void);
bool settings_check_api_token(const char *tok);

bool settings_has_password(void);
esp_err_t settings_set_password(const char *pw);
bool settings_check_password(const char *pw);

// Parses "a.b.c.d" or "a.b.c.d/nn" (empty = match all). Returns false on bad input.
bool settings_parse_allow(const char *s, uint32_t *addr, uint32_t *mask);
// Peer address in lwIP order. Uses the saved Z2M host.
bool settings_allow_peer(uint32_t peer);

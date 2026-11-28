#pragma once
// Persistent settings (NVS namespace "cfg") and the admin password.
// There is no default password: until the user creates one on the dashboard, only the
// first-boot setup endpoint answers. A factory reset (5 quick power cycles) brings that back.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "radio_core.h"

#define BRIDGE_ROLE_ZIGBEE RADIO_ROLE_ZIGBEE  // Zigbee coordinator for Zigbee2MQTT (zigbee_ncp builds)
#define BRIDGE_ROLE_THREAD RADIO_ROLE_THREAD  // OpenThread RCP for an OpenThread Border Router (openthread_rcp builds)
#define BRIDGE_ROLE_ZWAVE RADIO_ROLE_ZWAVE    // Z-Wave controller for Z-Wave JS (passed through)
#define BRIDGE_ROLE_ZHA RADIO_ROLE_ZHA        // Zigbee coordinator for ZHA (not radio 1; flashed like Zigbee)

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
    char update_url[192];  // release manifest for the custom train (updater.c)
    bool update_auto;      // check for releases by itself
    uint8_t update_train;  // ut_train_t (updtrain_core.h): stable, release candidates, or custom (update_url)
    // Every radio's link (radio_core.h). radios[0] is the radio above: its allow/tcp_port/baud/role are the flat
    // fields (those win on save); only its name is its own. The others have their own keys (r1_*, r2_*, r3_*).
    // All of it applies live: ports rebind, bauds switch, hosts apply to the next connection.
    radio_link_t radios[RADIO_MAX];
    uint32_t gen[RADIO_MAX];  // filled by settings_get: a radio adopted or freed since keeps its new link on save
    bool usb_fs_only;      // J3 refuses high speed so hubs run at full speed (applied at boot)
} bridge_settings_t;

void settings_init(void);
void settings_get(bridge_settings_t *out);
// Validates, saves and returns whether a reboot is needed (network settings and full-speed-only USB; radio
// links apply live, see web.c). A radio adopted or freed since `s` was read keeps its current link.
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
// Changes whenever the password is set: anything that remembered a good password checks this.
uint32_t settings_password_gen(void);

// Parses "a.b.c.d" or "a.b.c.d/nn" (empty = match all). Returns false on bad input.
bool settings_parse_allow(const char *s, uint32_t *addr, uint32_t *mask);
// Peer address in lwIP order. Uses the saved Z2M host.
bool settings_allow_peer(uint32_t peer);
bool settings_allow_peer_radio(int radio, uint32_t peer);
// The radio's rule is empty: any host on the network may connect.
bool settings_radio_open_to_all(int radio);
// What a rule that can't be read becomes: loopback, which no client on the network ever is.
#define ALLOW_NOBODY "127.0.0.1"

// Radios: which stick each keeps (radio_assign input), saving a radio's stick when it moved hub ports, and the
// live role / name / port / baud of one radio.
void settings_radio_slots(radio_slot_t out[RADIO_MAX]);
void settings_radio_bind(int radio, const radio_ident_t *id);
uint8_t settings_radio_role(int radio);
void settings_radio_name(int radio, char *out, size_t n);  // "" = unnamed (radio 0's too)
uint16_t settings_radio_port(int radio);
uint32_t settings_radio_baud(int radio);
// Radio lifecycle (radio_core.h): a new stick took radio i (its link is set up from scratch, except radio 0's,
// which keeps the user's; binding saved; not set up yet); radio i is free again (binding and set-up cleared:
// an unused stick left, a stale binding at boot, or Forget); a client connected to radio i (set up from now on).
void settings_radio_adopt(int radio, const radio_ident_t *id, radio_kind_t kind, uint32_t model_baud);
void settings_radio_free(int radio);
void settings_radio_mark_set_up(int radio);
bool settings_radio_is_set_up(int radio);
// dongle.c reports which radios have their stick; shown = radio 0, set up, or stick present.
void settings_radio_present(int radio, bool present);
uint32_t settings_radio_shown_mask(void);
uint32_t settings_radio_gen(int radio);
// settings_save_link for any radio (after flashing its stick with firmware at another baud).
esp_err_t settings_save_link_radio(int radio, uint32_t baud, bool hw_flow, char *err, size_t err_len);

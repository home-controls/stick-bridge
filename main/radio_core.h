#pragma once
// Several radios on one bridge (a USB hub on J3): which radio a USB stick belongs to, when a radio shows, the
// per-radio settings checks and the full-speed-only rule for hubs. Plain C with no ESP-IDF here, so
// test/host/test_radio.py checks it on the PC.
//
// A radio is a slot: its own stick, TCP port, allowed host, role and baud. Radios set themselves up: a stick
// plugged in takes the lowest free radio and its port opens at once. A radio is "set up" once a client has
// connected to it; unplugging the stick of a radio that isn't set up frees the radio again, while a set-up radio
// waits for its stick (and says so) until it comes back or is forgotten. Radio 0 is the one the bridge always
// had (Zigbee2MQTT on 6638, the EZSP decoder, the device table) and is always shown.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RADIO_MAX 4
#define RADIO_NONE (-1)
#define RADIO_PORT_BASE 6638  // radio i listens on RADIO_PORT_BASE + i unless that is taken

// Roles (settings role): what a radio's stick is for, which firmware is offered for it and which config
// snippet the dashboard shows.
#define RADIO_ROLE_ZIGBEE 0  // Zigbee coordinator for Zigbee2MQTT (zigbee_ncp builds)
#define RADIO_ROLE_THREAD 1  // OpenThread RCP for an OpenThread Border Router (openthread_rcp builds)
#define RADIO_ROLE_ZWAVE 2   // Z-Wave controller for Z-Wave JS: bytes pass through, no flashing here
#define RADIO_ROLE_ZHA 3     // Zigbee coordinator for Home Assistant's ZHA (same firmware as Zigbee); not radio 0
#define RADIO_ROLE_MAX RADIO_ROLE_ZHA

// Who a stick is. port: 0 = plugged straight into J3, n = port n of the hub on J3 (one hub level).
typedef struct {
    uint16_t vid, pid;
    char serial[48];
    uint8_t port;
} radio_ident_t;

typedef struct {
    radio_ident_t bound;  // the stick this radio belongs to; vid == 0 = free (the next new stick takes it)
} radio_slot_t;

typedef enum {
    RA_OK = 0,
    RA_BOUND,     // ok: matched the radio's saved stick
    RA_NEW,       // ok: a new stick took a free radio (caller sets the radio up for it and saves the binding)
    RA_REPLACED,  // ok: on J3 directly, radio 0 takes whatever is plugged in (caller saves the binding)
    RA_FULL,      // refused: every radio is in use or belongs to another stick
} radio_assign_t;

// Which radio gets a stick that just enumerated. in_use[i]: radio i has a stick open (or being opened).
// *result says how it matched; returns the radio index or RADIO_NONE.
//   1. on J3 itself (port 0) it is the only stick there can be: radio 0
//   2. a radio whose saved stick it is: same VID:PID and serial, or, when the serial is empty or that radio is
//      busy with a stick carrying the same serial, same VID:PID and hub port
//   3. the lowest free radio (no stick saved)
// A stick nobody knows never takes a radio saved for another stick, even if that stick is absent right now:
// at boot the sticks behind a hub enumerate one by one, and the Thread stick must not grab Zigbee's radio
// because the Zigbee stick hasn't shown up yet. A radio that isn't set up is freed when its stick leaves (and
// at boot if its stick never shows up); "Forget" on the dashboard frees a set-up one.
int radio_assign(const radio_slot_t slots[RADIO_MAX], const bool in_use[RADIO_MAX], const radio_ident_t *dev,
                 radio_assign_t *result);
bool radio_ident_empty(const radio_ident_t *id);
bool radio_ident_same(const radio_ident_t *a, const radio_ident_t *b);  // the same stick (serial, else port)

// One radio's link settings as the dashboard edits them.
typedef struct {
    char name[16];  // "" = by role ("Zigbee", "Thread", "Z-Wave", "ZHA")
    uint16_t tcp_port;
    char allow[24];  // client host: "a.b.c.d", "a.b.c.d/nn" or "" (anyone)
    uint32_t baud;
    uint8_t role;
} radio_link_t;

// Checks all radios' link settings together. Name and role always; port (unique among shown radios, not 80),
// baud and allowed host for the radios in shown_mask (bit i = radio i; radio 0 always counts as shown). On
// failure writes a message naming the radio and returns false.
bool radio_check_links(const radio_link_t links[RADIO_MAX], uint32_t shown_mask, char *err, size_t err_len);
// Parses "a.b.c.d" or "a.b.c.d/nn" (empty = anyone) into host-order address and mask.
bool radio_parse_allow(const char *s, uint32_t *addr_host, uint32_t *mask_host);
const char *radio_role_name(uint8_t role);  // "Zigbee", "Thread", "Z-Wave", "ZHA"
// The stored link of a radio nobody has used yet (radio 0's comes from the old flat settings instead).
void radio_link_default(int i, radio_link_t *out);

// What a stick probably is, from the stick list (its stock firmware) or the protocol seen on the wire.
typedef enum {
    RADIO_KIND_UNKNOWN = 0,
    RADIO_KIND_ZIGBEE,
    RADIO_KIND_THREAD,
    RADIO_KIND_ZWAVE,
} radio_kind_t;
// The role a new stick gets on radio i: Thread and Z-Wave sticks by kind; Zigbee (or unknown) is Zigbee2MQTT
// on radio 0 and ZHA on the others (radio 0 is normally Zigbee2MQTT's already).
uint8_t radio_guess_role(int i, radio_kind_t kind);
// The port radio i should use: RADIO_PORT_BASE + i unless another shown radio has it, else the lowest free one.
uint16_t radio_pick_port(const radio_link_t links[RADIO_MAX], uint32_t shown_mask, int i);
// A radio set up for a stick that just took it: role guessed, the stick's own baud (model_baud, 0 = unknown:
// 460800 for Thread RCPs, else 115200), radio 0's allowed host (the same Home Assistant, not "anyone"), a free
// port, no name.
void radio_fresh_link(int i, radio_kind_t kind, uint32_t model_baud, const radio_link_t links[RADIO_MAX],
                      uint32_t shown_mask, radio_link_t *out);

// What a radio shows as.
typedef enum {
    RS_HIDDEN = 0,  // no stick and not set up: not shown at all
    RS_NO_STICK,    // radio 0 without a stick (it is always shown)
    RS_MISSING,     // set up, but its stick is gone: its client can't reach its network
    RS_OPENING,     // stick there, not open yet
    RS_READY,       // stick open
    RS_FLASHING,    // the bridge is flashing its stick (which may be off the bus in its bootloader)
} radio_state_t;
bool radio_shown(int i, bool set_up, bool present);
radio_state_t radio_state(int i, bool set_up, bool present, bool open, bool flashing);
const char *radio_state_key(radio_state_t s);  // "hidden", "no_stick", "missing", "opening", "ready", "flashing"
// A stick left: true = free its radio (forget the stick) because nothing was ever set up on it. A stick being
// flashed leaves the bus on purpose (bootloader) and keeps its radio.
bool radio_unplug_frees(bool set_up, bool flashing);
// Once the bus has settled after boot: radios bound to a stick that isn't there and that aren't set up (no
// unplug will ever come for them). Bit i = free radio i.
uint32_t radio_stale_mask(const bool bound[RADIO_MAX], const bool set_up[RADIO_MAX], const bool present[RADIO_MAX]);

// Which fields differ between two links of one radio.
#define RL_PORT 1u
#define RL_BAUD 2u
#define RL_ALLOW 4u
#define RL_ROLE 8u
#define RL_NAME 16u
unsigned radio_link_diff(const radio_link_t *a, const radio_link_t *b);
// "6638,6639" for the shown radios (mDNS TXT). Returns false if it didn't fit.
bool radio_ports_txt(const radio_link_t links[RADIO_MAX], uint32_t shown_mask, char *buf, size_t n);
// From 1.x settings: a radio with a saved stick was in use, so it counts as set up (set_up_out). Radios that were
// on (was_on; radio 0 always) keep their ports, they had clients on them; a radio that was off but had a stick and
// now clashes moves to a free port, and gets radio 0's allowed host if it had none (it starts listening now).
// Returns the mask of radios whose link changed.
uint32_t radio_migrate_v2(radio_link_t links[RADIO_MAX], const bool bound[RADIO_MAX], const bool was_on[RADIO_MAX],
                          bool set_up_out[RADIO_MAX]);

// Full-speed-only J3 (HCFG.FSLSSupp): with it on, every hub should enumerate at full speed. A core reset after
// an unplug may clear the bit, so a hub seen at high speed means: set it again and power-cycle the port. At
// most once per RADIO_REFORCE_US, so a hub that ignores it doesn't loop the port forever.
#define RADIO_REFORCE_US (60LL * 1000 * 1000)
bool radio_hub_reforce(bool fs_only, bool is_hub, bool high_speed, int64_t now_us, int64_t *last_us);

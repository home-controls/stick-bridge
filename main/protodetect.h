#pragma once
// Which serial protocol a dongle speaks, worked out from the bytes going each way (no ESP-IDF here;
// test/host/test_usbser.py feeds it real frames). Every candidate's framing is checked in parallel,
// including its checksum, and the first one with PD_FRAMES_TO_DECIDE good frames wins. Used for dongles
// the catalogue doesn't know, and to confirm (or correct) the catalogue for the ones it does.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dongles.h"

#define PD_FRAMES_TO_DECIDE 2
#define PD_BUF 300

typedef struct {
    uint8_t buf[PD_BUF];
    uint16_t len;
    bool esc, active, bad;
} pd_frame_t;

typedef struct {
    pd_frame_t hdlc;    // 0x7E ... 0x7E, escape 0x7D (ASH and Spinel share this)
    pd_frame_t slip;    // 0xC0 ... 0xC0 (deCONZ)
    pd_frame_t zigate;  // 0x01 ... 0x03, escape 0x02
    // ZNP: 0xFE len cmd0 cmd1 data fcs (checked with a running XOR: no frame copy)
    uint16_t znp_len, znp_need;
    uint8_t znp_x;
    bool znp_in;
    uint8_t zboss_prev;
    // Z-Wave: 0x01 len type cmd data cksum (len counts itself..data; cksum = 0xFF ^ len..data)
    uint16_t zw_len, zw_need;
    uint8_t zw_x;
    bool zw_in;
} pd_dir_t;

typedef struct {
    pd_dir_t dir[2];
    uint8_t good[8];  // per proto_t
    proto_t decided;
} pd_state_t;

void pd_reset(pd_state_t *s);
// dir 0 = host -> dongle, 1 = dongle -> host. Returns the decision once made (PROTO_UNKNOWN before).
proto_t pd_feed(pd_state_t *s, int dir, const uint8_t *data, size_t len);

// The running firmware's version, read off the answers its host gets anyway (feed stick -> host bytes):
//  - OpenThread RCP: Spinel PROP_VALUE_IS(NCP_VERSION) string (OTBR asks at start-up)
//  - EmberZNet: the EZSP version response in an ASH DATA frame (Zigbee2MQTT's ember adapter and ZHA's bellows
//    both ask first thing): protocol version + stack version
//  - Z-Stack: SYS_VERSION SRSP (Zigbee2MQTT's zstack adapter and ZHA's zigpy-znp ask): product + revision
//    (Koenkk builds use the build date as the revision, e.g. 20250321)
typedef struct {
    pd_frame_t hdlc;
    uint8_t znp[64];
    uint16_t znp_len, znp_need;
    bool znp_in;
    char spinel_version[80];  // "" until seen
    uint32_t znp_revision;    // 0 until seen
    uint8_t znp_product, znp_major, znp_minor, znp_maint;
    bool znp_seen;
    bool ash_bad;  // this frame had a raw ASH substitute/cancel byte: not a good ASH frame (still fine for Spinel)
    bool ezsp_seen;
    uint8_t ezsp_version;     // EZSP protocol version (e.g. 13, 19)
    uint16_t ezsp_stack;      // EmberZNet version, one nibble per part: 0x9120 = 9.1.2.0
} pd_ver_t;
void pd_ver_reset(pd_ver_t *v);
// true when something new was learnt
bool pd_ver_feed(pd_ver_t *v, const uint8_t *data, size_t len);
// What was learnt, in the one format the dashboard shows for every radio: run = "EmberZNet 9.1.2.0 (EZSP v19)",
// "Z-Stack 3.x.0 build 20250321" or the Spinel string; ver = the part firmware builds are compared on
// ("9.1.2.0", "20250321", "2.6.1"). false (both "") when nothing is known yet.
bool pd_ver_text(const pd_ver_t *v, char *run, size_t run_n, char *ver, size_t ver_n);
// Z-Stack's product id as people know it: 0 = "1.2", 1 = "3.x.0", 2 = "3.0.x"; else major.minor.maint.
void pd_znp_name(uint8_t product, uint8_t major, uint8_t minor, uint8_t maint, char *out, size_t n);

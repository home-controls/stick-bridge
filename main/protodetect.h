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
    // ZNP: 0xFE len cmd0 cmd1 data fcs
    uint8_t znp[260];
    uint16_t znp_len, znp_need;
    bool znp_in;
    uint8_t zboss_prev;
} pd_dir_t;

typedef struct {
    pd_dir_t dir[2];
    uint8_t good[8];  // per proto_t
    proto_t decided;
} pd_state_t;

void pd_reset(pd_state_t *s);
// dir 0 = host -> dongle, 1 = dongle -> host. Returns the decision once made (PROTO_UNKNOWN before).
proto_t pd_feed(pd_state_t *s, int dir, const uint8_t *data, size_t len);

// The running firmware's version, read off the answers its host gets anyway (feed dongle -> host bytes):
//  - OpenThread RCP: Spinel PROP_VALUE_IS(NCP_VERSION) string (OTBR asks at start-up)
//  - Z-Stack: SYS_VERSION SRSP (Zigbee2MQTT's zstack adapter asks): product + revision (Koenkk builds use the
//    build date as the revision, e.g. 20250321)
typedef struct {
    pd_frame_t hdlc;
    uint8_t znp[64];
    uint16_t znp_len, znp_need;
    bool znp_in;
    char spinel_version[80];  // "" until seen
    uint32_t znp_revision;    // 0 until seen
    uint8_t znp_product, znp_major, znp_minor, znp_maint;
    bool znp_seen;
} pd_ver_t;
void pd_ver_reset(pd_ver_t *v);
// true when something new was learnt
bool pd_ver_feed(pd_ver_t *v, const uint8_t *data, size_t len);

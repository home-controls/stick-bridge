#pragma once
// Dongle firmware catalogue from GitHub releases (Nerivec/silabs-firmware-builder style), the parts with
// no ESP-IDF in them (test/host/test_fwcat.py runs them on real API responses):
//  - a streaming scanner for the releases API JSON (~220 KB per release: never held whole in RAM) that
//    reports each asset with its release's tag;
//  - the asset file-name parser: <board>_<type>_<sdk>_<version>_<baud>_<flow>.gbl
//    (bootloaders: <board>_bootloader_<sdk>_<version>.gbl);
//  - version comparison and picking the variant that suits the dongle.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char tag[48];
    bool prerelease;
    int index;  // 0 = first (newest) release in the response
} fc_release_t;

typedef struct {
    char name[112];
    char url[256];
    char sha256[65];  // from "digest": "sha256:<hex>", "" when absent
    uint32_t size;
} fc_asset_t;

typedef void (*fc_asset_cb)(void *ctx, const fc_release_t *rel, const fc_asset_t *a);

#define FC_DEPTH 12
typedef struct {
    fc_asset_cb cb;
    void *ctx;
    int depth;
    char kind[FC_DEPTH];          // '{' or '['
    char key[FC_DEPTH][24];       // current key per object depth
    int rel_depth;                // depth of a release object (1: single release, 2: array of releases)
    int assets_depth;             // depth of the "assets" array, 0 when not inside one
    fc_release_t rel;
    fc_asset_t asset;
    // token state
    enum { FC_IDLE, FC_STR, FC_STR_ESC, FC_STR_U, FC_LIT } st;
    int u_left;
    bool str_is_key;
    char sval[256];
    size_t slen;
    bool after_colon;
    char lit[24];
    size_t llen;
    bool bad;
} fc_json_t;

void fc_json_init(fc_json_t *j, fc_asset_cb cb, void *ctx);
void fc_json_feed(fc_json_t *j, const char *buf, size_t n);

typedef struct {
    char board[48];
    char type[16];   // zigbee_ncp, zigbee_router, openthread_rcp, bootloader
    char sdk[16];    // 2026.6.2
    char version[24];// 9.1.2
    uint32_t baud;   // 0 for bootloaders
    char flow[8];    // sw_flow, hw_flow, no_flow ("" for bootloaders)
} fc_name_t;

// true when the name follows the pattern (case of the board kept as is).
bool fc_parse_name(const char *name, fc_name_t *out);
// Koenkk/Z-Stack-firmware: <board>_<coordinator|router>_<YYYYMMDD>.zip -> board, type, version = the date
// (sdk, flow empty; baud 115200, Z-Stack's fixed rate).
bool fc_parse_ti_name(const char *name, fc_name_t *out);

// Compares dotted versions numerically, missing parts count as 0 ("9.1.2" == "9.1.2.0"). <0, 0, >0.
int fc_vercmp(const char *a, const char *b);

// How well a variant suits the dongle (higher is better, <0 = unusable): right board and type are a
// must; then the bridge's baud, then the flow control the board has (hw_flow only when RTS/CTS is wired).
int fc_score(const fc_name_t *n, const char *board, const char *type, uint32_t bridge_baud, bool rtscts_wired);

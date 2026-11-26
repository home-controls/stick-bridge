#pragma once
// Which Zigbee/Thread dongle is plugged in, from its USB IDs and strings (no ESP-IDF here; the table is
// checked by test/host/test_usbser.py). Many dongles share a USB-UART chip's VID:PID (10c4:ea60 is half
// of them), so the manufacturer/product strings decide, the way Home Assistant's USB matchers do.

#include <stdbool.h>
#include <stdint.h>

#include "usbser_core.h"

typedef enum {
    PROTO_UNKNOWN = 0,
    PROTO_EZSP,    // Silicon Labs EmberZNet (ASH)        -> Z2M adapter "ember"
    PROTO_ZNP,     // TI Z-Stack (MT, 0xFE frames)          -> "zstack"
    PROTO_DECONZ,  // dresden elektronik deCONZ (SLIP)      -> "deconz"
    PROTO_ZIGATE,  // ZiGate (0x01 .. 0x03)                 -> "zigate"
    PROTO_ZBOSS,   // Nordic/Espressif ZBOSS NCP            -> "zboss"
    PROTO_SPINEL,  // OpenThread RCP (not for Zigbee2MQTT)
    PROTO_ZWAVE,   // Z-Wave Serial API (SOF 01 len type cmd .. checksum) -> Z-Wave JS
} proto_t;

// How the dongle gets into its bootloader for flashing (beyond what its firmware can do itself, like
// EZSP launchStandaloneBootloader, which is always tried first on Silicon Labs dongles).
typedef enum {
    RST_NONE = 0,      // software only (or buttons)
    RST_RTS_DTR,       // universal-silabs-flasher rts_dtr: DTR0/RTS1 100 ms, DTR1/RTS0 500 ms, both 0
    RST_SLZB07_GPIO,   // CP2102N GPIO4/5: (1,1) (0,0) (1,0) (1,1), 100 ms steps
    RST_BAUD_KNOCK,    // ESP32-S3 bridge (Connect ZBT-2): 150, 300, 1200 baud then "BZ"
    RST_TI_SONOFF,     // TI ROM BSL through ZBDongle-P's gated DTR/RTS
    RST_TI_CC2538,     // TI ROM BSL, DTR = BSL pin, RTS = reset (cc2538-bsl default)
    RST_DECONZ,        // deCONZ watchdog command, re-enumerates into its bootloader
    RST_NRF_DFU,       // Nordic open DFU bootloader (button / DFU trigger)
} reset_t;

typedef enum {
    FAM_UNKNOWN = 0,
    FAM_SILABS,  // EFR32 + Gecko bootloader, .gbl (flashable here)
    FAM_TI,      // CC26xx/CC13xx + ROM BSL, .hex
    FAM_DECONZ,  // ConBee, .GCF
    FAM_NORDIC,  // nRF52840, DFU zip
    FAM_ESP,     // ESP32-C6/H2
    FAM_NXP,     // ZiGate (JN5189)
    FAM_ZWAVE,   // Z-Wave controller sticks (ZGM130S / EFR32ZG): passed through, not flashed here
} family_t;

typedef struct {
    const char *id;          // stable id, e.g. "sonoff_dongle-pmg24"
    const char *name;        // "SONOFF Dongle Plus MG24"
    uint16_t vid, pid;       // pid 0 = any
    const char *mfr;         // glob on the manufacturer string, NULL = any
    const char *product;     // glob on the product string, NULL = any
    const char *radio;       // "EFR32MG24"
    family_t family;
    proto_t proto;           // what it ships with
    uint32_t baud;           // default baud for that firmware
    bool hw_flow;            // RTS/CTS wired between the USB chip and the radio
    reset_t reset;
    const char *fw_board;    // firmware catalogue board: Nerivec model id for Silabs, Z-Stack file prefix for TI
    const char *note;        // short caveat shown on the dashboard, or NULL
} dongle_model_t;

// Entries from the downloadable list (dongles.json next to the release manifest), checked before the
// built-in table. The array must stay valid for as long as it's set (the caller never frees an old one).
void dongles_set_extra(const dongle_model_t *models, int n);
int dongles_extra_count(void);
const dongle_model_t *dongles_extra(void);
// true when m came from the downloaded list
bool dongle_is_extra(const dongle_model_t *m);
// Name lookups for the JSON list ("ezsp", "rts_dtr", "silabs", ...): -1 when unknown.
int proto_from_name(const char *s);
int reset_from_name(const char *s);
int family_from_name(const char *s);

// Best entry for this device, or a generic one for its USB chip. Never NULL.
const dongle_model_t *dongle_match(uint16_t vid, uint16_t pid, const char *mfr, const char *product, bool has_cdc);
// The whole table (for the API / dashboard list of supported dongles), terminated by id == NULL.
const dongle_model_t *dongle_models(void);

const char *proto_name(proto_t p);       // "EmberZNet (EZSP)"
const char *proto_z2m_adapter(proto_t p); // "ember", "zstack", ... or ""
const char *reset_name(reset_t r);
const char *family_name(family_t f);

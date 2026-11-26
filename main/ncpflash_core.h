#pragma once
// Flashing the Zigbee dongle (Silicon Labs EFR32 with the Gecko bootloader) from the bridge.
// Portable: no ESP-IDF here, everything goes through nf_io_t, so the host test can drive the very same
// code against a simulated dongle (test/host/test_ncpflash.py).
//
// The way in follows universal-silabs-flasher: ask the running EmberZNet firmware to start its
// bootloader (EZSP launchStandaloneBootloader over ASH); failing that, the RTS/DTR reset pattern; then
// the bootloader's menu at 115200, "1" and XMODEM-CRC, "2" to run the new firmware, and an EZSP version
// exchange to prove it answers.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NF_BL_BAUD 115200
#define NF_META_MAX 512

typedef struct {
    void *ctx;
    // Up to max bytes from the dongle, waiting at most timeout_ms for the first. 0 on timeout, <0 when
    // the dongle is gone.
    int (*read)(void *ctx, uint8_t *buf, size_t max, uint32_t timeout_ms);
    int (*write)(void *ctx, const uint8_t *buf, size_t len);  // 0 ok
    int (*set_baud)(void *ctx, uint32_t baud);                // 0 ok
    int (*set_lines)(void *ctx, bool dtr, bool rts);          // 0 ok
    void (*sleep_ms)(void *ctx, uint32_t ms);
    uint32_t (*now_ms)(void *ctx);
    int (*image_read)(void *ctx, uint32_t off, uint8_t *buf, size_t len);  // 0 ok
    void (*progress)(void *ctx, const char *step, uint32_t done, uint32_t total);  // may be NULL
    void (*log)(void *ctx, const char *msg);                                       // may be NULL
    // The board's own hardware route into the bootloader (CP2102N GPIOs, an ESP32-S3 bridge's baud knock).
    // NULL = universal-silabs-flasher's rts_dtr pattern on set_lines. Returns 0 when it did something
    // (how = what, for the result), <0 when this board has no hardware route (software only).
    int (*hw_reset)(void *ctx, const char **how);
} nf_io_t;

// GBL application types (application info tag)
#define NF_APP_ZIGBEE 0x01
#define NF_APP_THREAD 0x02
#define NF_APP_BLUETOOTH 0x08

typedef struct {
    uint32_t tag_len;  // bytes up to and including the end tag: what is sent to the dongle
    uint32_t gbl_version, gbl_type;
    bool has_app_info;
    uint32_t app_type, app_version;
    bool is_signed, encrypted, has_delta;
    uint32_t prog_bytes;  // firmware bytes in the program data tags
    uint32_t trailing;    // bytes after the end tag (ignored, not sent)
    char metadata[NF_META_MAX];  // metadata tag (JSON in the community builds), "" when none
    bool metadata_cut;
} nf_gbl_t;

typedef struct {
    bool ok;
    uint8_t ezsp_version, stack_type, reset_code;
    uint16_t stack_version;  // nibbles: major.minor.patch.special
} nf_ncp_t;

// Checks a GBL v3 file read through io->image_read: structure, every tag known, CRC-32, firmware inside,
// nothing that rewrites the bootloader or secure element, and Zigbee if it says what it is.
// NULL when it's fine, else the reason.
const char *nf_gbl_check(const nf_io_t *io, uint32_t size, nf_gbl_t *out);

// Opens an ASH session at baud and asks the firmware for its EZSP version.
bool nf_ncp_probe(const nf_io_t *io, uint32_t baud, nf_ncp_t *out);

#define NF_EXPECT_EZSP 0    // the new firmware is a Zigbee coordinator: verified with an EZSP version exchange
#define NF_EXPECT_SPINEL 1  // an OpenThread RCP: verified with a Spinel NCP_VERSION request

typedef struct {
    uint32_t app_baud;    // the baud the dongle's firmware talks at now
    uint32_t new_baud;    // the baud the new firmware should answer at
    uint32_t upload_len;  // nf_gbl_t.tag_len
    uint8_t expect;       // NF_EXPECT_*
} nf_flash_opts_t;

typedef struct {
    nf_ncp_t before, after;
    char bootloader[24];    // bootloader version, as its menu prints it
    const char *entered;    // how the bootloader was reached
    bool app_left_running;  // failed, but firmware (the old one) still answers
    bool in_bootloader;     // failed with the dongle sitting in its bootloader
    uint32_t retries;       // XMODEM blocks sent again
    char before_spinel[48]; // the old firmware's Spinel version, when it was an OpenThread RCP
    char after_spinel[48];  // the new one's (NF_EXPECT_SPINEL)
} nf_result_t;

// Asks an OpenThread RCP for its version over Spinel (HDLC-lite framing) at baud. ver gets e.g.
// "SL-OPENTHREAD/2.6.1_GitHub-...; EFR32; ...".
bool nf_spinel_probe(const nf_io_t *io, uint32_t baud, char *ver, size_t vlen);

// The whole job. True when the new firmware is on and answering; else err says what went wrong.
bool nf_flash(const nf_io_t *io, const nf_flash_opts_t *o, nf_result_t *r, char *err, size_t errlen);

uint32_t nf_crc32(uint32_t crc, const uint8_t *d, size_t n);  // zlib's; start with 0

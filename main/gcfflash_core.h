#pragma once
// Flashing dresden elektronik ConBee II / ConBee III sticks (deCONZ firmware, .GCF files) from the bridge.
// Portable: no ESP-IDF here, everything goes through gf_io_t, so the host test can drive the very same code
// against a simulated stick (test/host/test_gcfflash.py). Same shape as ncpflash_core (Silicon Labs).
//
// Everything follows dresden elektronik's own gcfflasher (github.com/dresden-elektronik/gcfflasher, gcf.c +
// protocol.c, checked at commit 25ccb7c, 2026-11-06):
//
//   ConBee II  = ATSAMR21, native USB CDC (1cf1:0030), GCF type < 30 at 0x5000, platform byte 0x07,
//                "V1" bootloader: "ID" -> banner with "Bootloader", sync 1A 1C A9 AE -> "READY", 10-byte header,
//                the stick pulls 256-byte pages with "GET" u16 ";", "#VALID CRC" when committed.
//   ConBee III = EFR32MG21 behind an FTDI (0403:6015), GCF type 60 (encrypted container, inner magic
//                0xDEC0DE03), platform byte 0x09, "V3" bootloader: SLIP frames starting 0x81
//                (ID 02/82, FW_UPDATE 03/83, the stick pulls data with 04 -> host answers 84).
//
// The way in is the same for both: the running deCONZ firmware is told (WRITE_PARAMETER, watchdog timeout
// 2 s) to stop feeding its watchdog, and the reset lands in the bootloader.
//
// USB re-enumeration (ConBee II): the SAMR21 is the USB device, so every reset (into the bootloader, and
// out of it into the new firmware) drops the CDC port and a new one appears. The core is a single blocking
// call meant to run in its own task (like ncpflash); when a read or write reports the port gone (<0) it
// calls io->reopen(timeout), which blocks until the USB host driver has seen the stick attach again and
// opened it (115200 8N1), then carries on where it was. That keeps the protocol in one straight-line
// function instead of a resumable state machine; the price is that the caller's reopen must be able to
// wait for an attach event (a FreeRTOS event group set by the USB host client). For the ConBee III the
// FTDI stays enumerated across MG21 resets, so a vanished port there is an error (reopen is still tried
// once, in case the stick was simply replugged).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GF_BAUD 115200  // app and bootloader, both sticks

typedef enum {
    GF_DEV_UNKNOWN = 0,
    GF_DEV_CONBEE2 = 2,  // 1cf1:0030
    GF_DEV_CONBEE3 = 3,  // 0403:6015 (same IDs as a ConBee I: the firmware's platform byte tells them apart)
} gf_device_t;

typedef struct {
    void *ctx;
    // Up to max bytes from the stick, waiting at most timeout_ms for the first. 0 on timeout, <0 when the
    // port is gone (USB detach).
    int (*read)(void *ctx, uint8_t *buf, size_t max, uint32_t timeout_ms);
    int (*write)(void *ctx, const uint8_t *buf, size_t len);  // 0 ok, <0 port gone
    int (*set_baud)(void *ctx, uint32_t baud);                // 0 ok; may be NULL (CDC ignores it)
    void (*sleep_ms)(void *ctx, uint32_t ms);
    uint32_t (*now_ms)(void *ctx);
    int (*image_read)(void *ctx, uint32_t off, uint8_t *buf, size_t len);  // the whole .GCF file; 0 ok
    void (*progress)(void *ctx, const char *step, uint32_t done, uint32_t total);  // may be NULL
    void (*log)(void *ctx, const char *msg);                                       // may be NULL
    // The port went away: wait up to timeout_ms for the stick to enumerate again, open it and return 0;
    // <0 when it didn't come back. NULL = this transport never re-enumerates.
    int (*reopen)(void *ctx, uint32_t timeout_ms);
} gf_io_t;

typedef struct {
    uint8_t type;           // GCF file type: 1 (ConBee II), 60 (ConBee III container), 90
    uint8_t proto;          // bootloader protocol: 1 (V1) or 3 (V3); gcfflasher: type < 30 -> V1
    uint8_t crc8;           // header checksum byte (sent to the V1 bootloader as is)
    uint8_t platform;       // 0x07 / 0x09 (from the file name, else implied by the header)
    gf_device_t device;     // which stick the file is for
    uint32_t target;        // flash target address (0x5000 ConBee II, 0x4000 ConBee III)
    uint32_t data_size;     // bytes the bootloader gets
    uint32_t data_offset;   // where they start in the file (14, or 18 for type 90)
    uint32_t crc32;         // sent with V3 FW_UPDATE (type 60: the inner image's plain CRC; type 90: header)
    uint32_t name_version;  // 0x26780700 from "deCONZ_ConBeeII_0x26780700.bin.GCF", 0 when the name has none
    uint32_t product;       // type 60 inner magic (0xDEC0DE03 = ConBee III)
    bool crc_checked;       // the file's own checksums were verified (type 60); V1's CRC-8 covers the
                            // decrypted firmware, which only the stick can check
} gf_file_t;

// Checks a .GCF file read through io->image_read (size bytes, file name for the version/platform) and
// refuses one that isn't for `want`. NULL when it's fine, else the reason. Touches nothing but image_read.
const char *gf_file_check(const gf_io_t *io, uint32_t size, const char *name, gf_device_t want, gf_file_t *out);

// Asks the running deCONZ firmware for its version (VERSION request). True and *version on an answer.
bool gf_app_version(const gf_io_t *io, uint32_t *version);

typedef struct {
    gf_device_t device;     // what is attached (from the USB IDs)
    const char *file_name;  // for the version in it; may be NULL
    uint32_t size;          // .GCF file size
    uint32_t budget_ms;     // gcfflasher's -t: keep retrying the whole sequence this long; 0 = 60 s
} gf_flash_opts_t;

typedef struct {
    uint32_t before, after;  // deCONZ firmware versions, 0 = didn't answer
    uint32_t expected;       // the file's version (from its name), 0 = unknown
    uint32_t bootloader;     // V3 bootloader version from its ID reply (0 for V1)
    uint32_t app_crc;        // V3: the app CRC the bootloader reported after the update
    bool app_crc_ok;         // ... and it matched the file's
    char banner[48];         // V1 bootloader banner line
    const char *entered;     // how the bootloader was reached
    uint32_t attempts;       // whole-sequence attempts (gcfflasher restarts from scratch on any failure)
    uint32_t reopens;        // USB re-enumerations ridden through
    uint32_t resent;         // pages / chunks the stick asked for again
    bool app_left_running;   // failed, but the (old) firmware still answers
    bool in_bootloader;      // failed with the stick sitting in its bootloader
} gf_result_t;

// The whole job: check the file, get the stick into its bootloader, upload, and see the new firmware
// answer with the file's version. True on success; else err says what went wrong.
bool gf_flash(const gf_io_t *io, const gf_flash_opts_t *o, gf_result_t *r, char *err, size_t errlen);

uint8_t gf_crc8(uint8_t crc, const uint8_t *d, size_t n);     // gcfflasher's CRC8_Dallas (poly 0x31, MSB first)
uint32_t gf_crc32(uint32_t crc, const uint8_t *d, size_t n);  // zlib's; start with 0

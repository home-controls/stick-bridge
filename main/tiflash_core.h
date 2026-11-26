#pragma once
// Flashing a TI CC26x2/CC13x2 Zigbee dongle (CC2652P, CC2652P7, CC2652R/RB, CC1352P) through its ROM
// serial bootloader (BSL), from the bridge. Portable: no ESP-IDF here, everything goes through tf_io_t,
// so the host test drives the very same code against a simulated chip (test/host/test_tiflash.py).
//
// Modelled on cc2538-bsl (JelmerT/cc2538-bsl, cc2538_bsl/cc2538_bsl.py v2.1): the same DTR/RTS entry
// patterns, the same packets, 248-byte SEND_DATA chunks with all-0xFF chunks skipped, CRC32 verify, RESET.
//
// Image: a flat binary starting at flash address 0 that covers the whole flash (it must, because the CCFG
// - the chip's boot configuration - is the last 88 bytes of flash). Build it from Koenkk's .hex with
// ihex_core, pre-filling gaps with 0xFF. Before anything is erased the image's CCFG is checked: ROM
// bootloader and its backdoor enabled, bank erase not disabled, no sector write-protected. An image that
// fails that would leave the dongle unflashable without a JTAG debugger, so it's refused.
//
// Write order is ascending, so the CCFG page goes last: if the job dies part-way, the CCFG is still blank
// (IMAGE_VALID_CONF != 0) and the ROM starts its bootloader on its own at the next power-up.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 460800: a standard rate every USB-UART bridge on these sticks (CP2102N, CH340, CH9102) makes exactly,
// ~4x faster than 115200. The ROM autobauds from the first 0x55 0x55, so any rate the bridge can make
// works; cc2538-bsl's own default is 500000. TF_FALLBACK_BAUD is tried (after a fresh entry) when the
// first sync fails.
#define TF_DEFAULT_BAUD 460800
#define TF_FALLBACK_BAUD 115200

#define TF_CHUNK 248                // SEND_DATA payload, as cc2538-bsl (writeMemory trsf_size)
#define TF_FLASH_352K 0x58000u      // CC2652P, CC2652R, CC2652RB, CC1352P: 44 x 8 KB sectors
#define TF_FLASH_704K 0xB0000u      // CC2652P7 (and the other x7 parts): 88 x 8 KB sectors
#define TF_SECTOR 8192u             // CC13x2/CC26x2 flash sector
#define TF_CCFG_LEN 88u             // CCFG = last 88 bytes of flash
#define TF_CCFG_BL_CONFIG 0x30u     // offsets inside the CCFG
#define TF_CCFG_ERASE_CONF 0x34u
#define TF_CCFG_IMAGE_VALID 0x44u
#define TF_CCFG_PROT 0x48u          // CCFG_PROT_31_0 .. CCFG_PROT_127_96, bit n = 0: sector n protected

typedef struct {
    void *ctx;
    // Up to max bytes from the dongle, waiting at most timeout_ms for the first. 0 on timeout, <0 when
    // the dongle is gone.
    int (*read)(void *ctx, uint8_t *buf, size_t max, uint32_t timeout_ms);
    int (*write)(void *ctx, const uint8_t *buf, size_t len);  // 0 ok
    int (*set_baud)(void *ctx, uint32_t baud);                // 0 ok
    // true = asserted, as pyserial's setDTR(True)/setRTS(True) (the pin is driven low on a USB-UART).
    int (*set_lines)(void *ctx, bool dtr, bool rts);  // 0 ok
    void (*sleep_ms)(void *ctx, uint32_t ms);
    uint32_t (*now_ms)(void *ctx);
    int (*image_read)(void *ctx, uint32_t off, uint8_t *buf, size_t len);          // 0 ok
    void (*progress)(void *ctx, const char *step, uint32_t done, uint32_t total);  // may be NULL
    void (*log)(void *ctx, const char *msg);                                       // may be NULL
} tf_io_t;

typedef enum {
    TF_ENTRY_NONE = 0,  // the user already put it in the bootloader (button held while plugging in)
    TF_ENTRY_SONOFF,    // cc2538-bsl --bootloader-sonoff-usb (SONOFF ZBDongle-P's gated DTR/RTS circuit)
    TF_ENTRY_DEFAULT,   // cc2538-bsl's default: DTR drives the BSL pin, RTS drives !RESET
} tf_entry_t;

// What the image's CCFG says (tf_image_check).
typedef struct {
    uint32_t flash_size;     // chip flash size this image is for (= its length)
    uint32_t bl_config;      // CCFG.BL_CONFIG word (little-endian in flash)
    uint8_t bl_pin;          // BL_PIN_NUMBER: the DIO the backdoor watches
    uint8_t bl_level;        // BL_LEVEL: 0 = backdoor when the pin is low, 1 = when high
    uint32_t erase_conf;     // CCFG.ERASE_CONF
    uint32_t image_valid;    // CCFG.IMAGE_VALID_CONF (0 = boot the image)
    uint32_t crc32;          // zlib CRC-32 of the whole image (what the chip's CRC32 must return)
    uint32_t chunks_ff;      // 248-byte chunks that are all 0xFF (skipped when writing)
} tf_image_t;

typedef struct {
    tf_entry_t entry;
    bool invert_lines;        // --bootloader-invert-lines: RTS drives the BSL pin, DTR drives reset
    bool active_high;         // --bootloader-active-high: BSL pin asserted high (DEFAULT entry only, as the script)
    uint32_t baud;            // 0 = TF_DEFAULT_BAUD
    uint32_t fallback_baud;   // 0 = TF_FALLBACK_BAUD; same as baud = no fallback
    uint32_t image_len;       // bytes in the flat image
    uint32_t chip_flash;      // the flash size the caller expects (0 = whatever the image is: 352 or 704 KB)
    bool skip_device_size;    // don't refuse when the chip's flash size can't be read (it's still refused on a mismatch)
    bool allow_bl_pin_change; // allow an image whose backdoor pin/level differs from the chip's current CCFG
    bool sector_erase;        // erase sector by sector instead of BANK_ERASE
    bool no_reset;            // leave it in the bootloader at the end
} tf_opts_t;

typedef struct {
    tf_image_t image;
    const char *entered;      // how the bootloader was reached
    uint32_t baud;            // the rate the BSL synced at
    bool chip_id_ok;
    uint32_t chip_id;         // GET_CHIP_ID's 4 bytes, big-endian (raw: the ROM doesn't document it for CC26x2)
    bool device_info_ok;      // the MEMORY_READs below worked
    uint32_t wafer_id;        // ICEPICK_DEVICE_ID.WAFER_ID: 0xBB41 = CC13x2/CC26x2, 0xBB77/0xBB7A newer parts
    uint8_t pg_rev;
    uint32_t device_flash;    // FLASH_SIZE.SECTORS x 8 KB, 0 = unknown
    uint32_t device_bl_config;// the chip's current CCFG.BL_CONFIG (before erasing)
    uint32_t device_erase_conf;
    const char *erase_how;    // "bank erase" / "sector erase"
    bool erased;              // flash was erased: on a failure after this the dongle has no firmware
                              // (its blank CCFG makes the ROM start the bootloader by itself, so try again)
    uint32_t bytes_written;   // data bytes sent in SEND_DATA (including retries' resends only once)
    uint32_t chunks_written, chunks_skipped;
    uint32_t downloads;       // DOWNLOAD commands issued
    uint32_t retries;         // commands sent again (NACK, timeout, bad answer)
    uint32_t passes;          // erase+write passes (a CRC mismatch earns one more)
    uint32_t crc_device;      // what the chip's CRC32 said last
    bool verified;            // CRC32 matched
    bool reset_sent;
} tf_result_t;

// Checks the flat image read through io->image_read: its length is a whole CC26x2 flash (352 or 704 KB,
// or exactly chip_flash when that's not 0) and its CCFG keeps the ROM bootloader reachable. Reads the
// whole image once (CRC-32, all-0xFF chunk count). NULL when fine, else the reason.
const char *tf_image_check(const tf_io_t *io, uint32_t len, uint32_t chip_flash, tf_image_t *out);

// The whole job: check the image, enter the bootloader, sync, read the chip, erase, write, CRC32-verify,
// reset. True when the new firmware is written, verified and (unless no_reset) started; else err says
// what went wrong and r says how far it got.
bool tf_flash(const tf_io_t *io, const tf_opts_t *o, tf_result_t *r, char *err, size_t errlen);

uint32_t tf_crc32(uint32_t crc, const uint8_t *d, size_t n);  // zlib's; start with 0

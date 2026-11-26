#pragma once
// Flashing a Nordic nRF52840 Dongle (PCA10059) through its preinstalled Open USB DFU bootloader (nRF5 SDK
// secure-DFU protocol, unsigned) from the bridge, which is the USB host. The dongle's bootloader shows up
// as USB CDC-ACM 1915:521F; this core speaks the serial DFU protocol (SLIP framed) over that CDC link.
// Portable: no ESP-IDF here, everything goes through nd_io_t, so the host test can drive the very same
// code against a simulated bootloader (test/host/test_nrfdfu.py).
//
// The protocol follows pc-nrfutil's dfu_transport_serial.py and the nRF5 SDK bootloader
// (nrf_dfu_req_handler.c, nrf_dfu_validation.c, nrf_dfu_ver_validation.c, nrf_dfu_serial.c):
//   ping, PRN = 0, get MTU, then the init packet as a command object (create, write, CRC, execute) and
//   the firmware as 4 KB data objects (create, write, CRC, execute). The last execute makes the
//   bootloader check the SHA-256 from the init packet, activate the image and reset into it.
// Only application updates are sent: the MBR, the SoftDevice area rules and the bootloader itself
// are never targeted, so a failed update leaves the dongle in its bootloader, not bricked.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ND_VID 0x1915
#define ND_PID_BOOTLOADER 0x521F  // the Open USB bootloader's CDC-ACM
#define ND_HW_VERSION 52          // NRF_DFU_HW_VERSION of the PCA10059 open bootloader (nrfutil --hw-version 52)
#define ND_APP_START 0x1000       // after the MBR (no SoftDevice): where a dongle app is linked
#define ND_APP_LIMIT 0xE0000      // the bootloader starts here on the PCA10059
#define ND_INIT_MAX 512           // INIT_COMMAND_MAX_SIZE (256 on old settings-v1 bootloaders)
#define ND_CHUNK_MAX 1024         // write payload: (MTU 2051 - 1) / 2 - 1, as pc-nrfutil sends over USB

typedef struct {
    void *ctx;
    // Up to max bytes from the dongle, waiting at most timeout_ms for the first. 0 on timeout, <0 when
    // the dongle is gone.
    int (*read)(void *ctx, uint8_t *buf, size_t max, uint32_t timeout_ms);
    int (*write)(void *ctx, const uint8_t *buf, size_t len);  // 0 ok
    // CDC SET_CONTROL_LINE_STATE. The SDK's CDC class drops every reply until DTR is set.
    int (*set_lines)(void *ctx, bool dtr, bool rts);  // 0 ok
    void (*sleep_ms)(void *ctx, uint32_t ms);
    uint32_t (*now_ms)(void *ctx);
    int (*image_read)(void *ctx, uint32_t off, uint8_t *buf, size_t len);  // 0 ok: the .bin (or the .hex for nd_hex_to_bin)
    void (*progress)(void *ctx, const char *step, uint32_t done, uint32_t total);  // may be NULL
    void (*log)(void *ctx, const char *msg);                                       // may be NULL
    // The board's way into the bootloader when the firmware runs: the nRF5 SDK "DFU trigger" USB
    // interface (class FF/01/01): control OUT bmRequestType 0x21, bRequest 0 (DETACH), wValue 0,
    // wIndex = that interface, 1 data byte; then wait for 1915:521F and reopen the port.
    // NULL or <0 = not possible (NCS/Zephyr builds have no trigger: the RESET button is the way in).
    int (*enter_dfu)(void *ctx);
} nd_io_t;

// The init packet (.dat), decoded. Field numbers from dfu-cc.proto.
typedef struct {
    bool is_signed;  // a SignedCommand (accepted unchecked by the open bootloader for applications)
    bool has_type, has_fw_version, has_hw_version, has_app_size, has_hash, has_is_debug;
    uint32_t type;  // 0 application, 1 SoftDevice, 2 bootloader, 3 SD+BL, 4 external app
    uint32_t fw_version, hw_version, app_size, sd_size, bl_size;
    uint32_t sd_req[16];
    uint8_t sd_req_count;
    uint32_t hash_type;  // 3 = SHA-256 (the only one the bootloader takes)
    uint8_t hash[32];    // as stored: SHA-256 digest in reversed (little-endian) byte order
    bool is_debug;
} nd_init_t;

// Decodes a .dat (Packet{command|signed_command}). NULL when it's well formed, else the reason.
const char *nd_init_parse(const uint8_t *dat, size_t len, nd_init_t *out);
// What the dongle's open bootloader will take and what's safe to send: an application, SHA-256,
// hw_version 52, an app_size. NULL when fine, else the reason.
const char *nd_init_policy(const nd_init_t *in);

// Builds the init packet nrfutil writes for `pkg generate --hw-version <hw> --sd-req 0x00
// --application <x> --application-version <fw>`: unsigned, APPLICATION, SHA-256, boot validation
// CRC. Byte-identical to pc-nrfutil 6.x. sha256 is the plain (big-endian) digest of the .bin.
// Returns the length, 0 when max is too small.
size_t nd_init_build(uint32_t app_size, const uint8_t sha256[32], uint32_t fw_version, uint32_t hw_version,
                     uint8_t *out, size_t max);

// SHA-256 of len bytes read through io->image_read. NULL on success.
const char *nd_image_sha256(const nd_io_t *io, uint32_t len, uint8_t out[32]);
// The .bin against its init packet: size, hash, and a Cortex-M vector table that boots at
// ND_APP_START (initial SP in RAM, reset handler a Thumb address inside the image). NULL when fine.
const char *nd_image_check(const nd_io_t *io, uint32_t bin_len, const nd_init_t *in);

// Intel HEX (read through io->image_read, hex_size bytes) to the .bin nrfutil would make: UICR records
// (>= 0x10000000) dropped like nrfutil does, gaps 0xFF, length rounded up to a word. Refuses the MBR
// area (a full-chip/bare build) and anything reaching the bootloader. out_max bytes at out.
// NULL when fine; *bin_len, *start (the lowest address) and *dropped (UICR bytes left out) set.
const char *nd_hex_to_bin(const nd_io_t *io, uint32_t hex_size, uint8_t *out, uint32_t out_max, uint32_t *bin_len,
                          uint32_t *start, uint32_t *dropped);

typedef struct {
    const uint8_t *init;  // the .dat (from the nrfutil zip, or nd_init_build)
    size_t init_len;
    uint32_t bin_len;  // the .bin, read through io->image_read
} nd_flash_opts_t;

typedef struct {
    const char *entered;  // "already in the bootloader" / "DFU trigger"
    uint16_t mtu;
    uint32_t chunk, obj_max;
    uint32_t retries;    // objects sent again after a CRC mismatch
    bool app_erased;     // failed after the bootloader accepted the init packet: the old firmware is gone
                         // and the dongle sits in DFU mode (red LED fading) until it is flashed again
    uint8_t result;      // the bootloader's last result code (1 = success)
    uint8_t ext_error;   // its extended error when result was 0x0B, else 0
} nd_result_t;

// The whole job: checks the image against the init packet, finds the bootloader (or asks enter_dfu),
// sends both objects. True when the bootloader accepted the image and is starting it (the dongle then
// re-enumerates as the new firmware: the caller reconnects and probes); else err says what went wrong.
bool nd_flash(const nd_io_t *io, const nd_flash_opts_t *o, nd_result_t *r, char *err, size_t errlen);

// Text for a bootloader extended error code (nrf_dfu_ext_error_code_t).
const char *nd_ext_error_str(uint8_t code);

uint32_t nd_crc32(uint32_t crc, const uint8_t *d, size_t n);  // zlib's; start with 0

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    uint32_t used;
} nd_sha256_t;
void nd_sha256_init(nd_sha256_t *s);
void nd_sha256_update(nd_sha256_t *s, const uint8_t *d, size_t n);
void nd_sha256_final(nd_sha256_t *s, uint8_t out[32]);

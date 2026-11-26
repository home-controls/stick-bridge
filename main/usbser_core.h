#pragma once
// USB-to-serial chips the dongles use, the parts that are plain arithmetic (no ESP-IDF here, so
// test/host/test_usbser.py checks them): which chip a VID:PID is, CH34x and FTDI baud divisors, and
// FTDI's per-packet status bytes. The control requests themselves are in usbser.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    US_NONE = 0,
    US_CP210X,  // Silicon Labs CP210x (vendor requests, AN571)
    US_CH34X,   // WCH CH340/CH341 (vendor requests)
    US_FTDI,    // FTDI FT232R / FT-X (vendor requests, 2 status bytes in every IN packet)
    US_CDC,     // standard CDC-ACM: CH9102F/CH343, ESP32-S3 bridges, ConBee II, nRF52840, ...
} us_chip_t;

const char *us_chip_name(us_chip_t c);

// The driver for a device. has_cdc: it has a CDC communications interface (class 0x02) — used for
// devices whose VID:PID isn't one of the vendor chips.
us_chip_t us_chip_for(uint16_t vid, uint16_t pid, bool has_cdc);

// CH340/CH341 divisor/prescaler word for register pair 0x1312 (Linux ch341_get_divisor), or -1 when
// the rate can't be made. chip_version > 0x27 sets bit 7 (don't hold RX until 32 bytes).
int us_ch34x_divisor(uint32_t baud, uint8_t chip_version);
// The rate that word really gives (for showing the error).
uint32_t us_ch34x_actual(int divisor_word);

// FTDI FT232R/FT-X ("BM" style) divisor: low 16 bits go in wValue, bits 16+ in wIndex (Linux
// ftdi_232bm_baud_base_to_divisor with a 48 MHz base).
uint32_t us_ftdi_divisor(uint32_t baud);
uint32_t us_ftdi_actual(uint32_t divisor);

// FTDI IN data: every max-packet chunk starts with 2 status bytes (modem, line). Strips them in place
// and returns the payload length; the latest status bytes go to *modem / *line (OR'd line errors).
size_t us_ftdi_strip(uint8_t *buf, size_t len, size_t mps, uint8_t *modem, uint8_t *line_errs);
#define US_FTDI_CTS 0x10
#define US_FTDI_LINE_OVERRUN 0x02
#define US_FTDI_LINE_PARITY 0x04
#define US_FTDI_LINE_FRAMING 0x08
#define US_FTDI_LINE_BREAK 0x10

// Case-insensitive glob ('*' and '?') for USB strings, like Home Assistant's matchers.
bool us_glob(const char *pattern, const char *s);

#include "usbser_core.h"

#include <ctype.h>

const char *us_chip_name(us_chip_t c) {
    switch (c) {
        case US_CP210X: return "CP210x";
        case US_CH34X: return "CH340/CH341";
        case US_FTDI: return "FTDI";
        case US_CDC: return "CDC-ACM";
        default: return "unknown";
    }
}

us_chip_t us_chip_for(uint16_t vid, uint16_t pid, bool has_cdc) {
    if (vid == 0x10C4 && (pid == 0xEA60 || pid == 0xEA70 || pid == 0xEA71 || pid == 0x8A2A)) return US_CP210X;
    if (vid == 0x1A86 && (pid == 0x7523 || pid == 0x7522 || pid == 0x5523)) return US_CH34X;
    // CH9102F / CH343 are CDC class chips (ttyACM on Linux)
    if (vid == 0x1A86 && (pid == 0x55D4 || pid == 0x55D3 || pid == 0x55D2)) return US_CDC;
    if (vid == 0x0403 && (pid == 0x6001 || pid == 0x6015)) return US_FTDI;
    return has_cdc ? US_CDC : US_NONE;
}

// Baud rates for CH340/CH341 and FTDI: serial_baud.c (written from docs/usb-serial-baud.md).

// ---------------------------------------------------------------- FTDI status bytes

size_t us_ftdi_strip(uint8_t *buf, size_t len, size_t mps, uint8_t *modem, uint8_t *line_errs) {
    if (mps < 3) mps = 64;
    size_t in = 0, out = 0;
    while (in < len) {
        size_t chunk = len - in < mps ? len - in : mps;
        if (chunk >= 2) {
            if (modem) *modem = buf[in];
            if (line_errs) *line_errs |= buf[in + 1] & 0x1E;
            for (size_t i = 2; i < chunk; i++) buf[out++] = buf[in + i];
        }
        in += chunk;
    }
    return out;
}

// ---------------------------------------------------------------- glob

bool us_glob(const char *p, const char *s) {
    if (p == NULL) return true;
    if (s == NULL) s = "";
    const char *star = NULL, *ss = NULL;
    while (*s) {
        if (*p == '*') {
            star = p++;
            ss = s;
        } else if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) {
            p++;
            s++;
        } else if (star) {
            p = star + 1;
            s = ++ss;
        } else {
            return false;
        }
    }
    while (*p == '*') p++;
    return *p == 0;
}

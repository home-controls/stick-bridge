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

// ---------------------------------------------------------------- CH340/CH341 (Linux ch341.c)

#define CH341_CLKRATE 48000000u
#define CH341_CLK_DIV(ps, fact) (1u << (12 - 3 * (ps) - (fact)))
#define CH341_MIN_RATE(ps) (CH341_CLKRATE / (CH341_CLK_DIV((ps), 1) * 512))
#define CH341_MIN_BPS CH341_MIN_RATE(0)
#define CH341_MAX_BPS (CH341_CLKRATE / (CH341_CLK_DIV(3, 0) * 2))

int us_ch34x_divisor(uint32_t speed, uint8_t chip_version) {
    static const uint32_t min_rates[] = {CH341_MIN_RATE(0), CH341_MIN_RATE(1), CH341_MIN_RATE(2), CH341_MIN_RATE(3)};
    if (speed < CH341_MIN_BPS) speed = CH341_MIN_BPS;
    if (speed > CH341_MAX_BPS) speed = CH341_MAX_BPS;
    unsigned fact = 1;
    int ps;
    for (ps = 3; ps >= 0; ps--) {
        if (speed > min_rates[ps]) break;
    }
    if (ps < 0) return -1;
    unsigned clk_div = CH341_CLK_DIV(ps, fact);
    unsigned div = CH341_CLKRATE / (clk_div * speed);
    if (div < 9 || div > 255) {
        div /= 2;
        clk_div *= 2;
        fact = 0;
    }
    if (div < 2) return -1;
    // Next divisor if it lands closer (scaled to avoid rounding at low rates).
    if (16 * CH341_CLKRATE / (clk_div * div) - 16 * speed >= 16 * speed - 16 * CH341_CLKRATE / (clk_div * (div + 1))) div++;
    // Lower base clock when the divisor is even: more tolerant receiver.
    if (fact == 1 && div % 2 == 0) {
        div /= 2;
        fact = 0;
    }
    int w = (int)((0x100 - div) << 8 | fact << 2 | (unsigned)ps);
    if (chip_version > 0x27) w |= 0x80;
    return w;
}

uint32_t us_ch34x_actual(int w) {
    if (w < 0) return 0;
    unsigned ps = (unsigned)w & 3, fact = ((unsigned)w >> 2) & 1, div = 0x100 - (((unsigned)w >> 8) & 0xFF);
    if (div == 0) return 0;
    return CH341_CLKRATE / (CH341_CLK_DIV(ps, fact) * div);
}

// ---------------------------------------------------------------- FTDI (Linux ftdi_sio.c)

uint32_t us_ftdi_divisor(uint32_t baud) {
    static const uint8_t divfrac[8] = {0, 3, 2, 4, 1, 5, 6, 7};
    if (baud == 0) baud = 1;
    const uint32_t base = 48000000u;
    uint32_t divisor3 = (base + baud) / (2 * baud);  // DIV_ROUND_CLOSEST(base, 2 * baud): divisor * 8
    uint32_t d = divisor3 >> 3;
    d |= (uint32_t)divfrac[divisor3 & 7] << 14;
    if (d == 1) d = 0;            // 3 Mbaud
    else if (d == 0x4001) d = 1;  // 2 Mbaud
    return d;
}

uint32_t us_ftdi_actual(uint32_t d) {
    // inverse: fraction code -> eighths
    static const uint8_t eighths[8] = {0, 4, 2, 1, 3, 5, 6, 7};
    if (d == 0) return 3000000;
    if (d == 1) return 2000000;
    uint32_t whole = d & 0x3FFF, frac = eighths[(d >> 14) & 7];
    uint32_t div8 = whole * 8 + frac;
    return div8 ? (uint32_t)((24000000ull + div8 / 2) / div8) : 0;
}

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

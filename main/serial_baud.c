// Baud rate to divisor values for CH340/CH341 and FTDI USB serial chips, written from docs/usb-serial-baud.md.
// Integer arithmetic only: rates are compared as exact fractions.

#include "serial_baud.h"

#define SB_CLOCK 48000000u
#define SB_CH34X_MIN 46u
#define SB_CH34X_MAX 3000000u
#define SB_FTDI_EIGHTHS_CLOCK 24000000u  // 3 MHz in eighths of a divisor
#define SB_FTDI_WHOLE_MAX 16383u

// ---- CH340 / CH341 ----

// P = 2^(12 - 3*PS - FACT)
static uint32_t ch34x_prescaler(unsigned ps, unsigned fact) { return 1u << (12u - 3u * ps - fact); }

typedef struct {
    uint64_t dist;  // |CLOCK - baud * m|: the distance to baud is dist / m
    uint64_t m;     // P * N; the rate is CLOCK / m
    unsigned ps, fact, n;
} ch34x_pick_t;

// Is a a better choice than b under the spec's rules (closest; same rate: FACT 0, then larger N;
// equally close but different rates: the higher rate)?
static int ch34x_better(const ch34x_pick_t *a, const ch34x_pick_t *b) {
    // a.dist / a.m vs b.dist / b.m. dist < 3e12 and m < 1.1e6, so the products stay under 2^63.
    uint64_t lhs = a->dist * b->m, rhs = b->dist * a->m;
    if (lhs != rhs) return lhs < rhs;
    if (a->m != b->m) return a->m < b->m;          // equally close, different rates: higher rate
    if (a->fact != b->fact) return a->fact == 0;   // same rate: the slower clock
    return a->n > b->n;                            // then the larger divisor
}

int sb_ch34x_word(uint32_t baud, uint8_t chip_version) {
    if (baud == 0) return -1;
    if (baud < SB_CH34X_MIN) baud = SB_CH34X_MIN;
    if (baud > SB_CH34X_MAX) baud = SB_CH34X_MAX;

    ch34x_pick_t best = {0};
    int have = 0;
    for (unsigned ps = 0; ps <= 3; ps++) {
        for (unsigned fact = 0; fact <= 1; fact++) {
            uint32_t p = ch34x_prescaler(ps, fact);
            for (unsigned n = (fact ? 9u : 2u); n <= 255; n++) {
                uint64_t m = (uint64_t)p * n;
                uint64_t want = (uint64_t)baud * m;
                ch34x_pick_t c = {
                    .dist = want > SB_CLOCK ? want - SB_CLOCK : SB_CLOCK - want, .m = m, .ps = ps, .fact = fact, .n = n};
                if (!have || ch34x_better(&c, &best)) {
                    best = c;
                    have = 1;
                }
            }
        }
    }
    int low = (int)(best.ps | (best.fact << 2));
    if (chip_version > 0x27) low |= 0x80;
    return ((int)(256u - best.n) << 8) | low;
}

uint32_t sb_ch34x_rate(int word) {
    if (word < 0) return 0;
    // Only the low 16 bits count; a high byte of 0 (N = 256, never produced) gives 0.
    unsigned ps = (unsigned)word & 3u;
    unsigned fact = ((unsigned)word >> 2) & 1u;
    unsigned hi = ((unsigned)word >> 8) & 0xFFu;
    if (hi == 0) return 0;
    unsigned n = 256u - hi;
    return (uint32_t)(SB_CLOCK / ((uint64_t)ch34x_prescaler(ps, fact) * n));
}

// ---- FTDI FT232R / FT-X ----

// Fraction in eighths -> 3-bit code, and back (AN232B-05's table).
static const uint8_t ftdi_code_of_eighth[8] = {0, 3, 2, 4, 1, 5, 6, 7};
static const uint8_t ftdi_eighth_of_code[8] = {0, 4, 2, 1, 3, 5, 6, 7};

uint32_t sb_ftdi_value(uint32_t baud) {
    if (baud == 0) baud = 1;
    // 24,000,000 / baud to the nearest integer, halves up.
    uint64_t e = (2ull * SB_FTDI_EIGHTHS_CLOCK + baud) / (2ull * baud);
    if (e <= 12) {
        // D = 1 (8 eighths, value 0) or D = 1.5 (12 eighths, value 1), whichever is closer to the rounded
        // eighths count above (the spec's "computed number"); a tie goes to the higher rate, D = 1.
        uint64_t d8 = e > 8 ? e - 8 : 8 - e;
        uint64_t d12 = 12 - e;
        return d12 < d8 ? 1u : 0u;
    }
    if (e <= 15) e = 16;  // no divisors between 1.5 and 2
    // Above 16383 7/8 (the most 14 bits + a fraction hold): exactly 16383 7/8, the slowest rate (~183 baud).
    if (e > SB_FTDI_WHOLE_MAX * 8u + 7u) e = SB_FTDI_WHOLE_MAX * 8u + 7u;
    return (uint32_t)(e / 8) | ((uint32_t)ftdi_code_of_eighth[e % 8] << 14);
}

uint32_t sb_ftdi_rate(uint32_t value) {
    value &= 0x1FFFFu;  // bits above 16 are ignored
    if (value == 0) return 3000000u;
    if (value == 1) return 2000000u;
    // Not zero: value != 0 with whole 0 and code 0 can't happen after the mask.
    uint64_t eighths = (uint64_t)(value & 0x3FFFu) * 8u + ftdi_eighth_of_code[(value >> 14) & 7u];
    return (uint32_t)((2ull * SB_FTDI_EIGHTHS_CLOCK + eighths) / (2ull * eighths));
}

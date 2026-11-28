// Checks the firmware's own serial_baud.c against docs/usb-serial-baud.md (test_serial_baud.py builds it
// with ASan/UBSan): the spec's expected values, the CH34x tie and preference rules against an independent
// brute force, clamping, the version bit, and the FTDI rounding and special values.
//
//   serial_baud_harness selftest

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "serial_baud.h"

static int g_fail;
static long g_checks;
#define CHECK(c, ...)                                         \
    do {                                                      \
        g_checks++;                                           \
        if (!(c)) {                                           \
            printf("FAIL %s:%d: ", __func__, __LINE__);       \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
            if (++g_fail > 50) exit(1);                       \
        }                                                     \
    } while (0)

static const uint32_t STANDARD[] = {300,   600,   1200,   1800,   2400,   4800,   9600,   14400,
                                    19200, 28800, 38400,  57600,  76800,  115200, 230400, 460800,
                                    500000, 576000, 921600};

// ---- CH34x helpers (decode a word independently of the code under test) ----

typedef struct {
    int ps, fact, n;
} ch_t;

static ch_t ch_decode(int w) {
    ch_t c = {w & 3, (w >> 2) & 1, 256 - ((w >> 8) & 0xFF)};
    return c;
}

static long long ch_p(int ps, int fact) {
    static const long long P[4][2] = {{4096, 2048}, {512, 256}, {64, 32}, {8, 4}};  // the spec's table
    return P[ps][fact];
}

// Independent brute force: N in the outer loop, distances compared as signed __int128 cross products,
// then the tie rules applied as separate filtering passes.
static int ch_brute(uint32_t baud) {
    if (baud < 46) baud = 46;
    if (baud > 3000000) baud = 3000000;
    typedef struct {
        int ps, fact, n;
        __int128 num, den;  // |rate - baud| = num / den
    } cand_t;
    static cand_t all[4 * 2 * 254];
    int k = 0;
    for (int n = 2; n <= 255; n++)
        for (int ps = 3; ps >= 0; ps--)
            for (int fact = 1; fact >= 0; fact--) {
                if (fact == 1 && n < 9) continue;
                __int128 den = (__int128)ch_p(ps, fact) * n;
                __int128 num = (__int128)48000000 - (__int128)baud * den;
                if (num < 0) num = -num;
                all[k++] = (cand_t){ps, fact, n, num, den};
            }
    // 1: the smallest distance
    int best = 0;
    for (int i = 1; i < k; i++)
        if (all[i].num * all[best].den < all[best].num * all[i].den) best = i;
    // 2: among those equally close, the highest rate (smallest P*N)
    __int128 mden = -1;
    for (int i = 0; i < k; i++)
        if (all[i].num * all[best].den == all[best].num * all[i].den && (mden < 0 || all[i].den < mden))
            mden = all[i].den;
    // 3: of that rate, FACT 0 if there is one; 4: then the largest N
    int pick = -1;
    for (int pass_fact = 0; pass_fact <= 1 && pick < 0; pass_fact++)
        for (int i = 0; i < k; i++)
            if (all[i].den == mden && all[i].fact == pass_fact && (pick < 0 || all[i].n > all[pick].n)) pick = i;
    return ((256 - all[pick].n) << 8) | all[pick].ps | (all[pick].fact << 2);
}

static void t_ch34x_expected(void) {
    struct {
        uint32_t baud;
        int word;
        uint32_t rate;
    } ex[] = {
        {115200, 0xCC03, 115384}, {9600, 0xB202, 9615},     {921600, 0xF307, 923076},
        {300, 0xD900, 300},       {3000000, 0xFE03, 3000000}, {1000000, 0xFA03, 1000000},
    };
    for (size_t i = 0; i < sizeof(ex) / sizeof(ex[0]); i++) {
        int w = sb_ch34x_word(ex[i].baud, 0x27);
        CHECK(w == ex[i].word, "%u -> 0x%04X, want 0x%04X", ex[i].baud, w, ex[i].word);
        CHECK(sb_ch34x_rate(w) == ex[i].rate, "%u rate %u, want %u", ex[i].baud, sb_ch34x_rate(w), ex[i].rate);
    }
    CHECK(sb_ch34x_word(115200, 0x30) == 0xCC83, "115200 with bit 7: 0x%04X", sb_ch34x_word(115200, 0x30));
    ch_t c = ch_decode(sb_ch34x_word(115200, 0));
    CHECK(c.ps == 3 && c.fact == 0 && c.n == 52, "115200 = PS3 FACT0 N52");
    c = ch_decode(sb_ch34x_word(9600, 0));
    CHECK(c.ps == 2 && c.fact == 0 && c.n == 78, "9600 = PS2 FACT0 N78");
    c = ch_decode(sb_ch34x_word(921600, 0));
    CHECK(c.ps == 3 && c.fact == 1 && c.n == 13, "921600 = PS3 FACT1 N13");
    c = ch_decode(sb_ch34x_word(300, 0));
    CHECK(c.ps == 0 && c.fact == 0 && c.n == 39, "300 = PS0 FACT0 N39");
    // 300 is not exact: 48e6 / (4096 * 39) = 300.48 (+0.16 %), shown as 300 only because the rate rounds down.
    CHECK(48000000LL % (4096LL * 39) != 0 && 48000000LL * 1000 / (4096LL * 39) == 300480, "300 -> 300.48");
}

static void t_ch34x_standard(void) {
    for (size_t i = 0; i < sizeof(STANDARD) / sizeof(STANDARD[0]); i++) {
        uint32_t b = STANDARD[i];
        uint32_t r = sb_ch34x_rate(sb_ch34x_word(b, 0));
        long long err = (long long)r - b;
        if (err < 0) err = -err;
        CHECK(err * 50 <= b, "%u -> %u, more than 2 %% off", b, r);
    }
    const uint32_t exact[] = {1000000, 2000000, 3000000};
    for (size_t i = 0; i < 3; i++) {
        int w = sb_ch34x_word(exact[i], 0);
        ch_t c = ch_decode(w);
        CHECK(48000000LL % (ch_p(c.ps, c.fact) * c.n) == 0 && sb_ch34x_rate(w) == exact[i], "%u not exact (%u)",
              exact[i], sb_ch34x_rate(w));
    }
}

static void t_ch34x_ties(void) {
    // Equal rates: FACT 0 over FACT 1 (115200: N52 at P8 = N104 at P4; 1,000,000: N6 at P8 = N12 at P4).
    CHECK(ch_decode(sb_ch34x_word(115200, 0)).fact == 0, "115200 prefers FACT 0");
    CHECK(sb_ch34x_word(1000000, 0) == 0xFA03, "1,000,000 prefers FACT 0 N6");
    // Equal rates, FACT 0 in several PS: P*N = 8192 is PS0 N2, PS1 N16, PS2 N128 (and PS1 FACT1 N32):
    // the largest N, PS2 N128 = 0x8002.
    CHECK(sb_ch34x_word(5859, 0) == 0x8002, "5859 -> 0x%04X, want PS2 N128 0x8002", sb_ch34x_word(5859, 0));
    // FACT 1 never with N < 9: 1,714,286 would be exactly P4 N7; must take 1.5M (P8 N4) instead.
    CHECK(sb_ch34x_word(1714286, 0) == 0xFC03, "1,714,286 -> 0x%04X, want 0xFC03", sb_ch34x_word(1714286, 0));
    // 2,400,000 would be P4 N5: 2M (P8 N3) is closer than 3M.
    CHECK(sb_ch34x_word(2400000, 0) == 0xFD03, "2,400,000 -> 0x%04X, want 0xFD03", sb_ch34x_word(2400000, 0));
    // Two different rates equally close: the higher. 2.5M is halfway between 2M and 3M; 1.75M between 1.5M and 2M.
    CHECK(sb_ch34x_word(2500000, 0) == 0xFE03, "2,500,000 -> 0x%04X, want 3M 0xFE03", sb_ch34x_word(2500000, 0));
    CHECK(sb_ch34x_word(1750000, 0) == 0xFD03, "1,750,000 -> 0x%04X, want 2M 0xFD03", sb_ch34x_word(1750000, 0));
    // N never < 2: 3M is P8 N2, never P8 N1 / P4 N4.
    ch_t c = ch_decode(sb_ch34x_word(3000000, 0));
    CHECK(c.n == 2 && c.fact == 0, "3M = N2 FACT0");
}

static void t_ch34x_clamp(void) {
    CHECK(sb_ch34x_word(0, 0) == -1, "0 -> -1");
    CHECK(sb_ch34x_word(0, 0x30) == -1, "0 -> -1 with a new chip too");
    int w46 = sb_ch34x_word(46, 0);
    CHECK(w46 == 0x0100, "46 -> 0x%04X, want PS0 FACT0 N255 0x0100", w46);
    CHECK(sb_ch34x_word(1, 0) == w46 && sb_ch34x_word(45, 0) == w46, "1 and 45 clamp to 46");
    CHECK(sb_ch34x_rate(w46) == 45, "slowest rate 45 (%u)", sb_ch34x_rate(w46));
    CHECK(sb_ch34x_word(3000000, 0) == 0xFE03 && sb_ch34x_word(10000000, 0) == 0xFE03 &&
              sb_ch34x_word(3000001, 0) == 0xFE03 && sb_ch34x_word(0xFFFFFFFFu, 0) == 0xFE03,
          "above 3M clamps to 3M");
}

static void t_ch34x_version_bit(void) {
    const uint32_t bauds[] = {300, 9600, 115200, 921600, 3000000};
    for (size_t i = 0; i < 5; i++) {
        for (int v = 0; v <= 0xFF; v++) {
            int w = sb_ch34x_word(bauds[i], (uint8_t)v);
            int plain = sb_ch34x_word(bauds[i], 0);
            CHECK(((w & 0x80) != 0) == (v > 0x27), "version 0x%02X bit 7 %d", v, (w >> 7) & 1);
            CHECK((w & ~0x80) == plain, "version only changes bit 7");
            CHECK(sb_ch34x_rate(w) == sb_ch34x_rate(plain), "bit 7 doesn't change the rate");
        }
    }
}

static void t_ch34x_rate(void) {
    CHECK(sb_ch34x_rate(-1) == 0, "negative word -> 0");
    CHECK(sb_ch34x_rate(-12345) == 0, "negative word -> 0");
    CHECK(sb_ch34x_rate(0x0003) == 0, "high byte 0 (N = 0) -> 0");
    CHECK(sb_ch34x_rate(0xFE03) == 3000000, "0xFE03 = 3M");
    CHECK(sb_ch34x_rate(0xFE83) == 3000000, "bit 7 ignored");
    CHECK(sb_ch34x_rate(0xF307) == 923076, "0xF307 = 923076 (rounded down)");
    CHECK(sb_ch34x_rate(0x0100) == 45, "N 255 PS0 = 45");
    CHECK(sb_ch34x_rate(0x1FE03) == 3000000 && sb_ch34x_rate(0x7FFFFE03) == 3000000, "only the low 16 bits count");
    CHECK(sb_ch34x_rate(0x10003) == 0, "high byte 0 with bits above 15 set -> 0");
}

static void t_ch34x_sweep(void) {
    long n = 0;
    for (uint32_t b = 46; b <= 3000000; b += 7, n++) {
        int w = sb_ch34x_word(b, 0);
        ch_t c = ch_decode(w);
        CHECK(w >= 0 && w <= 0xFFFF && (w & 0x78) == 0 && (w & 0x80) == 0, "%u: bad word 0x%04X", b, w);
        CHECK(c.n >= 2 && c.n <= 255 && !(c.fact && c.n < 9), "%u: N %d FACT %d", b, c.n, c.fact);
        int ref = ch_brute(b);
        CHECK(w == ref, "%u: 0x%04X, brute force says 0x%04X", b, w, ref);
    }
    // and the ends
    CHECK(sb_ch34x_word(3000000, 0) == ch_brute(3000000), "3M");
    CHECK(sb_ch34x_word(2999999, 0) == ch_brute(2999999), "2999999");
    printf("ch34x sweep: %ld rates cross-checked\n", n);
}

// ---- FTDI ----

static const int CODE_TO_EIGHTHS[8] = {0, 4, 2, 1, 3, 5, 6, 7};  // the app note's table, read backwards

static uint64_t ftdi_eighths(uint32_t v) { return (uint64_t)(v & 0x3FFF) * 8 + CODE_TO_EIGHTHS[(v >> 14) & 7]; }

// 24e6 / baud to the nearest integer, halves up, computed from quotient + remainder.
static uint64_t nearest_eighths(uint32_t baud) {
    uint64_t q = 24000000ull / baud, r = 24000000ull % baud;
    return q + (2 * r >= baud ? 1 : 0);
}

static void t_ftdi_expected(void) {
    struct {
        uint32_t baud, value;
    } ex[] = {{115200, 0x001A}, {460800, 0x4006}, {921600, 0x8003}, {9600, 0x4138}, {3000000, 0}, {2000000, 1}};
    for (size_t i = 0; i < sizeof(ex) / sizeof(ex[0]); i++)
        CHECK(sb_ftdi_value(ex[i].baud) == ex[i].value, "%u -> 0x%05X, want 0x%05X", ex[i].baud,
              sb_ftdi_value(ex[i].baud), ex[i].value);
    CHECK(sb_ftdi_rate(0) == 3000000 && sb_ftdi_rate(1) == 2000000, "special values");
    CHECK(sb_ftdi_rate(0x001A) == 115385, "0x001A = 24e6/208 = 115384.6 -> %u", sb_ftdi_rate(0x001A));
    CHECK(sb_ftdi_rate(0x4138) == 9600, "0x4138 = 9600");
    CHECK(sb_ftdi_rate(0x8003) == 923077, "0x8003 = 923076.9 -> %u", sb_ftdi_rate(0x8003));
    CHECK(sb_ftdi_rate(0x4006) == 461538, "0x4006 = 461538.5 -> %u", sb_ftdi_rate(0x4006));
    CHECK(sb_ftdi_rate(2) == 1500000, "D 2 = 1.5M");
}

static void t_ftdi_special(void) {
    CHECK(sb_ftdi_value(0) == sb_ftdi_value(1), "0 counts as 1");
    // <= 12 eighths: the closer of 8 and 12 (eighths rounded first), tie -> 8.
    CHECK(sb_ftdi_value(10000000) == 0, "10M -> 0");
    CHECK(sb_ftdi_value(0xFFFFFFFFu) == 0, "huge -> 0");
    CHECK(sb_ftdi_value(2400000) == 0, "10 eighths: tie -> D 1");
    CHECK(sb_ftdi_value(2181818) == 1, "11 eighths -> D 1.5");
    CHECK(sb_ftdi_value(2666667) == 0, "9 eighths -> D 1");
    CHECK(sb_ftdi_value(1846154) == 2, "13 eighths -> D 2");
    CHECK(sb_ftdi_value(1600000) == 2, "15 eighths -> D 2");
    CHECK(sb_ftdi_value(1500000) == 2, "16 eighths -> D 2");
    CHECK(sb_ftdi_value(1411765) == (2u | (3u << 14)),"17 eighths -> 2 + 1/8");
    // the cap: above 16383 7/8 -> exactly 16383 7/8 = whole 0x3FFF, code 7 in bits 14-16 = 0x1FFFF
    for (uint32_t b = 0; b <= 183; b++)
        CHECK(sb_ftdi_value(b) == 0x1FFFF, "%u -> 0x%05X, want capped 0x1FFFF", b, sb_ftdi_value(b));
    CHECK(sb_ftdi_rate(0x1FFFF) == 183, "16383 7/8 = 183.1 baud (%u)", sb_ftdi_rate(0x1FFFF));
    CHECK(sb_ftdi_value(184) == (16304u | (4u << 14)), "184: 130435 eighths = 16304 3/8, not capped (0x%05X)",
          sb_ftdi_value(184));
    // bits above 16 are ignored by sb_ftdi_rate
    CHECK(sb_ftdi_rate(0x2001A) == sb_ftdi_rate(0x1A) && sb_ftdi_rate(0xFFFE0000u) == 3000000 &&
              sb_ftdi_rate(0x20001) == 2000000,
          "bits above 16 ignored");
}

static void t_ftdi_sweep(void) {
    long n = 0;
    for (uint32_t b = 1; b <= 3300000; b += (b < 20000 ? 1 : 7), n++) {
        uint32_t v = sb_ftdi_value(b);
        uint32_t whole = v & 0x3FFF;
        uint64_t want = nearest_eighths(b);
        CHECK(v < (1u << 17), "%u: value 0x%X over 17 bits", b, v);
        CHECK(whole <= 16383, "%u: whole %u", b, whole);
        CHECK(whole != 1 || v == 1, "%u: whole part 1 in 0x%X", b, v);
        CHECK(whole != 0 || v == 0, "%u: whole part 0 in 0x%X", b, v);
        if (want <= 12) {
            uint32_t exp = (12 - want < (want > 8 ? want - 8 : 8 - want)) ? 1 : 0;
            CHECK(v == exp, "%u: %llu eighths -> 0x%X, want %u", b, (unsigned long long)want, v, exp);
        } else if (want <= 15) {
            CHECK(v == 2, "%u: %llu eighths -> 0x%X, want D 2", b, (unsigned long long)want, v);
        } else if (want <= 16383 * 8 + 7) {
            CHECK(ftdi_eighths(v) == want, "%u: %llu eighths, want %llu (nearest eighth)", b,
                  (unsigned long long)ftdi_eighths(v), (unsigned long long)want);
            uint32_t r = sb_ftdi_rate(v);
            uint64_t rr = (48000000ull + want) / (2 * want);
            CHECK(r == rr, "%u: rate %u, want %llu", b, r, (unsigned long long)rr);
        } else {
            CHECK(v == 0x1FFFF, "%u: capped at 16383 7/8 (0x%05X)", b, v);
        }
    }
    printf("ftdi sweep: %ld rates checked\n", n);
}

static void t_ftdi_standard(void) {
    for (size_t i = 0; i < sizeof(STANDARD) / sizeof(STANDARD[0]); i++) {
        uint32_t b = STANDARD[i];
        uint32_t r = sb_ftdi_rate(sb_ftdi_value(b));
        long long err = (long long)r - b;
        if (err < 0) err = -err;
        // nearest eighth: |D_chosen - D_exact| <= 1/16, i.e. the rate is off by at most 1/(16 D) of itself
        CHECK(err * 50 <= b, "%u -> %u, more than 2 %% off", b, r);
    }
    const uint32_t exact[] = {300, 1200, 9600, 19200, 1000000, 1500000, 2000000, 3000000};
    for (size_t i = 0; i < sizeof(exact) / sizeof(exact[0]); i++)
        CHECK(sb_ftdi_rate(sb_ftdi_value(exact[i])) == exact[i], "%u not exact (%u)", exact[i],
              sb_ftdi_rate(sb_ftdi_value(exact[i])));
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "selftest") != 0) {
        fprintf(stderr, "usage: serial_baud_harness selftest\n");
        return 2;
    }
    t_ch34x_expected();
    t_ch34x_standard();
    t_ch34x_ties();
    t_ch34x_clamp();
    t_ch34x_version_bit();
    t_ch34x_rate();
    t_ch34x_sweep();
    t_ftdi_expected();
    t_ftdi_special();
    t_ftdi_sweep();
    t_ftdi_standard();
    printf("%s: %ld checks, %d failure(s)\n", g_fail ? "FAILED" : "all passed", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

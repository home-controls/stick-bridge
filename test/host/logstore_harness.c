// Host test of main/logstore_core.c on a simulated NOR flash (built with ASan + UBSan by test_logstore.py).
//
// The flash behaves like the real thing: erase sets a sector to 0xFF, a write can only clear bits (writing
// over programmed bytes is reported as a bug), and a "power cut" stops a write or erase partway, leaving
// the bytes after the cut point unchanged (an erase cut halfway leaves garbage).
//
//   logstore_harness selftest            runs every scenario, prints "ok <n>" per check or "FAIL ..."
//   logstore_harness stamp <boot_ms> <line>   prints the stamped line (for the Python date checks)
//   logstore_harness parse <stamp>       prints the ms the stamp parses to

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "logstore_core.h"

static uint8_t *g_mem;
static uint32_t g_size;
static long g_budget = -1;  // bytes left before a power cut (-1 = never)
static int g_overwrites;
static unsigned g_seed = 1;
static unsigned rnd(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) & 0xFFFFFF;
}

static int f_read(void *ctx, uint32_t off, void *buf, size_t n) {
    (void)ctx;
    if (off + n > g_size) return -1;
    memcpy(buf, g_mem + off, n);
    return 0;
}
static int f_write(void *ctx, uint32_t off, const void *buf, size_t n) {
    (void)ctx;
    if (off + n > g_size) return -1;
    const uint8_t *b = buf;
    for (size_t i = 0; i < n; i++) {
        if (g_budget == 0) return -1;
        if (g_budget > 0) g_budget--;
        if ((g_mem[off + i] & b[i]) != b[i]) g_overwrites++;
        g_mem[off + i] &= b[i];
    }
    return 0;
}
static int f_erase(void *ctx, uint32_t off, size_t n) {
    (void)ctx;
    if (off % LS_SECTOR || n % LS_SECTOR || off + n > g_size) return -1;
    if (g_budget >= 0 && g_budget < 64) {  // cut mid-erase: part erased, part garbage
        for (size_t i = 0; i < n; i++) g_mem[off + i] = i < n / 2 ? 0xFF : (uint8_t)rnd();
        g_budget = 0;
        return -1;
    }
    if (g_budget > 0) g_budget -= 64;
    memset(g_mem + off, 0xFF, n);
    return 0;
}
static const ls_flash_t FL = {f_read, f_write, f_erase, NULL};

static int g_checks, g_fail;
#define CHECK(cond, ...)                         \
    do {                                         \
        g_checks++;                              \
        if (!(cond)) {                           \
            g_fail++;                            \
            printf("FAIL line %d: ", __LINE__);  \
            printf(__VA_ARGS__);                 \
            printf("\n");                        \
        }                                        \
    } while (0)

// Everything readable, oldest to newest.
static char *read_all(ls_ring_t *r, size_t *len) {
    size_t cap = g_size + 4000, n = 0;
    char *out = malloc(cap);
    ls_cursor_t c;
    ls_cursor_oldest(r, &c);
    for (;;) {
        size_t k = ls_read(r, &c, out + n, 1 + rnd() % 3000);
        if (!k) break;
        n += k;
        if (n > g_size) {  // more text than the area holds: the reader is looping
            printf("FAIL read_all: %zu bytes from a %u-byte area\n", n, g_size);
            g_fail++;
            break;
        }
    }
    out[n] = 0;
    *len = n;
    return out;
}

static void line(char *buf, size_t cap, unsigned i) {
    snprintf(buf, cap, "I (%u) test: line %u %.*s\n", i * 7, i, (int)(rnd() % 120), "abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz");
}

static int ends_with(const char *s, size_t n, const char *suf, size_t m) { return m <= n && memcmp(s + n - m, suf, m) == 0; }

static void fresh(uint32_t size, uint8_t fill) {
    free(g_mem);
    g_size = size;
    g_mem = malloc(size);
    memset(g_mem, fill, size);
    g_budget = -1;
    g_overwrites = 0;
}

static void test_blank_and_garbage(void) {
    ls_ring_t r;
    fresh(16 * LS_SECTOR, 0xFF);
    CHECK(ls_mount(&r, &FL, g_size, 0) == 0, "mount blank");
    CHECK(ls_used(&r) == 0, "blank used %u", ls_used(&r));
    size_t n;
    char *t = read_all(&r, &n);
    CHECK(n == 0, "blank read %zu", n);
    free(t);
    for (unsigned i = 0; i < g_size; i++) g_mem[i] = (uint8_t)rnd();
    CHECK(ls_mount(&r, &FL, g_size, 0) == 0, "mount garbage");
    t = read_all(&r, &n);
    CHECK(n == 0, "garbage read %zu", n);
    free(t);
    CHECK(ls_append(&r, "hello\n", 6) == 0, "append on garbage");
    t = read_all(&r, &n);
    CHECK(n == 6 && memcmp(t, "hello\n", 6) == 0, "garbage then hello: %zu '%s'", n, t);
    free(t);
    CHECK(g_overwrites == 0, "overwrites %d", g_overwrites);
}

// Appends lots, remounting now and then; what's readable must always be the tail of what was written,
// at least (sectors - 1) sectors' worth once that much was written.
static void test_wrap_and_remount(uint32_t sectors) {
    fresh(sectors * LS_SECTOR, 0xFF);
    size_t cap = 4 * 1024 * 1024, wn = 0;
    char *written = malloc(cap);
    ls_ring_t r;
    ls_mount(&r, &FL, g_size, 0);
    char l[200];
    for (unsigned i = 0; wn < (size_t)sectors * LS_SECTOR * 3 && wn + 300 < cap; i++) {
        line(l, sizeof(l), i);
        size_t k = strlen(l);
        CHECK(ls_append(&r, l, k) == 0, "append %u", i);
        memcpy(written + wn, l, k);
        wn += k;
        if (rnd() % 97 == 0) ls_mount(&r, &FL, g_size, 0);
        if (rnd() % 211 == 0 || i % 1000 == 999) {
            size_t n;
            char *t = read_all(&r, &n);
            CHECK(ends_with(written, wn, t, n), "tail property after %u lines (read %zu of %zu)", i, n, wn);
            size_t want = wn < (sectors - 1) * LS_TEXT ? wn : (sectors - 1) * LS_TEXT;
            CHECK(n >= want, "kept %zu < %zu", n, want);
            CHECK(n <= (size_t)sectors * LS_TEXT, "kept too much %zu", n);
            CHECK(ls_used(&r) == n, "used %u vs read %zu", ls_used(&r), n);
            free(t);
        }
    }
    // tail cursor
    ls_cursor_t c;
    ls_cursor_tail(&r, 5000, &c);
    char *buf = malloc(g_size);
    size_t n = 0, k;
    while ((k = ls_read(&r, &c, buf + n, 777)) > 0) n += k;
    CHECK(n >= 5000 && n < 5000 + 2 * LS_TEXT, "tail 5000 gave %zu", n);
    CHECK(ends_with(written, wn, buf, n), "tail is the end");
    free(buf);
    CHECK(g_overwrites == 0, "overwrites %d", g_overwrites);
    free(written);
}

// Power cuts at random points during appends: after remounting, the log is still a tail of what was
// written (minus at most the write that was cut), no programmed byte gets written over, and appending works.
static void test_power_cuts(void) {
    for (int round = 0; round < 400; round++) {
        uint32_t sectors = 3 + rnd() % 6;
        fresh(sectors * LS_SECTOR, round % 3 ? 0xFF : 0x00);
        ls_ring_t r;
        ls_mount(&r, &FL, g_size, 0);
        size_t cap = 512 * 1024, wn = 0;
        char *written = malloc(cap);
        char l[200];
        unsigned i = 0;
        for (int cut = 0; cut < 6; cut++) {
            g_budget = rnd() % (3 * LS_SECTOR);
            size_t before = wn;
            for (;; i++) {
                line(l, sizeof(l), i);
                size_t k = strlen(l);
                int rc = ls_append(&r, l, k);
                if (rc != 0 || g_budget == 0) {
                    // the cut write may have landed partly: what is on flash is unknown for this line only
                    break;
                }
                memcpy(written + wn, l, k);
                wn += k;
                if (wn + 300 > cap) break;
            }
            (void)before;
            g_budget = -1;
            ls_mount(&r, &FL, g_size, 0);
            size_t n;
            char *t = read_all(&r, &n);
            // Drop a trailing partial line from the read (the cut one), then it must be a tail of written.
            size_t cut_at = n;
            while (cut_at && t[cut_at - 1] != '\n') cut_at--;
            int ok = ends_with(written, wn, t, cut_at);
            if (!ok) {
                // The cut line may have been fully written before the cut: then it's the newest complete line.
                size_t prev = cut_at ? cut_at - 1 : 0;
                while (prev && t[prev - 1] != '\n') prev--;
                ok = ends_with(written, wn, t, prev);
            }
            CHECK(ok, "round %d cut %d: not a tail (read %zu, written %zu)", round, cut, n, wn);
            if (!ok && g_fail < 3) {
                size_t m = 0;  // longest common suffix
                while (m < cut_at && m < wn && t[cut_at - 1 - m] == written[wn - 1 - m]) m++;
                printf("  common suffix %zu; read before it: '%.60s'\n  written before it: '%.60s'\n", m,
                       cut_at - m >= 60 ? t + cut_at - m - 60 : t, wn - m >= 60 ? written + wn - m - 60 : written);
                printf("  head %u fill %u oldest %u sectors %u\n", r.head_seq, r.head_fill, r.oldest_seq, r.sectors);
            }
            // Lines before the cut must not be lost beyond the ring's capacity minus 2 sectors (one reused, one cut).
            size_t want = wn < (sectors - 2) * LS_TEXT ? wn : (sectors - 2) * LS_TEXT;
            CHECK(cut_at + LS_TEXT >= want, "round %d cut %d: kept %zu want %zu", round, cut, cut_at, want);
            free(t);
            // Carry on from a clean state: the reference resyncs to what the flash holds now.
            t = read_all(&r, &n);
            size_t full = n;
            while (full && t[full - 1] != '\n') full--;
            if (n != full) {  // finish the cut line so later checks compare whole lines
                ls_append(&r, "\n", 1);
                memcpy(written, t, n);
                written[n] = '\n';
                wn = n + 1;
            } else {
                memcpy(written, t, n);
                wn = n;
            }
            free(t);
        }
        CHECK(g_overwrites == 0, "round %d: %d overwrites", round, g_overwrites);
        free(written);
    }
}

static void test_clear_and_floor(void) {
    fresh(8 * LS_SECTOR, 0xFF);
    ls_ring_t r;
    ls_mount(&r, &FL, g_size, 0);
    char l[200];
    for (unsigned i = 0; i < 100; i++) {
        line(l, sizeof(l), i);
        ls_append(&r, l, strlen(l));
    }
    uint32_t floor = ls_clear(&r);
    size_t n;
    char *t = read_all(&r, &n);
    CHECK(n == 0, "after clear %zu", n);
    free(t);
    ls_append(&r, "after\n", 6);
    t = read_all(&r, &n);
    CHECK(n == 6 && !memcmp(t, "after\n", 6), "after clear append %zu", n);
    free(t);
    // remount with the floor: same; remount without it: old text is back (the floor is what hides it)
    ls_mount(&r, &FL, g_size, floor);
    t = read_all(&r, &n);
    CHECK(n == 6, "remount with floor %zu", n);
    free(t);
    ls_mount(&r, &FL, g_size, 0);
    t = read_all(&r, &n);
    CHECK(n > 6, "remount without floor shows old %zu", n);
    free(t);
    // clear, remount with floor before anything new is written: empty, and new text shows
    ls_mount(&r, &FL, g_size, floor);
    floor = ls_clear(&r);
    ls_mount(&r, &FL, g_size, floor);
    t = read_all(&r, &n);
    CHECK(n == 0, "cleared then remounted %zu", n);
    free(t);
    ls_append(&r, "x\n", 2);
    t = read_all(&r, &n);
    CHECK(n == 2, "append after cleared remount %zu", n);
    free(t);
    CHECK(r.head_seq >= floor, "head %u floor %u", r.head_seq, floor);
    CHECK(g_overwrites == 0, "overwrites %d", g_overwrites);
}

// A reader holding a cursor while the writer laps the ring: never returns text out of order or garbage.
static void test_read_while_lapping(void) {
    fresh(4 * LS_SECTOR, 0xFF);
    ls_ring_t r;
    ls_mount(&r, &FL, g_size, 0);
    char l[200];
    unsigned i = 0;
    for (; i < 200; i++) {
        line(l, sizeof(l), i);
        ls_append(&r, l, strlen(l));
    }
    ls_cursor_t c;
    ls_cursor_oldest(&r, &c);
    char buf[300];
    size_t k = ls_read(&r, &c, buf, 100);
    CHECK(k == 100, "first read %zu", k);
    for (; i < 600; i++) {
        line(l, sizeof(l), i);
        ls_append(&r, l, strlen(l));
    }
    k = ls_read(&r, &c, buf, sizeof(buf) - 1);
    buf[k] = 0;
    CHECK(k > 0, "read after lap");
    // must be from the current oldest sector, i.e. start at its text start, not the stale offset
    CHECK(c.seq >= r.oldest_seq, "cursor %u oldest %u", c.seq, r.oldest_seq);
}

static void test_stamps(void) {
    char out[64];
    size_t n = ls_stamp(1795495336123LL, 0, out);
    CHECK(!strcmp(out, "2026-11-24T04:42:16.123Z ") && n == 25, "stamp '%s'", out);
    ls_stamp(0, 12345, out);
    CHECK(!strcmp(out, "boot+12.345s "), "boot stamp '%s'", out);
    CHECK(ls_parse_stamp("2026-11-24T04:42:16.123Z", 24) == 1795495336123LL, "parse");
    CHECK(ls_parse_stamp("boot+12.345s", 12) == 0, "parse boot");
    CHECK(ls_parse_stamp("2026-13-24T04:42:16.123Z", 24) == 0, "parse bad month");
    const char *in = "I (1000) a: one\nraw continuation\nW (2500) b: two\nE (3";
    char o[512];
    uint32_t last = 0;
    size_t used;
    size_t on = ls_stamp_lines(in, strlen(in), 1795495336000LL, &last, o, sizeof(o), &used);
    o[on] = 0;
    CHECK(used == strlen(in) - 4, "used %zu", used);
    CHECK(!strcmp(o, "2026-11-24T04:42:17.000Z I (1000) a: one\n2026-11-24T04:42:17.000Z raw continuation\n"
                     "2026-11-24T04:42:18.500Z W (2500) b: two\n"), "lines '%s'", o);
    CHECK(last == 2500, "last %u", last);
    // too small an output: stops at a whole line
    on = ls_stamp_lines(in, strlen(in), 1795495336000LL, &last, o, 60, &used);
    CHECK(on == 41 && used == 16, "small cap on=%zu used=%zu", on, used);
    on = ls_note(0, 5, "Boot 3", o, sizeof(o));
    o[on] = 0;
    CHECK(!strcmp(o, "boot+0.005s # Boot 3\n"), "note '%s'", o);
    on = ls_note(0, 5, "a long note that won't fit", o, 16);
    CHECK(on == 15 && o[14] == '\n', "note cut %zu", on);
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "stamp")) {
        char out[600];
        uint32_t last = 0;
        size_t used;
        char in[512];
        snprintf(in, sizeof(in), "%s\n", argv[3]);
        size_t n = ls_stamp_lines(in, strlen(in), atoll(argv[2]), &last, out, sizeof(out), &used);
        fwrite(out, 1, n, stdout);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "parse")) {
        printf("%lld\n", (long long)ls_parse_stamp(argv[2], strlen(argv[2])));
        return 0;
    }
    test_blank_and_garbage();
    test_wrap_and_remount(5);
    test_wrap_and_remount(64);
    test_power_cuts();
    test_clear_and_floor();
    test_read_while_lapping();
    test_stamps();
    printf("%s %d checks, %d failed\n", g_fail ? "FAIL" : "ok", g_checks, g_fail);
    free(g_mem);
    return g_fail ? 1 : 0;
}

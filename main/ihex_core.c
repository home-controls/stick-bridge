#include "ihex_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void fail(ihex_t *h, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fail(ihex_t *h, const char *fmt, ...) {
    if (h->err[0]) return;  // keep the first error
    int n = snprintf(h->err, sizeof(h->err), "line %lu: ", (unsigned long)h->line);
    if (n < 0 || (size_t)n >= sizeof(h->err)) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->err + n, sizeof(h->err) - (size_t)n, fmt, ap);
    va_end(ap);
}

void ihex_fail(ihex_t *h, const char *msg) { fail(h, "%s", msg); }

void ihex_init(ihex_t *h, ihex_data_cb cb, void *ctx) {
    memset(h, 0, sizeof(*h));
    h->cb = cb;
    h->ctx = ctx;
    h->line = 1;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Adds [lo, hi) to the range table; false (with the error set) on overlap or a full table.
static bool add_range(ihex_t *h, uint32_t lo, uint32_t hi) {
    for (uint8_t i = 0; i < h->nranges; i++) {
        if (lo < h->ranges[i].hi && h->ranges[i].lo < hi) {
            fail(h, "data at 0x%08lx overlaps bytes an earlier record already wrote (0x%08lx-0x%08lx)", (unsigned long)lo,
                 (unsigned long)h->ranges[i].lo, (unsigned long)(h->ranges[i].hi - 1));
            return false;
        }
    }
    // Merge with neighbours (at most one on each side, since the ranges are disjoint and non-touching).
    int left = -1, right = -1;
    for (uint8_t i = 0; i < h->nranges; i++) {
        if (h->ranges[i].hi == lo) left = i;
        if (h->ranges[i].lo == hi) right = i;
    }
    if (left >= 0 && right >= 0) {
        h->ranges[left].hi = h->ranges[right].hi;
        h->ranges[right] = h->ranges[--h->nranges];
    } else if (left >= 0) {
        h->ranges[left].hi = hi;
    } else if (right >= 0) {
        h->ranges[right].lo = lo;
    } else {
        if (h->nranges == IHEX_MAX_RANGES) {
            fail(h, "the file is split into more than %d separate address ranges (too fragmented to check)", IHEX_MAX_RANGES);
            return false;
        }
        h->ranges[h->nranges].lo = lo;
        h->ranges[h->nranges].hi = hi;
        h->nranges++;
    }
    return true;
}

static void record(ihex_t *h) {
    const uint8_t *r = h->rec;
    uint8_t n = r[0];
    uint16_t off = (uint16_t)((r[1] << 8) | r[2]);
    uint8_t type = r[3];
    uint8_t sum = 0;
    for (uint16_t i = 0; i < h->rn; i++) sum = (uint8_t)(sum + r[i]);
    if (sum != 0) {
        fail(h, "checksum mismatch (record says 0x%02x, should be 0x%02x)", r[h->rn - 1],
             (uint8_t)(r[h->rn - 1] - sum));
        return;
    }
    h->records++;
    if (h->eof_seen) {
        fail(h, "a record after the end-of-file record");
        return;
    }
    const uint8_t *d = r + 4;
    switch (type) {
        case 0x00: {
            if (n == 0) return;  // legal, writes nothing
            if ((uint32_t)off + n > 0x10000u) {
                fail(h, "data record runs past the end of its 64 KB segment (offset 0x%04x + %u bytes)", off, n);
                return;
            }
            uint64_t a = (uint64_t)h->base + off;
            if (a + n > 0x100000000ull) {
                fail(h, "data record runs past 4 GB");
                return;
            }
            uint32_t lo = (uint32_t)a, hi = (uint32_t)(a + n);
            if (!add_range(h, lo, hi)) return;
            if (h->total_bytes == 0 || lo < h->min_addr) h->min_addr = lo;
            if (hi > h->end_addr) h->end_addr = hi;
            h->total_bytes += n;
            if (h->cb && h->cb(h->ctx, lo, d, n) != 0) fail(h, "rejected by the caller at 0x%08lx", (unsigned long)lo);
            return;
        }
        case 0x01:
            if (n != 0) {
                fail(h, "end-of-file record with %u data bytes", n);
                return;
            }
            h->eof_seen = true;
            return;
        case 0x02:
            if (n != 2) {
                fail(h, "extended segment address record with %u data bytes (should be 2)", n);
                return;
            }
            h->base = (uint32_t)((d[0] << 8) | d[1]) << 4;
            return;
        case 0x04:
            if (n != 2) {
                fail(h, "extended linear address record with %u data bytes (should be 2)", n);
                return;
            }
            h->base = (uint32_t)((d[0] << 8) | d[1]) << 16;
            return;
        case 0x03:
        case 0x05:
            if (n != 4) fail(h, "start address record with %u data bytes (should be 4)", n);
            return;
        default:
            fail(h, "unknown record type 0x%02x", type);
            return;
    }
}

static void end_line(ihex_t *h) {
    if (h->in_rec) {
        if (h->rn < 5) fail(h, "record cut short (%u bytes, needs at least 5)", h->rn);
        else fail(h, "record cut short (%u of %u bytes)", h->rn, h->rec[0] + 5u);
        return;
    }
    h->rec_done = false;
}

const char *ihex_feed(ihex_t *h, const char *buf, size_t len) {
    for (size_t i = 0; i < len && !h->err[0]; i++) {
        char c = buf[i];
        if (c == '\n' || c == '\r') {
            bool second_of_crlf = (c == '\n' && h->last_cr);
            h->last_cr = (c == '\r');
            if (second_of_crlf) continue;
            if (h->in_rec && h->half) {
                fail(h, "odd number of hex digits");
                break;
            }
            end_line(h);
            if (h->err[0]) break;
            h->line++;
            continue;
        }
        h->last_cr = false;
        if (h->in_rec) {
            int v = hexval(c);
            if (v < 0) {
                fail(h, "'%c' is not a hex digit", (c >= 0x20 && c < 0x7F) ? c : '?');
                break;
            }
            if (!h->half) {
                h->hi = (uint8_t)v;
                h->half = true;
                continue;
            }
            h->half = false;
            h->rec[h->rn++] = (uint8_t)((h->hi << 4) | v);
            if (h->rn >= 5 && h->rn == h->rec[0] + 5u) {
                h->in_rec = false;
                h->rec_done = true;
                record(h);
            }
            continue;
        }
        if (c == ' ' || c == '\t') continue;
        if (c == ':') {
            if (h->rec_done) {
                fail(h, "two records on one line");
                break;
            }
            if (h->eof_seen) {
                fail(h, "a record after the end-of-file record");
                break;
            }
            h->in_rec = true;
            h->rn = 0;
            h->half = false;
            continue;
        }
        if (h->rec_done) fail(h, "extra characters after the record");
        else if (h->eof_seen) fail(h, "data after the end-of-file record");
        else fail(h, "unexpected '%c' outside a record (not an Intel HEX file?)", (c >= 0x20 && c < 0x7F) ? c : '?');
    }
    return h->err[0] ? h->err : NULL;
}

const char *ihex_finish(ihex_t *h) {
    if (h->err[0]) return h->err;
    if (h->in_rec) {
        if (h->half) fail(h, "odd number of hex digits");
        else end_line(h);
        return h->err;
    }
    if (!h->eof_seen) {
        fail(h, "the file ends without an end-of-file record (cut short?)");
        return h->err;
    }
    return NULL;
}

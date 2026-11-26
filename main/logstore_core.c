#include "logstore_core.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t magic, seq, nseq, format;
} hdr_t;

static uint32_t sector_of(const ls_ring_t *r, uint32_t seq) { return seq % r->sectors; }

// The header's sequence number, or 0 if the sector holds no valid header.
static uint32_t hdr_seq(const ls_ring_t *r, uint32_t sector) {
    hdr_t h;
    if (r->fl.read(r->fl.ctx, sector * LS_SECTOR, &h, sizeof(h)) != 0) return 0;
    if (h.magic != LS_MAGIC || h.seq != ~h.nseq || h.seq == 0 || h.format != 1) return 0;
    if (h.seq % r->sectors != sector) return 0;
    return h.seq;
}

// Text bytes in a sector: up to the first 0xFF.
static uint32_t text_len(const ls_ring_t *r, uint32_t sector) {
    uint8_t buf[256];
    for (uint32_t off = 0; off < LS_TEXT; off += sizeof(buf)) {
        uint32_t k = LS_TEXT - off < sizeof(buf) ? LS_TEXT - off : sizeof(buf);
        if (r->fl.read(r->fl.ctx, sector * LS_SECTOR + LS_HDR + off, buf, k) != 0) return off;
        for (uint32_t i = 0; i < k; i++)
            if (buf[i] == 0xFF) return off + i;
    }
    return LS_TEXT;
}

int ls_mount(ls_ring_t *r, const ls_flash_t *fl, uint32_t size, uint32_t floor_seq) {
    memset(r, 0, sizeof(*r));
    r->fl = *fl;
    r->sectors = size / LS_SECTOR;
    r->floor_seq = floor_seq;
    if (r->sectors < 2) return -1;
    uint32_t best = 0;
    for (uint32_t i = 0; i < r->sectors; i++) {
        uint32_t s = hdr_seq(r, i);
        if (s > best) best = s;
    }
    if (best == 0) {
        // Nothing stored yet (or everything cleared at the format level). Continue numbering above the floor.
        r->head_seq = floor_seq ? floor_seq - 1 : 0;
        r->head_fill = LS_TEXT;  // the first append opens a fresh sector
        r->oldest_seq = r->head_seq + 1;
        return 0;
    }
    r->head_seq = best;
    r->head_fill = text_len(r, sector_of(r, best));
    // Walk back while the sectors are consecutive and above the floor.
    uint32_t oldest = best;
    for (uint32_t k = 1; k < r->sectors && oldest > 1; k++) {
        uint32_t s = oldest - 1;
        if (s < floor_seq || hdr_seq(r, sector_of(r, s)) != s) break;
        oldest = s;
    }
    r->oldest_seq = oldest;
    if (best < floor_seq) {  // everything is below the floor
        r->head_fill = LS_TEXT;
        r->oldest_seq = best + 1;
    }
    return 0;
}

static int open_sector(ls_ring_t *r) {
    uint32_t seq = r->head_seq + 1;
    if (seq < r->floor_seq) seq = r->floor_seq;
    uint32_t sector = sector_of(r, seq);
    // The sector we're about to reuse may be the oldest one: move the oldest forward first, so a failed erase
    // doesn't leave a reader pointed at it.
    if (seq >= r->sectors && r->oldest_seq <= seq - r->sectors + 1) r->oldest_seq = seq - r->sectors + 1;
    if (r->fl.erase(r->fl.ctx, sector * LS_SECTOR, LS_SECTOR) != 0) return -1;
    hdr_t h = {.magic = LS_MAGIC, .seq = seq, .nseq = ~seq, .format = 1};
    if (r->fl.write(r->fl.ctx, sector * LS_SECTOR, &h, sizeof(h)) != 0) return -1;
    if (r->head_seq < r->oldest_seq || r->oldest_seq < r->floor_seq) r->oldest_seq = seq;
    r->head_seq = seq;
    r->head_fill = 0;
    return 0;
}

int ls_append(ls_ring_t *r, const char *text, size_t n) {
    char clean[256];
    while (n) {
        if (r->head_seq == 0 || r->head_fill >= LS_TEXT || r->head_seq < r->floor_seq) {
            if (open_sector(r) != 0) return -1;
        }
        size_t room = LS_TEXT - r->head_fill;
        size_t k = 0, used = 0;
        while (used < n && k < room && k < sizeof(clean)) {
            char c = text[used++];
            if ((uint8_t)c != 0xFF) clean[k++] = c;
        }
        if (k && r->fl.write(r->fl.ctx, sector_of(r, r->head_seq) * LS_SECTOR + LS_HDR + r->head_fill, clean, k) != 0) {
            r->head_fill = LS_TEXT;  // don't write over a half-written spot: start a new sector next time
            return -1;
        }
        r->head_fill += k;
        text += used;
        n -= used;
    }
    return 0;
}

uint32_t ls_clear(ls_ring_t *r) {
    r->floor_seq = r->head_seq + 1;
    r->oldest_seq = r->floor_seq;
    r->head_fill = LS_TEXT;
    return r->floor_seq;
}

uint32_t ls_used(const ls_ring_t *r) {
    if (r->head_seq == 0 || r->head_seq < r->oldest_seq) return 0;
    return (r->head_seq - r->oldest_seq) * LS_TEXT + r->head_fill;
}

void ls_cursor_oldest(const ls_ring_t *r, ls_cursor_t *c) {
    c->seq = r->oldest_seq;
    c->off = 0;
}

void ls_cursor_tail(const ls_ring_t *r, uint32_t bytes, ls_cursor_t *c) {
    uint32_t back = bytes > r->head_fill ? (bytes - r->head_fill + LS_TEXT - 1) / LS_TEXT : 0;
    c->seq = r->head_seq > back ? r->head_seq - back : 0;
    if (c->seq < r->oldest_seq) c->seq = r->oldest_seq;
    c->off = 0;
}

size_t ls_read(ls_ring_t *r, ls_cursor_t *c, char *buf, size_t n) {
    while (n && r->head_seq && c->seq <= r->head_seq) {
        if (c->seq < r->oldest_seq) {
            c->seq = r->oldest_seq;
            c->off = 0;
            continue;
        }
        uint32_t sector = sector_of(r, c->seq);
        if (hdr_seq(r, sector) != c->seq) {  // gone (reused or never written): next one
            c->seq++;
            c->off = 0;
            continue;
        }
        uint32_t limit = c->seq == r->head_seq ? r->head_fill : LS_TEXT;
        if (c->off >= limit) {
            if (c->seq == r->head_seq) return 0;
            c->seq++;
            c->off = 0;
            continue;
        }
        size_t k = limit - c->off < n ? limit - c->off : n;
        if (r->fl.read(r->fl.ctx, sector * LS_SECTOR + LS_HDR + c->off, buf, k) != 0) return 0;
        if (hdr_seq(r, sector) != c->seq) continue;  // reused while we read it: drop what we got
        size_t got = k;
        for (size_t i = 0; i < k; i++) {
            if ((uint8_t)buf[i] == 0xFF) {
                got = i;
                break;
            }
        }
        if (got < k) {  // end of this sector's text
            c->off = LS_TEXT;
        } else {
            c->off += got;
        }
        if (got) return got;
    }
    return 0;
}

// ---------------------------------------------------------------- stamps

// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm), so no time zone is involved.
static void civil(int64_t days, int *y, unsigned *m, unsigned *d) {
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    unsigned doe = (unsigned)(days - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yy + (*m <= 2));
}

static int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

size_t ls_stamp(int64_t boot_epoch_ms, uint32_t ms, char *out) {
    if (boot_epoch_ms <= 0) return (size_t)snprintf(out, 32, "boot+%lu.%03lus ", (unsigned long)(ms / 1000), (unsigned long)(ms % 1000));
    int64_t t = boot_epoch_ms + ms;
    int64_t secs = t / 1000, days = secs / 86400;
    int64_t sod = secs % 86400;
    int y;
    unsigned mo, d;
    civil(days, &y, &mo, &d);
    return (size_t)snprintf(out, 32, "%04d-%02u-%02uT%02u:%02u:%02u.%03uZ ", y, mo, d, (unsigned)(sod / 3600),
                            (unsigned)(sod / 60 % 60), (unsigned)(sod % 60), (unsigned)(t % 1000));
}

// "X (1234) " at the start of a line: the ms number, else -1.
static int64_t line_ms(const char *p, size_t n) {
    if (n < 5 || !strchr("EWIDV", p[0]) || p[1] != ' ' || p[2] != '(') return -1;
    int64_t v = 0;
    size_t i = 3;
    for (; i < n && p[i] >= '0' && p[i] <= '9' && i < 14; i++) v = v * 10 + (p[i] - '0');
    if (i == 3 || i >= n || p[i] != ')') return -1;
    return v;
}

size_t ls_stamp_lines(const char *in, size_t n, int64_t boot_epoch_ms, uint32_t *last_ms, char *out, size_t cap,
                      size_t *used) {
    size_t o = 0, i = 0;
    *used = 0;
    while (i < n) {
        const char *nl = memchr(in + i, '\n', n - i);
        if (!nl) break;  // partial line: wait for the rest
        size_t len = (size_t)(nl - (in + i)) + 1;
        if (len > 1) {  // skip empty lines
            int64_t ms = line_ms(in + i, len);
            if (ms >= 0) *last_ms = (uint32_t)ms;
            char st[32];
            size_t sl = ls_stamp(boot_epoch_ms, *last_ms, st);
            if (o + sl + len > cap) break;
            memcpy(out + o, st, sl);
            memcpy(out + o + sl, in + i, len);
            o += sl + len;
        }
        i += len;
        *used = i;
    }
    return o;
}

size_t ls_note(int64_t boot_epoch_ms, uint32_t ms, const char *text, char *out, size_t cap) {
    char st[32];
    ls_stamp(boot_epoch_ms, ms, st);
    int n = snprintf(out, cap, "%s# %s\n", st, text);
    if (n < 0) return 0;
    if ((size_t)n >= cap) {
        out[cap - 2] = '\n';
        out[cap - 1] = 0;
        return cap - 1;
    }
    return (size_t)n;
}

static int num(const char *p, int k) {
    int v = 0;
    for (int i = 0; i < k; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

int64_t ls_parse_stamp(const char *s, size_t n) {
    // 2026-11-24T04:25:36.123Z
    if (n < 24 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != '.' || s[23] != 'Z')
        return 0;
    int y = num(s, 4), mo = num(s + 5, 2), d = num(s + 8, 2), h = num(s + 11, 2), mi = num(s + 14, 2), se = num(s + 17, 2),
        ms = num(s + 20, 3);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60 || ms < 0)
        return 0;
    return ((days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400) + h * 3600 + mi * 60 + se) * 1000 + ms;
}

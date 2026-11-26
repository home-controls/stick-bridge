#include "protodetect.h"

#include <string.h>

void pd_reset(pd_state_t *s) { memset(s, 0, sizeof(*s)); }

static uint16_t crc_ccitt(const uint8_t *d, size_t n) {  // ASH: poly 0x1021, init 0xFFFF, big-endian on the wire
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}

static uint16_t crc_x25(const uint8_t *d, size_t n) {  // HDLC/Spinel: reflected 0x1021, init/xorout 0xFFFF, LE
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return (uint16_t)~c;
}

// ASH reverses its data-field randomisation only for DATA frames; the CRC covers the frame as sent, so
// the check doesn't need it.
static bool ash_ok(const uint8_t *f, size_t n) {
    if (n < 3 || n > 140) return false;
    return crc_ccitt(f, n - 2) == (uint16_t)(f[n - 2] << 8 | f[n - 1]);
}

static bool spinel_ok(const uint8_t *f, size_t n) {
    if (n < 3) return false;
    return crc_x25(f, n - 2) == (uint16_t)(f[n - 2] | f[n - 1] << 8);
}

static bool deconz_ok(const uint8_t *f, size_t n) {  // data..., crc16 LE = two's complement of the byte sum
    if (n < 5) return false;
    uint16_t sum = 0;
    for (size_t i = 0; i < n - 2; i++) sum += f[i];
    uint16_t crc = (uint16_t)(f[n - 2] | f[n - 1] << 8);
    return (uint16_t)(sum + crc) == 0;
}

static bool zigate_ok(const uint8_t *f, size_t n) {  // type(2) len(2) chk data(len): chk = XOR of type, len, data
    if (n < 5) return false;
    uint16_t len = (uint16_t)(f[2] << 8 | f[3]);
    if ((size_t)len + 5 != n) return false;
    uint8_t x = f[0] ^ f[1] ^ f[2] ^ f[3];
    for (size_t i = 5; i < n; i++) x ^= f[i];
    return x == f[4];
}

static void good(pd_state_t *s, proto_t p) {
    if (s->decided == PROTO_UNKNOWN && ++s->good[p] >= PD_FRAMES_TO_DECIDE) s->decided = p;
}

static void put(pd_frame_t *f, uint8_t b) {
    if (f->len < PD_BUF) f->buf[f->len++] = b;
    else f->bad = true;
}

proto_t pd_feed(pd_state_t *s, int dir, const uint8_t *data, size_t len) {
    if (s->decided != PROTO_UNKNOWN) return s->decided;
    pd_dir_t *d = &s->dir[dir ? 1 : 0];
    for (size_t i = 0; i < len && s->decided == PROTO_UNKNOWN; i++) {
        uint8_t b = data[i];

        // HDLC-like (ASH / Spinel). ASH also uses 0x11/0x13 (XON/XOFF), 0x18 (substitute), 0x1A (cancel).
        pd_frame_t *h = &d->hdlc;
        if (b == 0x7E) {
            if (h->len && !h->bad) {
                if (ash_ok(h->buf, h->len)) good(s, PROTO_EZSP);
                else if (spinel_ok(h->buf, h->len)) good(s, PROTO_SPINEL);
            }
            h->len = 0;
            h->esc = h->bad = false;
        } else if (b == 0x1A) {
            h->len = 0;
            h->esc = h->bad = false;
        } else if (b == 0x11 || b == 0x13) {
            // flow control, not data
        } else if (b == 0x7D) {
            h->esc = true;
        } else {
            put(h, h->esc ? (uint8_t)(b ^ 0x20) : b);
            h->esc = false;
        }

        // SLIP (deCONZ)
        pd_frame_t *sl = &d->slip;
        if (b == 0xC0) {
            if (sl->len && !sl->bad && deconz_ok(sl->buf, sl->len)) good(s, PROTO_DECONZ);
            sl->len = 0;
            sl->esc = sl->bad = false;
        } else if (b == 0xDB) {
            sl->esc = true;
        } else {
            put(sl, sl->esc ? (b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b) : b);
            sl->esc = false;
        }

        // ZiGate
        pd_frame_t *z = &d->zigate;
        if (b == 0x01) {
            z->active = true;
            z->len = 0;
            z->esc = z->bad = false;
        } else if (b == 0x03 && z->active) {
            if (!z->bad && zigate_ok(z->buf, z->len)) good(s, PROTO_ZIGATE);
            z->active = false;
        } else if (z->active) {
            if (b == 0x02) z->esc = true;
            else {
                put(z, z->esc ? (uint8_t)(b ^ 0x10) : b);
                z->esc = false;
            }
        }

        // ZNP: 0xFE len cmd0 cmd1 data[len] fcs(XOR of len..data)
        if (!d->znp_in) {
            if (b == 0xFE) {
                d->znp_in = true;
                d->znp_len = 0;
                d->znp_need = 0;
            }
        } else {
            d->znp[d->znp_len++] = b;
            if (d->znp_len == 1) {
                if (b > 250) d->znp_in = false;
                else d->znp_need = (uint16_t)(b + 4);  // len, cmd0, cmd1, data, fcs
            } else if (d->znp_len == d->znp_need) {
                uint8_t x = 0;
                for (uint16_t k = 0; k + 1 < d->znp_len; k++) x ^= d->znp[k];
                if (x == d->znp[d->znp_len - 1]) good(s, PROTO_ZNP);
                d->znp_in = false;
            }
        }

        // ZBOSS NCP low-level frames start with the signature DE AD
        if (d->zboss_prev == 0xDE && b == 0xAD) good(s, PROTO_ZBOSS);
        d->zboss_prev = b;
    }
    return s->decided;
}

// ---------------------------------------------------------------- firmware version sniffing

void pd_ver_reset(pd_ver_t *v) { memset(v, 0, sizeof(*v)); }

bool pd_ver_feed(pd_ver_t *v, const uint8_t *data, size_t len) {
    bool learnt = false;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        // Spinel over HDLC-lite: 7E ... 7E, escape 7D, X.25 FCS
        pd_frame_t *h = &v->hdlc;
        if (b == 0x7E) {
            const uint8_t *f = h->buf;
            size_t n = h->len;
            if (n >= 6 && !h->bad && spinel_ok(f, n) && (f[0] & 0xC0) == 0x80 && f[1] == 0x06 && f[2] == 0x02) {
                char tmp[sizeof(v->spinel_version)];
                size_t k = 0;
                for (size_t j = 3; j < n - 2 && f[j] && k + 1 < sizeof(tmp); j++) tmp[k++] = (f[j] >= 0x20 && f[j] < 0x7F) ? (char)f[j] : '?';
                tmp[k] = 0;
                if (k && strcmp(tmp, v->spinel_version) != 0) {
                    memcpy(v->spinel_version, tmp, k + 1);
                    learnt = true;
                }
            }
            h->len = 0;
            h->esc = h->bad = false;
        } else if (b == 0x7D) {
            h->esc = true;
        } else {
            put(h, h->esc ? (uint8_t)(b ^ 0x20) : b);
            h->esc = false;
        }
        // ZNP: FE len cmd0 cmd1 data fcs; SYS_VERSION SRSP = 61 02: transport, product, major, minor, maint, revision u32 LE
        if (!v->znp_in) {
            if (b == 0xFE) {
                v->znp_in = true;
                v->znp_len = 0;
            }
        } else {
            if (v->znp_len < sizeof(v->znp)) v->znp[v->znp_len] = b;
            v->znp_len++;
            if (v->znp_len == 1) {
                if (b > 60) v->znp_in = false;  // longer than any frame we care about
                else v->znp_need = (uint16_t)(b + 4);
            } else if (v->znp_len == v->znp_need) {
                uint8_t x = 0;
                for (uint16_t k = 0; k + 1 < v->znp_len; k++) x ^= v->znp[k];
                const uint8_t *z = v->znp;
                if (x == z[v->znp_len - 1] && z[1] == 0x61 && z[2] == 0x02 && z[0] >= 9) {
                    uint32_t rev = (uint32_t)z[8] | (uint32_t)z[9] << 8 | (uint32_t)z[10] << 16 | (uint32_t)z[11] << 24;
                    if (!v->znp_seen || rev != v->znp_revision) learnt = true;
                    v->znp_seen = true;
                    v->znp_product = z[4];
                    v->znp_major = z[5];
                    v->znp_minor = z[6];
                    v->znp_maint = z[7];
                    v->znp_revision = rev;
                }
                v->znp_in = false;
            }
        }
    }
    return learnt;
}

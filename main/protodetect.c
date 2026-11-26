#include "protodetect.h"

#include <stdio.h>
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
            uint16_t k = ++d->znp_len;
            if (k == 1) {
                if (b > 250) d->znp_in = false;
                d->znp_need = (uint16_t)(b + 4);  // len, cmd0, cmd1, data, fcs
                d->znp_x = b;
            } else if (k == d->znp_need) {
                if (d->znp_x == b) good(s, PROTO_ZNP);  // fcs = XOR of len..data
                d->znp_in = false;
            } else {
                d->znp_x ^= b;
            }
        }

        // Z-Wave Serial API: SOF 01, len (>= 3: itself, type, cmd), type 00 request / 01 response, cmd,
        // data, checksum = 0xFF XOR len..data. ACK/NAK/CAN (06/15/18) are single bytes between frames.
        if (!d->zw_in) {
            if (b == 0x01) {
                d->zw_in = true;
                d->zw_len = 0;
            }
        } else {
            uint16_t k = ++d->zw_len;
            if (k == 1) {
                if (b < 3) d->zw_in = false;
                d->zw_need = (uint16_t)(b + 1);  // len..data plus the checksum
                d->zw_x = (uint8_t)(0xFF ^ b);
            } else if (k == 2 && b > 0x01) {
                d->zw_in = false;  // type is request or response
            } else if (k == d->zw_need) {
                if (d->zw_x == b) good(s, PROTO_ZWAVE);  // checksum = 0xFF XOR len..data
                d->zw_in = false;
            } else {
                d->zw_x ^= b;
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

// An ASH DATA frame (control, data, CRC-CCITT big-endian) carrying the EZSP version response: legacy
// (seq, fc, 0x00, version, stack type, stack version) or extended (seq, fc_lo, fc_hi = 0x01, frame id 0x0000, ...).
static bool ezsp_version_frame(pd_ver_t *v, const uint8_t *f, size_t n) {
    if (n < 3 + 7 || (f[0] & 0x80) != 0 || n - 3 > 32) return false;  // DATA frames only; the response is short
    if (crc_ccitt(f, n - 2) != (uint16_t)(f[n - 2] << 8 | f[n - 1])) return false;
    uint8_t d[32], seed = 0x42;
    size_t dn = n - 3;
    for (size_t i = 0; i < dn; i++) {
        d[i] = f[1 + i] ^ seed;
        seed = (seed & 1) ? (uint8_t)((seed >> 1) ^ 0xB8) : (uint8_t)(seed >> 1);
    }
    if ((d[1] & 0x80) == 0 || (d[1] & 0x18) != 0) return false;  // a response, not a callback
    const uint8_t *pv;
    if (dn >= 9 && (d[2] & 0x03) == 0x01 && d[3] == 0 && d[4] == 0) pv = d + 5;  // extended frame
    else if (d[2] == 0x00) pv = d + 3;                                            // legacy frame
    else return false;
    if (pv + 4 > d + dn || pv[0] < 4 || pv[0] > 40) return false;
    uint16_t stack = (uint16_t)(pv[2] | pv[3] << 8);
    bool news = !v->ezsp_seen || v->ezsp_version != pv[0] || v->ezsp_stack != stack;
    v->ezsp_seen = true;
    v->ezsp_version = pv[0];
    v->ezsp_stack = stack;
    return news;
}

void pd_znp_name(uint8_t product, uint8_t major, uint8_t minor, uint8_t maint, char *out, size_t n) {
    switch (product) {
        case 0: snprintf(out, n, "1.2"); break;
        case 1: snprintf(out, n, "3.x.0"); break;
        case 2: snprintf(out, n, "3.0.x"); break;
        default: snprintf(out, n, "%u.%u.%u", major, minor, maint); break;
    }
}

bool pd_ver_text(const pd_ver_t *v, char *run, size_t run_n, char *ver, size_t ver_n) {
    run[0] = ver[0] = 0;
    if (v->spinel_version[0]) {
        snprintf(run, run_n, "%s", v->spinel_version);
        // the first x.y.z ("SL-OPENTHREAD/2.6.1_GitHub-..." -> "2.6.1")
        const char *s = v->spinel_version;
        for (const char *p = s; *p; p++) {
            if (*p < '0' || *p > '9' || !(p == s || p[-1] == '/' || p[-1] == ' ' || p[-1] == 'v' || p[-1] == '-')) continue;
            size_t k = 0;
            int dots = 0;
            while ((p[k] >= '0' && p[k] <= '9') || (p[k] == '.' && p[k + 1] >= '0' && p[k + 1] <= '9')) {
                if (p[k] == '.') dots++;
                k++;
            }
            if (dots >= 1) {
                snprintf(ver, ver_n, "%.*s", (int)k, p);
                break;
            }
        }
        return true;
    }
    if (v->ezsp_seen) {
        uint16_t s = v->ezsp_stack;
        snprintf(ver, ver_n, "%u.%u.%u.%u", (s >> 12) & 15, (s >> 8) & 15, (s >> 4) & 15, s & 15);
        snprintf(run, run_n, "EmberZNet %u.%u.%u.%u (EZSP v%u)", (s >> 12) & 15, (s >> 8) & 15, (s >> 4) & 15, s & 15,
                 v->ezsp_version);
        return true;
    }
    if (v->znp_seen) {
        char name[16];
        pd_znp_name(v->znp_product, v->znp_major, v->znp_minor, v->znp_maint, name, sizeof(name));
        snprintf(run, run_n, "Z-Stack %s build %lu", name, (unsigned long)v->znp_revision);
        snprintf(ver, ver_n, "%lu", (unsigned long)v->znp_revision);
        return true;
    }
    return false;
}

bool pd_ver_feed(pd_ver_t *v, const uint8_t *data, size_t len) {
    bool learnt = false;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        // Spinel over HDLC-lite (X.25 FCS) and ASH (CRC-CCITT) share 7E ... 7E with escape 7D; neither carries a
        // raw 11/13 (XON/XOFF) or 18/1A (ASH substitute / cancel) as data.
        pd_frame_t *h = &v->hdlc;
        if (b == 0x11 || b == 0x13) {
            // flow control (both escape these when they are data)
        } else if ((b == 0x1A || b == 0x18) && !h->esc) {
            // ASH cancel / substitute; OpenThread's HDLC-lite sends these raw as data
            v->ash_bad = true;
            put(h, b);
        } else if (b == 0x7E) {
            const uint8_t *f = h->buf;
            size_t n = h->len;
            // header first: the CRC only runs for a PROP_VALUE_IS(NCP_VERSION) frame, not every ASH frame
            if (n >= 6 && !h->bad && (f[0] & 0xC0) == 0x80 && f[1] == 0x06 && f[2] == 0x02 && spinel_ok(f, n)) {
                char tmp[sizeof(v->spinel_version)];
                size_t k = 0;
                for (size_t j = 3; j < n - 2 && f[j] && k + 1 < sizeof(tmp); j++) tmp[k++] = (f[j] >= 0x20 && f[j] < 0x7F) ? (char)f[j] : '?';
                tmp[k] = 0;
                if (k && strcmp(tmp, v->spinel_version) != 0) {
                    memcpy(v->spinel_version, tmp, k + 1);
                    learnt = true;
                }
            } else if (n && !h->bad && !v->ash_bad && ezsp_version_frame(v, f, n)) {
                learnt = true;
            }
            v->ash_bad = false;
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

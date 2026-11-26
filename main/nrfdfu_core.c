// Nordic nRF52840 Dongle flasher: serial DFU (SLIP) to the Open USB bootloader. See nrfdfu_core.h.

#include "nrfdfu_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- CRC-32, SHA-256

uint32_t nd_crc32(uint32_t crc, const uint8_t *d, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *d++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(nd_sha256_t *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    s->h[0] += a, s->h[1] += b, s->h[2] += c, s->h[3] += d, s->h[4] += e, s->h[5] += f, s->h[6] += g, s->h[7] += h;
}

void nd_sha256_init(nd_sha256_t *s) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                   0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->used = 0;
}

void nd_sha256_update(nd_sha256_t *s, const uint8_t *d, size_t n) {
    s->len += n;
    while (n) {
        size_t k = 64 - s->used;
        if (k > n) k = n;
        memcpy(s->buf + s->used, d, k);
        s->used += (uint32_t)k, d += k, n -= k;
        if (s->used == 64) {
            sha_block(s, s->buf);
            s->used = 0;
        }
    }
}

void nd_sha256_final(nd_sha256_t *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    nd_sha256_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56) nd_sha256_update(s, &pad, 1);
    uint8_t l[8];
    for (int i = 0; i < 8; i++) l[i] = (uint8_t)(bits >> (56 - 8 * i));
    nd_sha256_update(s, l, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24), out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8), out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

// ---------------------------------------------------------------- the init packet (protobuf, dfu-cc.proto)

typedef struct {
    const uint8_t *p, *end;
    const char *bad;
} pbr_t;

static uint64_t pb_varint(pbr_t *r) {
    uint64_t v = 0;
    for (int s = 0; s < 64; s += 7) {
        if (r->p >= r->end) break;
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7F) << s;
        if (!(b & 0x80)) return v;
    }
    if (!r->bad) r->bad = "init packet: broken varint";
    return 0;
}

// Reads one field header. False at the end (or when broken: r->bad).
static bool pb_field(pbr_t *r, uint32_t *field, uint32_t *wt) {
    if (r->bad || r->p >= r->end) return false;
    uint64_t key = pb_varint(r);
    *field = (uint32_t)(key >> 3);
    *wt = (uint32_t)(key & 7);
    return !r->bad;
}

static uint32_t pb_u32(pbr_t *r, uint32_t wt) {
    if (wt != 0) {
        if (!r->bad) r->bad = "init packet: a number field of the wrong wire type";
        return 0;
    }
    uint64_t v = pb_varint(r);
    if (v > 0xFFFFFFFFu && !r->bad) r->bad = "init packet: a 32-bit field too large";
    return (uint32_t)v;
}

// A length-delimited field: its bytes as a sub-reader.
static pbr_t pb_sub(pbr_t *r, uint32_t wt) {
    pbr_t s = {r->p, r->p, NULL};
    if (wt != 2) {
        if (!r->bad) r->bad = "init packet: a message field of the wrong wire type";
        return s;
    }
    uint64_t n = pb_varint(r);
    if (r->bad) return s;
    if (n > (uint64_t)(r->end - r->p)) {
        r->bad = "init packet: cut short";
        return s;
    }
    s.p = r->p;
    s.end = r->p + n;
    r->p += n;
    return s;
}

static void pb_skip(pbr_t *r, uint32_t wt) {
    switch (wt) {
        case 0: pb_varint(r); break;
        case 1:
        case 5: {
            size_t n = wt == 1 ? 8 : 4;
            if ((size_t)(r->end - r->p) < n) r->bad = "init packet: cut short";
            else r->p += n;
        } break;
        case 2: pb_sub(r, wt); break;
        default:
            if (!r->bad) r->bad = "init packet: unknown wire type (not a protobuf .dat?)";
    }
}

static void parse_hash(pbr_t *r, nd_init_t *o) {
    uint32_t f, wt;
    bool has_type = false, has_bytes = false;
    while (pb_field(r, &f, &wt)) {
        if (f == 1) {
            o->hash_type = pb_u32(r, wt);
            has_type = true;
        } else if (f == 2) {
            pbr_t s = pb_sub(r, wt);
            if (r->bad) return;
            if (s.end - s.p != 32) {
                r->bad = "init packet: hash is not 32 bytes";
                return;
            }
            memcpy(o->hash, s.p, 32);
            has_bytes = true;
        } else {
            pb_skip(r, wt);
        }
    }
    if (!r->bad && !(has_type && has_bytes)) r->bad = "init packet: hash without its type or bytes";
    o->has_hash = !r->bad;
}

static void parse_init(pbr_t *r, nd_init_t *o) {
    uint32_t f, wt, nbv = 0;
    while (pb_field(r, &f, &wt)) {
        switch (f) {
            case 1: o->fw_version = pb_u32(r, wt), o->has_fw_version = true; break;
            case 2: o->hw_version = pb_u32(r, wt), o->has_hw_version = true; break;
            case 3:  // repeated uint32, packed (nrfutil) or not: nanopb takes both
                if (wt == 2) {
                    pbr_t s = pb_sub(r, wt);
                    while (!r->bad && !s.bad && s.p < s.end) {
                        uint32_t v = pb_u32(&s, 0);
                        if (o->sd_req_count >= 16) s.bad = "init packet: more than 16 sd_req";
                        else o->sd_req[o->sd_req_count++] = v;
                    }
                    if (s.bad && !r->bad) r->bad = s.bad;
                } else {
                    uint32_t v = pb_u32(r, wt);
                    if (o->sd_req_count >= 16) r->bad = "init packet: more than 16 sd_req";
                    else o->sd_req[o->sd_req_count++] = v;
                }
                break;
            case 4: o->type = pb_u32(r, wt), o->has_type = true; break;
            case 5: o->sd_size = pb_u32(r, wt); break;
            case 6: o->bl_size = pb_u32(r, wt); break;
            case 7: o->app_size = pb_u32(r, wt), o->has_app_size = true; break;
            case 8: {
                pbr_t s = pb_sub(r, wt);
                if (r->bad) return;
                parse_hash(&s, o);
                if (s.bad) r->bad = s.bad;
            } break;
            case 9: o->is_debug = pb_u32(r, wt) != 0, o->has_is_debug = true; break;
            case 10: {  // boot_validation: the bootloader works it out itself (CRC when absent)
                pbr_t s = pb_sub(r, wt);
                if (++nbv > 3 && !r->bad) r->bad = "init packet: more than 3 boot validations";
                uint32_t f2, w2;
                while (!r->bad && pb_field(&s, &f2, &w2)) {
                    if (f2 == 2) {
                        pbr_t b = pb_sub(&s, w2);
                        if (!s.bad && b.end - b.p > 64) s.bad = "init packet: boot validation over 64 bytes";
                    } else {
                        pb_skip(&s, w2);
                    }
                }
                if (s.bad && !r->bad) r->bad = s.bad;
            } break;
            default: pb_skip(r, wt);
        }
    }
}

// Command { op_code = 1, init = 2, reset = 3 }
static bool parse_command(pbr_t *r, nd_init_t *o) {
    uint32_t f, wt;
    bool has_init = false;
    while (pb_field(r, &f, &wt)) {
        if (f == 1) {
            if (pb_u32(r, wt) != 1 && !r->bad) r->bad = "init packet: not an INIT command";
        } else if (f == 2) {
            pbr_t s = pb_sub(r, wt);
            if (r->bad) break;
            if (has_init) {
                r->bad = "init packet: two init commands";
                break;
            }
            parse_init(&s, o);
            if (s.bad) r->bad = s.bad;
            has_init = true;
        } else {
            pb_skip(r, wt);
        }
    }
    return has_init;
}

const char *nd_init_parse(const uint8_t *dat, size_t len, nd_init_t *o) {
    memset(o, 0, sizeof(*o));
    if (!dat || len == 0) return "init packet: empty";
    if (len > ND_INIT_MAX) return "init packet: over 512 bytes (not a .dat?)";
    pbr_t r = {dat, dat + len, NULL};
    uint32_t f, wt;
    bool cmd = false, signed_cmd = false, init = false;
    while (pb_field(&r, &f, &wt)) {
        if (f == 1 || f == 2) {
            pbr_t s = pb_sub(&r, wt);
            if (r.bad) break;
            if (f == 1) {
                if (cmd) return "init packet: two commands";
                cmd = true;
                init = parse_command(&s, o);
            } else {  // SignedCommand { command = 1, signature_type = 2, signature = 3 }
                if (signed_cmd) return "init packet: two signed commands";
                signed_cmd = true;
                uint32_t f2, w2;
                bool has_sig = false, has_st = false;
                while (pb_field(&s, &f2, &w2)) {
                    if (f2 == 1) {
                        pbr_t c = pb_sub(&s, w2);
                        if (s.bad) break;
                        init = parse_command(&c, o);
                        if (c.bad) s.bad = c.bad;
                    } else if (f2 == 2) {
                        pb_u32(&s, w2);
                        has_st = true;
                    } else if (f2 == 3) {
                        pbr_t g = pb_sub(&s, w2);
                        if (!s.bad && g.end - g.p > 64) s.bad = "init packet: signature over 64 bytes";
                        has_sig = true;
                    } else {
                        pb_skip(&s, w2);
                    }
                }
                if (!s.bad && !(has_sig && has_st)) s.bad = "init packet: signed command without its signature";
            }
            if (s.bad) return s.bad;
        } else {
            pb_skip(&r, wt);
        }
    }
    if (r.bad) return r.bad;
    if (cmd && signed_cmd) return "init packet: both a command and a signed command";  // the bootloader refuses this
    if (!init) return "init packet: no init command inside (not a .dat?)";
    o->is_signed = signed_cmd;
    return NULL;
}

const char *nd_init_policy(const nd_init_t *in) {
    static const char *const types[] = {"application", "SoftDevice", "bootloader", "SoftDevice + bootloader",
                                        "external application"};
    if (!in->has_type) return "init packet has no firmware type";
    if (in->type != 0) {
        // Bootloader/SoftDevice images rewrite what keeps the dongle recoverable: never sent from here.
        static char why[96];
        snprintf(why, sizeof(why), "init packet is for a %s update: only applications are flashed here",
                 in->type < 5 ? types[in->type] : "unknown");
        return why;
    }
    if (!in->has_hash || in->hash_type != 3) return "init packet hash is not SHA-256 (the bootloader only checks SHA-256)";
    if (!in->has_hw_version) return "init packet has no hw_version (the bootloader requires one)";
    if (in->hw_version != ND_HW_VERSION) {
        static char why[120];
        snprintf(why, sizeof(why), "init packet is for hw_version %u; the dongle's bootloader takes 52 (nrfutil --hw-version 52)",
                 (unsigned)in->hw_version);
        return why;
    }
    if (!in->has_app_size || in->app_size == 0) return "init packet has no application size";
    if (in->app_size > ND_APP_LIMIT - ND_APP_START) return "application is bigger than the dongle's flash";
    return NULL;
}

static size_t put_varint(uint8_t *o, uint32_t v) {
    size_t n = 0;
    do {
        o[n] = (uint8_t)(v & 0x7F);
        v >>= 7;
        if (v) o[n] |= 0x80;
        n++;
    } while (v);
    return n;
}

size_t nd_init_build(uint32_t app_size, const uint8_t sha256[32], uint32_t fw_version, uint32_t hw_version,
                     uint8_t *out, size_t max) {
    // Field order and presence as Google protobuf serializes pc-nrfutil's InitPacketPB (proto2: every
    // field it sets is written, defaults included).
    uint8_t init[96], cmd[112];
    size_t n = 0;
    init[n++] = 0x08, n += put_varint(init + n, fw_version);
    init[n++] = 0x10, n += put_varint(init + n, hw_version);
    init[n++] = 0x1A, init[n++] = 0x01, init[n++] = 0x00;  // sd_req [0x00]: no SoftDevice needed
    init[n++] = 0x20, init[n++] = 0x00;                    // type APPLICATION
    init[n++] = 0x28, init[n++] = 0x00;                    // sd_size
    init[n++] = 0x30, init[n++] = 0x00;                    // bl_size
    init[n++] = 0x38, n += put_varint(init + n, app_size);
    init[n++] = 0x42, init[n++] = 0x24, init[n++] = 0x08, init[n++] = 0x03, init[n++] = 0x12, init[n++] = 0x20;
    for (int i = 0; i < 32; i++) init[n++] = sha256[31 - i];  // little-endian, as nrfutil stores it
    init[n++] = 0x48, init[n++] = 0x00;                        // is_debug false
    init[n++] = 0x52, init[n++] = 0x04, init[n++] = 0x08, init[n++] = 0x01, init[n++] = 0x12, init[n++] = 0x00;  // CRC
    size_t c = 0;
    cmd[c++] = 0x08, cmd[c++] = 0x01;  // op_code INIT
    cmd[c++] = 0x12, c += put_varint(cmd + c, (uint32_t)n);
    memcpy(cmd + c, init, n);
    c += n;
    uint8_t hdr[6];
    size_t h = 0;
    hdr[h++] = 0x0A, h += put_varint(hdr + h, (uint32_t)c);
    if (h + c > max) return 0;
    memcpy(out, hdr, h);
    memcpy(out + h, cmd, c);
    return h + c;
}

// ---------------------------------------------------------------- the image

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

const char *nd_image_sha256(const nd_io_t *io, uint32_t len, uint8_t out[32]) {
    nd_sha256_t s;
    uint8_t buf[512];
    nd_sha256_init(&s);
    for (uint32_t off = 0; off < len;) {
        uint32_t k = len - off < sizeof(buf) ? len - off : (uint32_t)sizeof(buf);
        if (io->image_read(io->ctx, off, buf, k) != 0) return "couldn't read the firmware file";
        nd_sha256_update(&s, buf, k);
        off += k;
    }
    nd_sha256_final(&s, out);
    return NULL;
}

const char *nd_image_check(const nd_io_t *io, uint32_t bin_len, const nd_init_t *in) {
    static char why[160];
    if (bin_len < 8) return "firmware file is too small";
    if (in->app_size != bin_len) {
        snprintf(why, sizeof(why), "firmware is %u bytes but its init packet says %u (files from different packages?)",
                 (unsigned)bin_len, (unsigned)in->app_size);
        return why;
    }
    uint8_t d[32];
    const char *e = nd_image_sha256(io, bin_len, d);
    if (e) return e;
    for (int i = 0; i < 32; i++)
        if (in->hash[i] != d[31 - i]) return "firmware doesn't match the SHA-256 in its init packet (files from different packages?)";
    uint8_t v[8];
    if (io->image_read(io->ctx, 0, v, 8) != 0) return "couldn't read the firmware file";
    uint32_t sp = le32(v), reset = le32(v + 4);
    if (sp < 0x20000000u || sp > 0x20040000u || (sp & 3))
        return "firmware doesn't start with a Cortex-M vector table (initial stack not in nRF52840 RAM)";
    // An app that needs a SoftDevice sits after it, at an address only the SoftDevice knows.
    bool needs_sd = in->sd_req_count > 0 && in->sd_req[0] != 0;
    if (!(reset & 1) || (!needs_sd && ((reset & ~1u) < ND_APP_START + 8 || (reset & ~1u) >= ND_APP_START + bin_len))) {
        snprintf(why, sizeof(why), "firmware isn't linked for the dongle's bootloader (reset handler 0x%08x, expected inside 0x%x..0x%x)",
                 (unsigned)reset, ND_APP_START, (unsigned)(ND_APP_START + bin_len));
        return why;
    }
    return NULL;
}

// ---------------------------------------------------------------- Intel HEX

typedef struct {
    const nd_io_t *io;
    uint32_t size, off, pos, fill;
    uint8_t buf[256];
} hexrd_t;

static int hex_getc(hexrd_t *h) {
    if (h->pos == h->fill) {
        if (h->off >= h->size) return -1;
        uint32_t k = h->size - h->off < sizeof(h->buf) ? h->size - h->off : (uint32_t)sizeof(h->buf);
        if (h->io->image_read(h->io->ctx, h->off, h->buf, k) != 0) return -2;
        h->off += k, h->pos = 0, h->fill = k;
    }
    return h->buf[h->pos++];
}

static int nib(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// One record into rec (count, addr hi, addr lo, type, data..., checksum). Returns its length, 0 at the
// end of the file, or <0 with *why set.
static int hex_record(hexrd_t *h, uint8_t *rec, const char **why) {
    int c;
    do c = hex_getc(h);
    while (c == '\r' || c == '\n' || c == ' ' || c == '\t');
    if (c == -1) return 0;
    if (c == -2) return *why = "couldn't read the firmware file", -1;
    if (c != ':') return *why = "not an Intel HEX file", -1;
    int n = 0;
    for (;;) {
        int a = hex_getc(h);
        if (a < 0 || a == '\r' || a == '\n') break;
        int b = hex_getc(h);
        if (nib(a) < 0 || nib(b) < 0) return *why = "Intel HEX: bad character", -1;
        if (n >= 5 + 255) return *why = "Intel HEX: record too long", -1;
        rec[n++] = (uint8_t)(nib(a) << 4 | nib(b));
    }
    if (n < 5 || n != 5 + rec[0]) return *why = "Intel HEX: record length doesn't match its count", -1;
    uint8_t sum = 0;
    for (int i = 0; i < n; i++) sum = (uint8_t)(sum + rec[i]);
    if (sum) return *why = "Intel HEX: checksum wrong", -1;
    return n;
}

// pass 0: find the extent; pass 1: copy into out (base = lowest address).
static const char *hex_pass(const nd_io_t *io, uint32_t size, int pass, uint32_t *lo, uint32_t *hi, uint32_t *dropped,
                            uint8_t *out, uint32_t base) {
    hexrd_t h = {.io = io, .size = size};
    uint8_t rec[5 + 255];
    uint32_t upper = 0;
    const char *why = NULL;
    for (;;) {
        int n = hex_record(&h, rec, &why);
        if (n < 0) return why;
        if (n == 0) return "Intel HEX: cut short (no end record)";
        uint8_t cnt = rec[0], type = rec[3];
        uint32_t addr = upper + ((uint32_t)rec[1] << 8 | rec[2]);
        switch (type) {
            case 0x00:
                if (cnt == 0) break;
                if (addr >= 0x10000000u) {  // UICR/FICR: nrfutil drops it too
                    if (pass == 0) *dropped += cnt;
                    break;
                }
                if (addr < ND_APP_START) return "Intel HEX has data in the MBR area (a full-chip or bare build, not one for the USB bootloader)";
                if (addr + cnt > ND_APP_LIMIT) return "Intel HEX reaches the bootloader area (0xE0000 and up): refusing";
                if (pass == 0) {
                    if (addr < *lo) *lo = addr;
                    if (addr + cnt > *hi) *hi = addr + cnt;
                } else {
                    memcpy(out + (addr - base), rec + 4, cnt);
                }
                break;
            case 0x01: return NULL;
            case 0x02:
                if (cnt != 2) return "Intel HEX: bad segment record";
                upper = ((uint32_t)rec[4] << 8 | rec[5]) << 4;
                break;
            case 0x04:
                if (cnt != 2) return "Intel HEX: bad address record";
                upper = ((uint32_t)rec[4] << 8 | rec[5]) << 16;
                break;
            case 0x03:
            case 0x05: break;  // start address: not part of the image
            default: return "Intel HEX: unknown record type";
        }
    }
}

const char *nd_hex_to_bin(const nd_io_t *io, uint32_t hex_size, uint8_t *out, uint32_t out_max, uint32_t *bin_len,
                          uint32_t *start, uint32_t *dropped) {
    uint32_t lo = 0xFFFFFFFFu, hi = 0, drop = 0;
    *bin_len = 0, *start = 0, *dropped = 0;
    const char *e = hex_pass(io, hex_size, 0, &lo, &hi, &drop, NULL, 0);
    if (e) return e;
    if (hi == 0) return "Intel HEX has no firmware data";
    uint32_t len = (hi - lo + 3) & ~3u;  // nrfutil rounds the size up to whole words
    if (len > out_max) return "firmware is too big for the buffer";
    memset(out, 0xFF, len);
    e = hex_pass(io, hex_size, 1, &lo, &hi, &drop, out, lo);
    if (e) return e;
    *bin_len = len, *start = lo, *dropped = drop;
    return NULL;
}

// ---------------------------------------------------------------- the serial DFU transport

#define SLIP_END 0xC0
#define SLIP_ESC 0xDB
#define SLIP_ESC_END 0xDC
#define SLIP_ESC_ESC 0xDD

#define OP_CREATE 0x01
#define OP_PRN 0x02
#define OP_CRC 0x03
#define OP_EXECUTE 0x04
#define OP_SELECT 0x06
#define OP_MTU 0x07
#define OP_WRITE 0x08
#define OP_PING 0x09
#define OP_RESPONSE 0x60
#define RES_SUCCESS 0x01
#define RES_EXT 0x0B
#define OBJ_COMMAND 0x01
#define OBJ_DATA 0x02

typedef struct {
    const nd_io_t *io;
    nd_result_t *r;
    char *err;
    size_t errlen;
    uint8_t tx[2 * (1 + ND_CHUNK_MAX) + 1];
    uint8_t frame[64];
    size_t flen;
    bool esc, junk;
    uint8_t rx[64];
    size_t rxpos, rxlen;
    uint8_t ping_id;
} st_t;

static void logf_(st_t *s, const char *fmt, ...) {
    if (!s->io->log) return;
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    s->io->log(s->io->ctx, b);
}

static bool fail(st_t *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->err, s->errlen, fmt, ap);
    va_end(ap);
    return false;
}

static const char *op_name(uint8_t op) {
    switch (op) {
        case OP_CREATE: return "create";
        case OP_PRN: return "set PRN";
        case OP_CRC: return "CRC";
        case OP_EXECUTE: return "execute";
        case OP_SELECT: return "select";
        case OP_MTU: return "get MTU";
        case OP_PING: return "ping";
        default: return "request";
    }
}

static const char *res_name(uint8_t r) {
    switch (r) {
        case 0x00: return "invalid opcode";
        case 0x02: return "opcode not supported";
        case 0x03: return "invalid parameter";
        case 0x04: return "insufficient resources";
        case 0x05: return "invalid object";
        case 0x07: return "unsupported object type";
        case 0x08: return "operation not permitted";
        case 0x0A: return "operation failed";
        default: return "unknown result";
    }
}

const char *nd_ext_error_str(uint8_t c) {
    static const char *const t[] = {
        "no extended error",
        "invalid error code",
        "wrong command format",
        "unknown command",
        "init command invalid",
        "firmware version too low",
        "hardware version doesn't match this bootloader",
        "needs a SoftDevice this dongle hasn't got (sd_req)",
        "this bootloader wants signed updates (not the open bootloader)",
        "hash type not supported",
        "hash couldn't be calculated",
        "signature type not supported",
        "firmware doesn't match the hash in the init packet",
        "not enough space on the dongle",
    };
    return c < sizeof(t) / sizeof(t[0]) ? t[c] : "unknown extended error";
}

// Sends one SLIP frame (END only at the end: a leading END would reach the bootloader as an empty
// packet, which its serial layer doesn't guard against).
static bool send(st_t *s, const uint8_t *p, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == SLIP_END) s->tx[o++] = SLIP_ESC, s->tx[o++] = SLIP_ESC_END;
        else if (p[i] == SLIP_ESC) s->tx[o++] = SLIP_ESC, s->tx[o++] = SLIP_ESC_ESC;
        else s->tx[o++] = p[i];
    }
    s->tx[o++] = SLIP_END;
    if (s->io->write(s->io->ctx, s->tx, o) != 0) return fail(s, "the dongle went away (USB write failed)");
    return true;
}

// Next whole frame into s->frame. 1 = got one, 0 = timeout, -1 = gone.
static int recv(st_t *s, uint32_t timeout_ms) {
    uint32_t t0 = s->io->now_ms(s->io->ctx);
    for (;;) {
        while (s->rxpos < s->rxlen) {
            uint8_t c = s->rx[s->rxpos++];
            if (c == SLIP_END) {
                bool ok = !s->junk && s->flen > 0;
                s->junk = s->esc = false;
                if (ok) return 1;
                s->flen = 0;
                continue;
            }
            if (s->junk) continue;
            if (s->esc) {
                s->esc = false;
                if (c == SLIP_ESC_END) c = SLIP_END;
                else if (c == SLIP_ESC_ESC) c = SLIP_ESC;
                else {
                    s->junk = true;
                    continue;
                }
            } else if (c == SLIP_ESC) {
                s->esc = true;
                continue;
            }
            if (s->flen >= sizeof(s->frame)) s->junk = true;
            else s->frame[s->flen++] = c;
        }
        uint32_t el = s->io->now_ms(s->io->ctx) - t0;
        if (el >= timeout_ms) return 0;
        int n = s->io->read(s->io->ctx, s->rx, sizeof(s->rx), timeout_ms - el);
        if (n < 0) return -1;
        s->rxpos = 0;
        s->rxlen = (size_t)n;
    }
}

static void frame_done(st_t *s) { s->flen = 0; }

// One request and its response. The payload after [0x60 op result] lands in out (up to max); returns
// its length, or -1 with err set.
static int request(st_t *s, const uint8_t *req, size_t n, uint8_t *out, size_t max, uint32_t timeout_ms) {
    uint8_t op = req[0];
    if (!send(s, req, n)) return -1;
    uint32_t t0 = s->io->now_ms(s->io->ctx);
    for (;;) {
        uint32_t el = s->io->now_ms(s->io->ctx) - t0;
        int g = el >= timeout_ms ? 0 : recv(s, timeout_ms - el);
        if (g < 0) return fail(s, "the dongle went away (during %s)", op_name(op)), -1;
        if (g == 0) return fail(s, "the bootloader didn't answer %s", op_name(op)), -1;
        if (s->flen < 3 || s->frame[0] != OP_RESPONSE || s->frame[1] != op) {
            logf_(s, "ignored a stray %u-byte frame (%02x %02x)", (unsigned)s->flen, s->frame[0], s->flen > 1 ? s->frame[1] : 0);
            frame_done(s);
            continue;
        }
        uint8_t res = s->frame[2];
        s->r->result = res;
        if (res == RES_EXT) {
            s->r->ext_error = s->flen > 3 ? s->frame[3] : 0;
            frame_done(s);
            return fail(s, "%s refused: %s (extended error 0x%02x)", op_name(op), nd_ext_error_str(s->r->ext_error),
                        s->r->ext_error), -1;
        }
        if (res != RES_SUCCESS) {
            frame_done(s);
            return fail(s, "%s refused: %s (0x%02x)", op_name(op), res_name(res), res), -1;
        }
        size_t k = s->flen - 3;
        if (k > max) k = max;
        if (out) memcpy(out, s->frame + 3, k);
        frame_done(s);
        return (int)k;
    }
}

// Ping, quietly: true when a bootloader answered (any result counts, as in pc-nrfutil).
static bool ping(st_t *s, int tries, uint32_t each_ms) {
    for (int i = 0; i < tries; i++) {
        uint8_t id = ++s->ping_id;
        uint8_t req[2] = {OP_PING, id};
        if (!send(s, req, 2)) return false;
        uint32_t t0 = s->io->now_ms(s->io->ctx);
        for (;;) {
            uint32_t el = s->io->now_ms(s->io->ctx) - t0;
            int g = el >= each_ms ? 0 : recv(s, each_ms - el);
            if (g <= 0) break;
            bool ok = s->flen >= 3 && s->frame[0] == OP_RESPONSE && s->frame[1] == OP_PING &&
                      (s->frame[2] != RES_SUCCESS || (s->flen >= 4 && s->frame[3] == id));
            frame_done(s);
            if (ok) return true;
        }
    }
    return false;
}

static void drain(st_t *s) {
    uint8_t b[64];
    for (int i = 0; i < 64 && s->io->read(s->io->ctx, b, sizeof(b), 0) > 0; i++) {
    }
    s->rxpos = s->rxlen = 0;
    s->flen = 0;
    s->esc = s->junk = false;
}

// One object: create, write in chunks, check the CRC, execute. 1 = done, 0 = CRC/offset mismatch (send
// it again), -1 = failed (err set). base/crc_in: where the object starts and the running CRC before it.
static int send_object(st_t *s, uint8_t type, const uint8_t *mem, uint32_t base, uint32_t len, uint32_t crc_in,
                       uint32_t *crc_out, bool last) {
    uint8_t req[1 + ND_CHUNK_MAX], rsp[12];
    req[0] = OP_CREATE, req[1] = type;
    req[2] = (uint8_t)len, req[3] = (uint8_t)(len >> 8), req[4] = (uint8_t)(len >> 16), req[5] = (uint8_t)(len >> 24);
    if (request(s, req, 6, NULL, 0, 5000) < 0) return -1;  // erases the page(s) first: allow for it
    uint32_t crc = crc_in;
    for (uint32_t off = 0; off < len;) {
        uint32_t k = len - off < s->r->chunk ? len - off : s->r->chunk;
        req[0] = OP_WRITE;
        if (mem) memcpy(req + 1, mem + off, k);
        else if (s->io->image_read(s->io->ctx, base + off, req + 1, k) != 0) return fail(s, "couldn't read the firmware file"), -1;
        if (!send(s, req, 1 + k)) return -1;  // PRN 0: no answer to writes
        crc = nd_crc32(crc, req + 1, k);
        off += k;
    }
    req[0] = OP_CRC;
    int n = request(s, req, 1, rsp, sizeof(rsp), 3000);
    if (n < 0) return -1;
    if (n < 8) return fail(s, "short CRC answer"), -1;
    uint32_t got_off = le32(rsp), got_crc = le32(rsp + 4);
    if (got_off != base + len || got_crc != crc) {
        logf_(s, "%s object at %u: bootloader has offset %u CRC %08x, sent %u CRC %08x", type == OBJ_COMMAND ? "init" : "data",
              (unsigned)base, (unsigned)got_off, (unsigned)got_crc, (unsigned)(base + len), (unsigned)crc);
        return 0;
    }
    req[0] = OP_EXECUTE;
    // The last execute hashes the whole image and writes the settings page before answering.
    if (request(s, req, 1, NULL, 0, last ? 20000 : 5000) < 0) return -1;
    *crc_out = crc;
    return 1;
}

#define TRIES 3

bool nd_flash(const nd_io_t *io, const nd_flash_opts_t *o, nd_result_t *r, char *err, size_t errlen) {
    static st_t st;  // ~2.2 KB of buffers: kept off the caller's stack (one flash at a time)
    st_t *s = &st;
    memset(s, 0, sizeof(*s));
    memset(r, 0, sizeof(*r));
    s->io = io, s->r = r, s->err = err, s->errlen = errlen;
    if (errlen) err[0] = 0;

    // ---- everything checked before the dongle is touched
    nd_init_t in;
    const char *why = nd_init_parse(o->init, o->init_len, &in);
    if (!why) why = nd_init_policy(&in);
    if (!why) why = nd_image_check(io, o->bin_len, &in);
    if (why) return fail(s, "%s", why);
    if (in.sd_req_count && in.sd_req[0] != 0)
        logf_(s, "init packet needs SoftDevice 0x%04x: the bootloader checks that before erasing anything", (unsigned)in.sd_req[0]);
    if (in.is_signed) logf_(s, "init packet is signed (the open bootloader doesn't check application signatures)");

    // ---- the bootloader
    if (io->set_lines && io->set_lines(io->ctx, true, true) != 0) return fail(s, "the dongle went away (setting DTR)");
    drain(s);
    if (ping(s, 3, 300)) {
        r->entered = "already in the bootloader";
    } else {
        if (s->err[0]) return false;
        int rc = io->enter_dfu ? io->enter_dfu(io->ctx) : -1;
        if (rc != 0)
            return fail(s, "the dongle isn't in its DFU bootloader: press its RESET button (the sideways one, "
                           "pushed towards the USB plug; the red LED fades) and try again");
        logf_(s, "DFU trigger sent, looking for the bootloader");
        if (io->set_lines && io->set_lines(io->ctx, true, true) != 0) return fail(s, "the dongle went away (setting DTR)");
        drain(s);
        if (!ping(s, 20, 250)) {
            if (s->err[0]) return false;
            return fail(s, "the dongle didn't come up in its DFU bootloader after the trigger: press its RESET button and try again");
        }
        r->entered = "DFU trigger";
    }
    logf_(s, "bootloader answers (%s)", r->entered);

    uint8_t req[8], rsp[12];
    req[0] = OP_PRN, req[1] = 0, req[2] = 0;
    if (request(s, req, 3, NULL, 0, 1000) < 0) return false;
    req[0] = OP_MTU;
    int n = request(s, req, 1, rsp, sizeof(rsp), 1000);
    if (n < 0) return false;
    if (n < 2) return fail(s, "short MTU answer");
    r->mtu = (uint16_t)(rsp[0] | rsp[1] << 8);
    uint32_t chunk = r->mtu >= 5 ? (uint32_t)(r->mtu - 1) / 2 - 1 : 0;  // worst case SLIP doubling, as pc-nrfutil
    if (chunk > ND_CHUNK_MAX) chunk = ND_CHUNK_MAX;
    if (chunk < 1) return fail(s, "bootloader MTU %u is too small", r->mtu);
    r->chunk = chunk;

    // ---- the init packet
    req[0] = OP_SELECT, req[1] = OBJ_COMMAND;
    n = request(s, req, 2, rsp, sizeof(rsp), 1000);
    if (n < 0) return false;
    if (n < 12) return fail(s, "short select answer");
    if (o->init_len > le32(rsp))
        return fail(s, "init packet is %u bytes; this bootloader takes at most %u", (unsigned)o->init_len, (unsigned)le32(rsp));
    uint32_t crc = 0;
    int got = 0;
    for (int t = 0; t < TRIES && got == 0; t++) {
        if (t) r->retries++;
        got = send_object(s, OBJ_COMMAND, o->init, 0, (uint32_t)o->init_len, 0, &crc, false);
    }
    if (got < 0) {
        if (r->ext_error) {
            char e2[160];
            snprintf(e2, sizeof(e2), "%s", err);
            return fail(s, "the dongle's bootloader refused the init packet: %s (nothing was erased)", e2);
        }
        return false;
    }
    if (got == 0) return fail(s, "the init packet kept arriving damaged (nothing was erased)");
    r->app_erased = true;  // single-bank update: the bootloader has now let go of the old firmware
    logf_(s, "init packet accepted");

    // ---- the firmware
    req[0] = OP_SELECT, req[1] = OBJ_DATA;
    n = request(s, req, 2, rsp, sizeof(rsp), 1000);
    if (n < 0) return false;
    if (n < 12) return fail(s, "short select answer");
    r->obj_max = le32(rsp);
    if (r->obj_max == 0 || r->obj_max > 65536) return fail(s, "bootloader object size %u makes no sense", (unsigned)r->obj_max);
    if (le32(rsp + 4) != 0) return fail(s, "bootloader kept %u bytes from an earlier upload", (unsigned)le32(rsp + 4));
    crc = 0;
    for (uint32_t off = 0; off < o->bin_len;) {
        uint32_t k = o->bin_len - off < r->obj_max ? o->bin_len - off : r->obj_max;
        bool last = off + k == o->bin_len;
        got = 0;
        uint32_t next = crc;
        for (int t = 0; t < TRIES && got == 0; t++) {
            if (t) r->retries++;
            got = send_object(s, OBJ_DATA, NULL, off, k, crc, &next, last);
        }
        if (got < 0) {
            char e2[160];
            snprintf(e2, sizeof(e2), "%s", err);
            return fail(s, "%s at %u%%; the dongle waits in its bootloader for another try", e2,
                        (unsigned)((uint64_t)off * 100 / o->bin_len));
        }
        if (got == 0)
            return fail(s, "firmware kept arriving damaged at %u%%; the dongle waits in its bootloader for another try",
                        (unsigned)((uint64_t)off * 100 / o->bin_len));
        crc = next;
        off += k;
        if (io->progress) io->progress(io->ctx, "upload", off, o->bin_len);
    }
    r->app_erased = false;
    logf_(s, "image accepted: the bootloader is starting it");
    return true;
}

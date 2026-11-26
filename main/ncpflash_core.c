#include "ncpflash_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- small helpers

static void logf_(const nf_io_t *io, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(const nf_io_t *io, const char *fmt, ...) {
    if (io->log == NULL) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    io->log(io->ctx, b);
}

static void progress(const nf_io_t *io, const char *step, uint32_t done, uint32_t total) {
    if (io->progress) io->progress(io->ctx, step, done, total);
}

static void seterr(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void seterr(char *err, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool elapsed(const nf_io_t *io, uint32_t start, uint32_t ms) { return io->now_ms(io->ctx) - start >= ms; }

static uint32_t left(const nf_io_t *io, uint32_t start, uint32_t ms) {
    uint32_t e = io->now_ms(io->ctx) - start;
    return e >= ms ? 0 : ms - e;
}

// Throws away whatever the dongle already sent.
static void drain(const nf_io_t *io) {
    uint8_t b[64];
    for (int i = 0; i < 64 && io->read(io->ctx, b, sizeof(b), 20) > 0; i++) {
    }
}

uint32_t nf_crc32(uint32_t crc, const uint8_t *d, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

static uint16_t crc16_xmodem(const uint8_t *d, size_t n) {  // CRC-16/XMODEM (XMODEM-CRC blocks)
    uint16_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static uint16_t crc16_ash(const uint8_t *d, size_t n) {  // CRC-CCITT, init 0xFFFF (ASH frames)
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

// ---------------------------------------------------------------- GBL v3

#define GBL_HEADER 0x03A617EBu
#define GBL4_ROOT 0x84A617EBu
#define GBL_APP_INFO 0xF40A0AF4u
#define GBL_SE_UPGRADE 0x5EA617EBu
#define GBL_BOOTLOADER 0xF50909F5u
#define GBL_PROG 0xFE0101FEu
#define GBL_ERASEPROG 0xFD0303FDu
#define GBL_PROG_LZ4 0xFD0505FDu
#define GBL_PROG_LZMA 0xFD0707FDu
#define GBL_DELTA 0xF80A0AF8u
#define GBL_DELTA_LZ4 0xF80B0BF8u
#define GBL_DELTA_LZMA 0xF80C0CF8u
#define GBL_VERSION_DEP 0x76A617EBu
#define GBL_METADATA 0xF60808F6u
#define GBL_SIGNATURE 0xF70A0AF7u
#define GBL_CERT 0xF30B0BF3u
#define GBL_ENC_HEADER 0xFB0505FBu
#define GBL_ENC_INIT 0xFA0606FAu
#define GBL_ENC_DATA 0xF90707F9u
#define GBL_END 0xFC0404FCu
#define GBL_CRC_RESIDUE 0x2144DF1Cu  // zlib CRC-32 over the tags with the end tag's CRC included

const char *nf_gbl_check(const nf_io_t *io, uint32_t size, nf_gbl_t *g) {
    memset(g, 0, sizeof(*g));
    uint8_t buf[256];
    uint32_t crc = 0, off = 0;
    bool first = true, ended = false;
    while (!ended) {
        if (size - off < 8) return off == 0 ? "The file is too short to be a GBL image" : "The image is cut short (no end tag)";
        uint8_t th[8];
        if (io->image_read(io->ctx, off, th, 8) != 0) return "Reading the stored image failed";
        uint32_t id = rd32(th), len = rd32(th + 4);
        if (first) {
            if (id == GBL4_ROOT) return "GBL v4 images aren't supported";
            if (id != GBL_HEADER) return "Not a GBL firmware image (.gbl)";
            if (len != 8) return "The GBL header is malformed";
        }
        if (len > size - off - 8) return "The image is cut short (a tag runs past the end of the file)";
        crc = nf_crc32(crc, th, 8);
        // Payload: CRC all of it, keep what we need to look at.
        uint32_t keep = 0;
        uint8_t head[32];
        for (uint32_t p = 0; p < len;) {
            uint32_t n = len - p < sizeof(buf) ? len - p : sizeof(buf);
            if (io->image_read(io->ctx, off + 8 + p, buf, n) != 0) return "Reading the stored image failed";
            crc = nf_crc32(crc, buf, n);
            if (keep < sizeof(head)) {
                uint32_t k = n < sizeof(head) - keep ? n : sizeof(head) - keep;
                memcpy(head + keep, buf, k);
                keep += k;
            }
            if (id == GBL_METADATA) {
                size_t have = strlen(g->metadata);
                size_t room = sizeof(g->metadata) - 1 - have;
                size_t k = n < room ? n : room;
                for (size_t i = 0; i < k; i++) {
                    char c = (char)buf[i];
                    g->metadata[have + i] = (c >= 0x20 && c < 0x7F) ? c : ' ';
                }
                g->metadata[have + k] = 0;
                if (k < n) g->metadata_cut = true;
            }
            p += n;
        }
        switch (id) {
            case GBL_HEADER:
                if (!first) return "The image has a second GBL header";
                g->gbl_version = rd32(head);  // 0x03000000 for v3 (bytes 00 00 00 03)
                g->gbl_type = rd32(head + 4);
                if ((g->gbl_version >> 24) != 3) return "Only GBL v3 images are supported";
                g->encrypted = (g->gbl_type & 0x01) != 0;
                g->is_signed = (g->gbl_type & 0x100) != 0;
                break;
            case GBL_APP_INFO:
                if (len < 28) return "The application info tag is malformed";
                g->has_app_info = true;
                g->app_type = rd32(head);
                g->app_version = rd32(head + 4);
                break;
            case GBL_BOOTLOADER:
                return "This image replaces the dongle's bootloader: the bridge only flashes Zigbee firmware (use a PC for that)";
            case GBL_SE_UPGRADE:
                return "This image upgrades the dongle's secure element: the bridge only flashes Zigbee firmware (use a PC for that)";
            case GBL_PROG:
            case GBL_ERASEPROG:
            case GBL_PROG_LZ4:
            case GBL_PROG_LZMA:
                if (len < 4) return "A program data tag is malformed";
                g->prog_bytes += len - 4;
                break;
            case GBL_DELTA:
            case GBL_DELTA_LZ4:
            case GBL_DELTA_LZMA:
                g->has_delta = true;
                break;
            case GBL_ENC_DATA:
                g->prog_bytes += len;
                break;
            case GBL_SIGNATURE:
                g->is_signed = true;
                break;
            case GBL_VERSION_DEP:
            case GBL_METADATA:
            case GBL_CERT:
            case GBL_ENC_HEADER:
            case GBL_ENC_INIT:
                break;
            case GBL_END:
                if (len != 4) return "The end tag is malformed";
                ended = true;
                break;
            default: {
                static char why[64];
                snprintf(why, sizeof(why), "The image has an unknown tag 0x%08lx", (unsigned long)id);
                return why;
            }
        }
        first = false;
        off += 8 + len;
    }
    g->tag_len = off;
    g->trailing = size - off;
    if (crc != GBL_CRC_RESIDUE) return "The image's CRC-32 doesn't match: the file is damaged";
    if (g->prog_bytes == 0 && !g->has_delta) return "The image has no firmware in it";
    if (g->has_app_info && g->app_type != 0 && (g->app_type & NF_APP_ZIGBEE) == 0) {
        if (g->app_type & NF_APP_THREAD) return "This is Thread firmware, not Zigbee";
        if (g->app_type & NF_APP_BLUETOOTH) return "This is Bluetooth firmware, not Zigbee";
        return "This firmware isn't Zigbee";
    }
    return NULL;
}

// ---------------------------------------------------------------- ASH / EZSP (just enough to talk to EmberZNet)

#define ASH_FLAG 0x7E
#define ASH_ESC 0x7D
#define ASH_XON 0x11
#define ASH_XOFF 0x13
#define ASH_SUB 0x18
#define ASH_CANCEL 0x1A

typedef struct {
    uint8_t frm_tx, ack_rx, seq;
    bool extended;
    uint8_t rx[160];
    size_t rn;
    bool esc, bad;
} ash_t;

static void randomize(uint8_t *d, size_t n) {
    uint8_t r = 0x42;
    for (size_t i = 0; i < n; i++) {
        d[i] ^= r;
        r = (r & 1) ? (uint8_t)((r >> 1) ^ 0xB8) : (uint8_t)(r >> 1);
    }
}

static int ash_send(const nf_io_t *io, uint8_t control, const uint8_t *data, size_t n) {
    uint8_t raw[80], out[170];
    if (n + 3 > sizeof(raw)) return -1;
    raw[0] = control;
    if (n) memcpy(raw + 1, data, n);
    uint16_t c = crc16_ash(raw, n + 1);
    raw[n + 1] = (uint8_t)(c >> 8);
    raw[n + 2] = (uint8_t)c;
    size_t o = 0;
    for (size_t i = 0; i < n + 3; i++) {
        uint8_t b = raw[i];
        if (b == ASH_FLAG || b == ASH_ESC || b == ASH_XON || b == ASH_XOFF || b == ASH_SUB || b == ASH_CANCEL) {
            out[o++] = ASH_ESC;
            out[o++] = b ^ 0x20;
        } else {
            out[o++] = b;
        }
    }
    out[o++] = ASH_FLAG;
    return io->write(io->ctx, out, o);
}

// Next good frame (control + data, CRC stripped) within timeout_ms. Length, 0 on timeout, <0 dongle gone.
static int ash_recv(const nf_io_t *io, ash_t *a, uint8_t *out, size_t max, uint32_t timeout_ms) {
    uint32_t t0 = io->now_ms(io->ctx);
    for (;;) {
        uint32_t w = left(io, t0, timeout_ms);
        if (w == 0) return 0;
        uint8_t b;
        int r = io->read(io->ctx, &b, 1, w);
        if (r < 0) return r;
        if (r == 0) continue;
        switch (b) {
            case ASH_FLAG: {
                size_t n = a->rn;
                bool bad = a->bad;
                a->rn = 0;
                a->esc = a->bad = false;
                if (bad || n < 3) break;
                uint16_t want = (uint16_t)((a->rx[n - 2] << 8) | a->rx[n - 1]);
                if (crc16_ash(a->rx, n - 2) != want) break;
                if (n - 2 > max) break;
                memcpy(out, a->rx, n - 2);
                return (int)(n - 2);
            }
            case ASH_CANCEL:
                a->rn = 0;
                a->esc = a->bad = false;
                break;
            case ASH_SUB:
                a->bad = true;
                break;
            case ASH_XON:
            case ASH_XOFF:
                break;
            case ASH_ESC:
                a->esc = true;
                break;
            default:
                if (a->esc) {
                    b ^= 0x20;
                    a->esc = false;
                }
                if (a->rn < sizeof(a->rx)) a->rx[a->rn++] = b;
                else a->bad = true;
                break;
        }
    }
}

static bool ash_reset(const nf_io_t *io, ash_t *a, nf_ncp_t *out) {
    memset(a, 0, sizeof(*a));
    for (int attempt = 0; attempt < 2; attempt++) {
        static const uint8_t cancel = ASH_CANCEL;
        io->write(io->ctx, &cancel, 1);
        ash_send(io, 0xC0, NULL, 0);
        uint32_t t0 = io->now_ms(io->ctx);
        uint8_t f[160];
        while (!elapsed(io, t0, 2500)) {
            int n = ash_recv(io, a, f, sizeof(f), left(io, t0, 2500));
            if (n < 0) return false;
            if (n >= 3 && f[0] == 0xC1) {
                out->reset_code = f[2];
                return true;
            }
        }
    }
    return false;
}

// One EZSP command; the response's EZSP bytes (derandomized) in resp. Length, or -1.
static int ezsp_txn(const nf_io_t *io, ash_t *a, uint16_t frame_id, const uint8_t *params, size_t pn, uint8_t *resp,
                    size_t rmax, uint32_t timeout_ms) {
    uint8_t ez[40];
    size_t n = 0;
    ez[n++] = a->seq;
    if (a->extended) {
        ez[n++] = 0x00;
        ez[n++] = 0x01;
        ez[n++] = (uint8_t)frame_id;
        ez[n++] = (uint8_t)(frame_id >> 8);
    } else {
        ez[n++] = 0x00;
        ez[n++] = (uint8_t)frame_id;
    }
    if (pn > sizeof(ez) - n) return -1;
    memcpy(ez + n, params, pn);
    n += pn;
    randomize(ez, n);
    uint8_t want_seq = a->seq++;
    uint8_t control = (uint8_t)((a->frm_tx << 4) | a->ack_rx);
    a->frm_tx = (a->frm_tx + 1) & 7;
    for (int attempt = 0; attempt < 3; attempt++) {
        ash_send(io, attempt ? (uint8_t)(control | 0x08) : control, ez, n);
        uint32_t t0 = io->now_ms(io->ctx);
        uint8_t f[160];
        while (!elapsed(io, t0, timeout_ms)) {
            int fn = ash_recv(io, a, f, sizeof(f), left(io, t0, timeout_ms));
            if (fn < 0) return -1;
            if (fn < 1 || (f[0] & 0x80) != 0) continue;  // ACK/NAK/other: keep waiting for the DATA answer
            uint8_t frm = (f[0] >> 4) & 7;
            a->ack_rx = (frm + 1) & 7;
            ash_send(io, (uint8_t)(0x80 | a->ack_rx), NULL, 0);
            size_t en = (size_t)fn - 1;
            if (en > rmax) en = rmax;
            memcpy(resp, f + 1, en);
            randomize(resp, en);
            if (en < 3 || resp[0] != want_seq || (resp[1] & 0x80) == 0) continue;  // not our answer
            return (int)en;
        }
    }
    return -1;
}

// Version exchange, as bellows does it: ask in the legacy frame format, then again in the extended one
// at the version the firmware speaks.
static bool ezsp_version(const nf_io_t *io, ash_t *a, nf_ncp_t *out) {
    uint8_t r[40];
    a->extended = false;
    uint8_t want = 4;
    int n = ezsp_txn(io, a, 0x0000, &want, 1, r, sizeof(r), 1500);
    if (n < 7) return false;
    uint8_t ver = r[3];
    if (ver >= 8) {
        a->extended = true;
        n = ezsp_txn(io, a, 0x0000, &ver, 1, r, sizeof(r), 1500);
        if (n < 9 || r[5] != ver) return false;
        out->ezsp_version = r[5];
        out->stack_type = r[6];
        out->stack_version = (uint16_t)(r[7] | (r[8] << 8));
    } else {
        out->ezsp_version = ver;
        out->stack_type = r[4];
        out->stack_version = (uint16_t)(r[5] | (r[6] << 8));
    }
    out->ok = true;
    return true;
}

bool nf_ncp_probe(const nf_io_t *io, uint32_t baud, nf_ncp_t *out) {
    memset(out, 0, sizeof(*out));
    if (io->set_baud(io->ctx, baud) != 0) return false;
    drain(io);
    ash_t a;
    if (!ash_reset(io, &a, out)) return false;
    return ezsp_version(io, &a, out);
}

// ---------------------------------------------------------------- Gecko bootloader

static const char *find(const char *hay, size_t n, const char *needle) {
    size_t k = strlen(needle);
    for (size_t i = 0; i + k <= n; i++) {
        if (memcmp(hay + i, needle, k) == 0) return hay + i;
    }
    return NULL;
}

typedef struct {
    char t[768];
    size_t n;
} text_t;

static void text_add(text_t *t, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (t->n == sizeof(t->t) - 1) {  // keep the newest half
            memmove(t->t, t->t + sizeof(t->t) / 2, t->n - sizeof(t->t) / 2);
            t->n -= sizeof(t->t) / 2;
        }
        t->t[t->n++] = d[i] ? (char)d[i] : ' ';
    }
    t->t[t->n] = 0;
}

// The full menu ("... Bootloader v<x>\r\n1. upload gbl\r\n2. run\r\n3. ebl info\r\n... BL > ") in what
// arrives within timeout_ms. Copies the version.
static bool wait_menu(const nf_io_t *io, text_t *t, char *ver, size_t vlen, uint32_t timeout_ms) {
    uint32_t t0 = io->now_ms(io->ctx);
    for (;;) {
        const char *b = find(t->t, t->n, "Bootloader v");
        if (b) {
            const char *rest = b + 12;
            size_t rn = t->n - (size_t)(rest - t->t);
            const char *eol = find(rest, rn, "\r\n");
            if (eol && find(eol, rn - (size_t)(eol - rest), "1. upload") && find(eol, rn - (size_t)(eol - rest), "BL > ")) {
                if (ver) {
                    size_t k = (size_t)(eol - rest);
                    if (k >= vlen) k = vlen - 1;
                    memcpy(ver, rest, k);
                    ver[k] = 0;
                }
                t->n = 0;
                t->t[0] = 0;
                return true;
            }
        }
        uint32_t w = left(io, t0, timeout_ms);
        if (w == 0) return false;
        uint8_t d[64];
        int r = io->read(io->ctx, d, sizeof(d), w);
        if (r < 0) return false;
        if (r > 0) text_add(t, d, (size_t)r);
    }
}

// Is the bootloader there? Asks for "ebl info" (prints the menu) a few times.
static bool bl_probe(const nf_io_t *io, char *ver, size_t vlen, int tries) {
    if (io->set_baud(io->ctx, NF_BL_BAUD) != 0) return false;
    drain(io);
    text_t t = {.n = 0};
    for (int i = 0; i < tries; i++) {
        static const uint8_t ask[] = {'\n', '3'};
        if (io->write(io->ctx, ask, sizeof(ask)) != 0) return false;
        if (wait_menu(io, &t, ver, vlen, 1000)) return true;
    }
    return false;
}

static bool launch_from_app(const nf_io_t *io, uint32_t baud, nf_ncp_t *before) {
    memset(before, 0, sizeof(*before));
    if (io->set_baud(io->ctx, baud) != 0) return false;
    drain(io);
    ash_t a;
    if (!ash_reset(io, &a, before)) return false;
    if (!ezsp_version(io, &a, before)) return false;
    logf_(io, "Dongle firmware answers: EZSP v%u, stack %u.%u.%u.%u", before->ezsp_version, (before->stack_version >> 12) & 15,
          (before->stack_version >> 8) & 15, (before->stack_version >> 4) & 15, before->stack_version & 15);
    uint8_t mode = 0x01, r[40];  // STANDALONE_BOOTLOADER_NORMAL_MODE
    int n = ezsp_txn(io, &a, 0x008F, &mode, 1, r, sizeof(r), 1500);
    if (n < 0) logf_(io, "No answer to launchStandaloneBootloader (it may have started anyway)");
    else {
        size_t sp = a.extended ? 5 : 3;
        if ((size_t)n > sp && r[sp] != 0) {
            logf_(io, "The firmware refused to start its bootloader (status 0x%02x)", r[sp]);
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- Spinel (OpenThread RCP)

static uint16_t crc_x25(const uint8_t *d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int b = 0; b < 8; b++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
    }
    return (uint16_t)~c;
}

static bool hdlc_special(uint8_t b) { return b == 0x7E || b == 0x7D || b == 0x11 || b == 0x13 || b == 0xF8; }

static int spinel_send(const nf_io_t *io, const uint8_t *p, size_t n) {
    uint8_t out[48];
    size_t k = 0;
    uint16_t fcs = crc_x25(p, n);
    uint8_t tail[2] = {(uint8_t)fcs, (uint8_t)(fcs >> 8)};
    out[k++] = 0x7E;
    for (size_t i = 0; i < n + 2 && k + 3 < sizeof(out); i++) {
        uint8_t b = i < n ? p[i] : tail[i - n];
        if (hdlc_special(b)) {
            out[k++] = 0x7D;
            out[k++] = b ^ 0x20;
        } else {
            out[k++] = b;
        }
    }
    out[k++] = 0x7E;
    return io->write(io->ctx, out, k);
}

// Next good HDLC frame (FCS stripped) within timeout_ms; length, 0 on timeout, <0 when the dongle is gone.
static int spinel_frame(const nf_io_t *io, uint8_t *f, size_t max, uint32_t timeout_ms) {
    uint32_t t0 = io->now_ms(io->ctx);
    size_t n = 0;
    bool esc = false;
    for (;;) {
        uint32_t el = io->now_ms(io->ctx) - t0;
        if (el >= timeout_ms) return 0;
        uint8_t b;
        int r = io->read(io->ctx, &b, 1, timeout_ms - el);
        if (r < 0) return r;
        if (r == 0) continue;
        if (b == 0x7E) {
            if (n >= 3 && crc_x25(f, n - 2) == (uint16_t)(f[n - 2] | f[n - 1] << 8)) return (int)(n - 2);
            n = 0;
            esc = false;
        } else if (b == 0x7D) {
            esc = true;
        } else if (n < max) {
            f[n++] = esc ? b ^ 0x20 : b;
            esc = false;
        }
    }
}

bool nf_spinel_probe(const nf_io_t *io, uint32_t baud, char *ver, size_t vlen) {
    if (vlen) ver[0] = 0;
    if (io->set_baud(io->ctx, baud) != 0) return false;
    drain(io);
    static const uint8_t get_version[] = {0x81, 0x02, 0x02};  // header TID 1, CMD_PROP_VALUE_GET, PROP_NCP_VERSION
    uint8_t f[140];
    for (int attempt = 0; attempt < 3; attempt++) {
        if (spinel_send(io, get_version, sizeof(get_version)) != 0) return false;
        uint32_t t0 = io->now_ms(io->ctx);
        while (io->now_ms(io->ctx) - t0 < 800) {
            int n = spinel_frame(io, f, sizeof(f), 800);
            if (n < 0) return false;
            if (n == 0) break;
            // header (flag bit set), CMD_PROP_VALUE_IS (6), PROP_NCP_VERSION (2), NUL-terminated string
            if (n >= 4 && (f[0] & 0xC0) == 0x80 && f[1] == 0x06 && f[2] == 0x02) {
                size_t k = 0;
                for (int i = 3; i < n && f[i] && k + 1 < vlen; i++) ver[k++] = (f[i] >= 0x20 && f[i] < 0x7F) ? (char)f[i] : '?';
                if (vlen) ver[k] = 0;
                return true;
            }
        }
    }
    return false;
}

// A running RCP asked to reset into its bootloader (Spinel CMD_RESET, reset type BOOTLOADER). Builds
// without bootloader support just restart; the caller probes for the menu either way.
static bool spinel_bootloader(const nf_io_t *io, uint32_t baud, char *ver, size_t vlen) {
    if (!nf_spinel_probe(io, baud, ver, vlen)) return false;
    logf_(io, "Dongle firmware answers Spinel: %s", ver);
    static const uint8_t reset_bl[] = {0x81, 0x01, 0x03};  // CMD_RESET, SPINEL_RESET_BOOTLOADER
    spinel_send(io, reset_bl, sizeof(reset_bl));
    return true;
}

static void reset_into_bootloader(const nf_io_t *io) {  // universal-silabs-flasher's rts_dtr pattern
    io->set_lines(io->ctx, false, true);
    io->sleep_ms(io->ctx, 100);
    io->set_lines(io->ctx, true, false);
    io->sleep_ms(io->ctx, 500);
    io->set_lines(io->ctx, false, false);
}

// The bootloader's reason after "Serial upload aborted": printable, one line, without the menu.
static void aborted_reason(const text_t *t, char *why, size_t wn) {
    why[0] = 0;
    const char *bad = find(t->t, t->n, "Serial upload aborted");
    if (!bad) return;
    size_t k = 0;
    for (const char *p = bad + 21; *p && k < wn - 1; p++) {
        if (strncmp(p, "BL > ", 5) == 0 || strncmp(p, "Gecko Bootloader", 16) == 0 || strncmp(p, "Bootloader v", 12) == 0) break;
        if (*p >= 0x20 && *p < 0x7F) why[k++] = *p;
        else if (k && why[k - 1] != ' ') why[k++] = ' ';
    }
    while (k && why[k - 1] == ' ') k--;
    why[k] = 0;
}

#define XM_SOH 0x01
#define XM_EOT 0x04
#define XM_ACK 0x06
#define XM_NAK 0x15
#define XM_CAN 0x18
#define XM_BLOCK 128
#define XM_TIMEOUT_MS 3000
#define XM_MAX_FAILS 6

// "1", then the image over XMODEM-CRC, then the bootloader's verdict.
static bool upload(const nf_io_t *io, uint32_t len, nf_result_t *r, char *err, size_t el) {
    text_t t = {.n = 0};
    static const uint8_t one = '1';
    drain(io);
    if (io->write(io->ctx, &one, 1) != 0) {
        seterr(err, el, "The dongle went away");
        return false;
    }
    // "begin upload", then 'C' once a second until the first block.
    uint32_t t0 = io->now_ms(io->ctx);
    bool ready = false;
    while (!ready && !elapsed(io, t0, 5000)) {
        uint8_t d[32];
        int n = io->read(io->ctx, d, sizeof(d), left(io, t0, 5000));
        if (n < 0) break;
        for (int i = 0; i < n; i++) ready |= d[i] == 'C';
    }
    if (!ready) {
        seterr(err, el, "The bootloader didn't start the upload");
        return false;
    }
    io->sleep_ms(io->ctx, 20);
    drain(io);  // spare 'C's

    uint32_t blocks = (len + XM_BLOCK - 1) / XM_BLOCK;
    uint8_t pkt[3 + XM_BLOCK + 2];
    for (uint32_t i = 0; i < blocks; i++) {
        uint32_t off = i * XM_BLOCK, n = len - off < XM_BLOCK ? len - off : XM_BLOCK;
        memset(pkt + 3, 0xFF, XM_BLOCK);
        if (io->image_read(io->ctx, off, pkt + 3, n) != 0) {
            seterr(err, el, "Reading the stored image failed at byte %lu", (unsigned long)off);
            return false;
        }
        uint8_t num = (uint8_t)(i + 1);
        pkt[0] = XM_SOH;
        pkt[1] = num;
        pkt[2] = (uint8_t)(0xFF - num);
        uint16_t c = crc16_xmodem(pkt + 3, XM_BLOCK);
        pkt[3 + XM_BLOCK] = (uint8_t)(c >> 8);
        pkt[4 + XM_BLOCK] = (uint8_t)c;
        int fails = 0;
        for (;;) {
            if (io->write(io->ctx, pkt, sizeof(pkt)) != 0) {
                seterr(err, el, "The dongle went away during the upload (%lu%%)", (unsigned long)(off * 100 / len));
                return false;
            }
            uint8_t resp = 0;
            uint32_t w0 = io->now_ms(io->ctx);
            while (!elapsed(io, w0, XM_TIMEOUT_MS)) {
                int rn = io->read(io->ctx, &resp, 1, left(io, w0, XM_TIMEOUT_MS));
                if (rn < 0) {
                    seterr(err, el, "The dongle went away during the upload (%lu%%)", (unsigned long)(off * 100 / len));
                    return false;
                }
                if (rn == 0) break;
                if (i == 0 && resp == 'C') {  // a late "ready" from before the first block
                    resp = 0;
                    continue;
                }
                break;
            }
            if (resp == XM_ACK) break;
            if (resp == XM_CAN) {
                // Whatever the bootloader says after cancelling helps (e.g. the image was refused).
                uint8_t d[64];
                uint32_t c0 = io->now_ms(io->ctx);
                while (!elapsed(io, c0, 700)) {
                    int dn = io->read(io->ctx, d, sizeof(d), left(io, c0, 700));
                    if (dn <= 0) break;
                    text_add(&t, d, (size_t)dn);
                }
                char why[80];
                aborted_reason(&t, why, sizeof(why));
                if (why[0]) seterr(err, el, "The bootloader rejected the image at %lu%%: %s", (unsigned long)(off * 100 / len), why);
                else seterr(err, el, "The bootloader cancelled the upload at %lu%%", (unsigned long)(off * 100 / len));
                return false;
            }
            r->retries++;
            if (++fails >= XM_MAX_FAILS) {
                seterr(err, el, "The upload kept failing at %lu%% (%s)", (unsigned long)(off * 100 / len),
                       resp == 0 ? "no answer" : resp == XM_NAK ? "refused" : "garbled answers");
                return false;
            }
        }
        if ((i & 15) == 15 || i + 1 == blocks) progress(io, "upload", (i + 1) * XM_BLOCK < len ? (i + 1) * XM_BLOCK : len, len);
    }
    bool eot_ok = false;
    for (int k = 0; k < 3 && !eot_ok; k++) {
        static const uint8_t eot = XM_EOT;
        io->write(io->ctx, &eot, 1);
        uint8_t resp = 0;
        uint32_t w0 = io->now_ms(io->ctx);
        while (!elapsed(io, w0, XM_TIMEOUT_MS)) {
            int rn = io->read(io->ctx, &resp, 1, left(io, w0, XM_TIMEOUT_MS));
            if (rn <= 0) break;
            if (resp == XM_ACK) {
                eot_ok = true;
                break;
            }
            text_add(&t, &resp, 1);
        }
    }
    if (!eot_ok) {
        seterr(err, el, "The bootloader didn't confirm the end of the upload");
        return false;
    }
    // "\r\nSerial upload complete\r\n" (or "aborted" + why), then the menu.
    uint32_t s0 = io->now_ms(io->ctx);
    for (;;) {
        const char *ok = find(t.t, t.n, "Serial upload complete");
        const char *bad = find(t.t, t.n, "Serial upload aborted");
        if (ok) return true;
        if (bad && (find(bad, t.n - (size_t)(bad - t.t), "BL > ") || elapsed(io, s0, 5000))) {
            char why[80];
            aborted_reason(&t, why, sizeof(why));
            seterr(err, el, "The bootloader rejected the image%s%s", why[0] ? ": " : "", why);
            return false;
        }
        uint32_t w = left(io, s0, 5000);
        if (w == 0) {
            if (bad) continue;
            seterr(err, el, "No word from the bootloader after the upload");
            return false;
        }
        uint8_t d[64];
        int n = io->read(io->ctx, d, sizeof(d), w);
        if (n < 0) {
            seterr(err, el, "The dongle went away after the upload");
            return false;
        }
        if (n > 0) text_add(&t, d, (size_t)n);
    }
}

// Ends an XMODEM transfer we gave up on, so the bootloader goes back to its menu.
static void xm_cancel(const nf_io_t *io) {
    static const uint8_t can[] = {XM_CAN, XM_CAN, XM_CAN};
    io->write(io->ctx, can, sizeof(can));
    io->sleep_ms(io->ctx, 500);
    drain(io);
}

// "2". If the menu comes back, there's no runnable firmware.
static bool run_app(const nf_io_t *io) {
    static const uint8_t two = '2';
    drain(io);
    io->write(io->ctx, &two, 1);
    text_t t = {.n = 0};
    return !wait_menu(io, &t, NULL, 0, 2000);
}

static void fmt_ncp(char *out, size_t n, const nf_ncp_t *v) {
    snprintf(out, n, "EZSP v%u, stack %u.%u.%u.%u", v->ezsp_version, (v->stack_version >> 12) & 15, (v->stack_version >> 8) & 15,
             (v->stack_version >> 4) & 15, v->stack_version & 15);
}

bool nf_flash(const nf_io_t *io, const nf_flash_opts_t *o, nf_result_t *r, char *err, size_t el) {
    memset(r, 0, sizeof(*r));
    err[0] = 0;

    // 1. Into the bootloader.
    progress(io, "bootloader", 0, 0);
    bool in_bl = false;
    if (launch_from_app(io, o->app_baud, &r->before)) {
        io->sleep_ms(io->ctx, 1000);
        if (bl_probe(io, r->bootloader, sizeof(r->bootloader), 4)) {
            in_bl = true;
            r->entered = "asked the firmware";
        }
    } else if (spinel_bootloader(io, o->app_baud, r->before_spinel, sizeof(r->before_spinel))) {
        io->sleep_ms(io->ctx, 1000);
        if (bl_probe(io, r->bootloader, sizeof(r->bootloader), 4)) {
            in_bl = true;
            r->entered = "asked the firmware (Spinel)";
        }
    }
    if (!in_bl && bl_probe(io, r->bootloader, sizeof(r->bootloader), 2)) {
        in_bl = true;
        r->entered = "already in the bootloader";
    }
    if (!in_bl) {
        const char *how = "RTS/DTR reset";
        int hr = 0;
        if (io->hw_reset) {
            hr = io->hw_reset(io->ctx, &how);
            if (hr < 0) logf_(io, "This dongle has no hardware reset into its bootloader");
        } else {
            logf_(io, "Trying the RTS/DTR reset into the bootloader");
            reset_into_bootloader(io);
        }
        if (hr >= 0 && bl_probe(io, r->bootloader, sizeof(r->bootloader), 3)) {
            in_bl = true;
            r->entered = how;
        }
    }
    if (!in_bl) {
        seterr(err, el, "Couldn't get the dongle into its bootloader (no EZSP answer at %lu baud and no bootloader menu)",
               (unsigned long)o->app_baud);
        r->app_left_running = r->before.ok;
        return false;
    }
    logf_(io, "In the Gecko bootloader v%s (%s)", r->bootloader, r->entered);

    // 2. Upload, once more if the first try fails.
    bool up = false;
    for (int attempt = 0; attempt < 2 && !up; attempt++) {
        if (attempt) {
            logf_(io, "Upload failed (%s); trying once more", err);
            text_t t = {.n = 0};
            static const uint8_t ask[] = {'\n', '3'};
            drain(io);
            io->write(io->ctx, ask, sizeof(ask));
            if (!wait_menu(io, &t, NULL, 0, 3000) && !bl_probe(io, NULL, 0, 2)) break;
        }
        progress(io, "upload", 0, o->upload_len);
        up = upload(io, o->upload_len, r, err, el);
        if (!up) xm_cancel(io);
        if (!up && strstr(err, "rejected")) break;  // the bootloader looked at it and said no: again won't help
        if (!up && strstr(err, "went away")) break;
    }
    if (!up) {
        // A refused image may have left the old firmware in place: try to start it.
        if (run_app(io)) {
            nf_ncp_t back;
            io->sleep_ms(io->ctx, 1000);
            if (nf_ncp_probe(io, o->app_baud, &back) && back.ok) {
                r->app_left_running = true;
                r->after = back;
                return false;
            }
        }
        r->in_bootloader = true;
        return false;
    }
    logf_(io, "Upload complete (%lu bytes, %lu blocks sent again)", (unsigned long)o->upload_len, (unsigned long)r->retries);

    // 3. Run it and make sure it answers.
    progress(io, "start", 0, 0);
    if (!run_app(io)) {
        seterr(err, el, "The dongle didn't start the new firmware (its bootloader is still showing)");
        r->in_bootloader = true;
        return false;
    }
    progress(io, "verify", 0, 0);
    io->sleep_ms(io->ctx, 500);
    if (o->expect == NF_EXPECT_SPINEL) {
        for (int i = 0; i < 5; i++) {
            if (nf_spinel_probe(io, o->new_baud, r->after_spinel, sizeof(r->after_spinel))) {
                logf_(io, "New firmware answers Spinel: %s", r->after_spinel);
                return true;
            }
            io->sleep_ms(io->ctx, 1000);
        }
        seterr(err, el, "The new firmware started but doesn't answer as an OpenThread RCP (Spinel) at %lu baud",
               (unsigned long)o->new_baud);
        return false;
    }
    for (int i = 0; i < 5; i++) {
        if (nf_ncp_probe(io, o->new_baud, &r->after) && r->after.ok) {
            char v[48];
            fmt_ncp(v, sizeof(v), &r->after);
            logf_(io, "New firmware answers: %s", v);
            return true;
        }
        io->sleep_ms(io->ctx, 1000);
    }
    seterr(err, el, "The new firmware started but doesn't answer as a Zigbee coordinator (EZSP) at %lu baud",
           (unsigned long)o->new_baud);
    return false;
}

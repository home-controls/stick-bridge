#include "gcfflash_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// Everything protocol-level below is from gcfflasher (gcf.c / protocol.c at 25ccb7c); the gcf.c names are
// given next to the values.

#define GCF_MAGIC 0xCAFEFEEDu  // GCF_MAGIC
#define GCF_HEADER_SIZE 14     // GCF_HEADER_SIZE
#define PRODUCT_HIVE 0xDEC0DE02u
#define PRODUCT_CONBEE3 0xDEC0DE03u
#define PLATFORM_R21 0x07  // FW_VERSION_PLATFORM_R21 >> 8
#define PLATFORM_MG21 0x09

// deCONZ serial protocol (application)
#define CMD_FIRMWARE_VERSION 0x0D
#define CMD_WRITE_PARAMETER 0x0B
#define PARAM_WATCHDOG_TIMEOUT 0x26

// V3 bootloader
#define BTL_MAGIC 0x81
#define BTL_ID_REQUEST 0x02
#define BTL_ID_RESPONSE 0x82
#define BTL_FW_UPDATE_REQUEST 0x03
#define BTL_FW_UPDATE_RESPONSE 0x83
#define BTL_FW_DATA_REQUEST 0x04
#define BTL_FW_DATA_RESPONSE 0x84
#define V3_MAX_CHUNK 480  // gcfflasher refuses (status 2) more than sizeof(ascii) - 32

// Timing (ms)
#define T_PROBE 500             // our own: who answers, the app or a bootloader
#define T_RESET 6000            // RESET_TIMEOUT_MS: watchdog request -> USB drop / bootloader
#define T_REOPEN 10000          // BTL_CONNECT_ATTEMPTS * BTL_CONNECT_RETRY_MS (+ settle)
#define T_V1_ID 1500            // V1_ID_WAIT_MS
#define T_V3_ID 200             // BTL_ID_WAIT_MS
#define ID_WINDOWS 3            // V1_ID_ATTEMPTS
#define T_V1_SYNC 500           // ST_V1ProgramSync
#define T_V1_FIRST 1000         // ST_V1ProgramWriteHeader
#define T_V1_PAGE 2000          // V1_PAGE_TIMEOUT_MS
#define T_V1_COMMIT 60000       // V1_VALIDATE_TIMEOUT_MS
#define T_V3_SETTLE 50          // ST_V3ProgramSync PL_MSleep(50)
#define T_V3_UPDATE 1000        // ST_V3ProgramSync
#define T_V3_FIRST 3000         // after FW_UPDATE_RESPONSE
#define T_V3_DATA 5000          // ST_V3ProgramUpload
#define T_V3_VERIFY 20000       // ST_V3ProgramWaitID
#define T_APP_BACK 15000        // our own: the new firmware must answer within this
#define T_RETRY_PAUSE 250       // gcfRetry
#define DEFAULT_BUDGET 60000    // gcfflasher's default -t
#define MAX_ATTEMPTS 10
#define V1_PAGESIZE 256         // V1_PAGESIZE

// ---------------------------------------------------------------- small helpers

static void logf_(const gf_io_t *io, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(const gf_io_t *io, const char *fmt, ...) {
    if (io->log == NULL) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    io->log(io->ctx, b);
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
static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint8_t *wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) *p++ = (uint8_t)(v >> (8 * i));
    return p;
}
static inline uint8_t *wr16(uint8_t *p, uint16_t v) {
    *p++ = (uint8_t)v;
    *p++ = (uint8_t)(v >> 8);
    return p;
}

static uint32_t now(const gf_io_t *io) { return io->now_ms(io->ctx); }
static bool past(const gf_io_t *io, uint32_t deadline) { return (int32_t)(deadline - now(io)) <= 0; }

uint8_t gf_crc8(uint8_t crc, const uint8_t *d, size_t n) {  // CRC8_Dallas() in gcf.c
    while (n--) {
        crc ^= *d++;
        for (int i = 0; i < 8; i++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
    return crc;
}

uint32_t gf_crc32(uint32_t crc, const uint8_t *d, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

static const char *dev_name(gf_device_t d) {
    return d == GF_DEV_CONBEE2 ? "ConBee II" : d == GF_DEV_CONBEE3 ? "ConBee III" : "unknown stick";
}

// ---------------------------------------------------------------- the file

static char s_why[200];  // formatted gf_file_check reasons (one flasher at a time)

static const char *why(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static const char *why(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_why, sizeof(s_why), fmt, ap);
    va_end(ap);
    return s_why;
}

// GCF_ParseFile(): the first "0x" in the name and the hex digits after it.
static uint32_t name_version(const char *name) {
    if (name == NULL) return 0;
    const char *p = strstr(name, "0x");
    if (p == NULL) return 0;
    uint32_t v = 0;
    int digits = 0;
    for (p += 2; *p; p++, digits++) {
        char c = *p;
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = (v << 4) | (uint32_t)d;
    }
    return digits == 8 ? v : 0;
}

const char *gf_file_check(const gf_io_t *io, uint32_t size, const char *name, gf_device_t want, gf_file_t *f) {
    memset(f, 0, sizeof(*f));
    uint8_t h[GCF_HEADER_SIZE + 28];
    if (size < GCF_HEADER_SIZE) return "The file is too short to be a GCF image";
    if (io->image_read(io->ctx, 0, h, GCF_HEADER_SIZE) != 0) return "Reading the stored image failed";
    if (rd32(h) != GCF_MAGIC) return "Not a GCF firmware file (no 0xCAFEFEED header)";
    // GCF header (14 bytes, LE): u32 magic, u8 file type, u32 target address, u32 file size, u8 checksum
    f->type = h[4];
    f->target = rd32(h + 5);
    f->data_size = rd32(h + 9);
    f->crc8 = h[13];
    f->data_offset = GCF_HEADER_SIZE;
    f->name_version = name_version(name);
    uint8_t name_plat = (uint8_t)(f->name_version >> 8);
    if (name && (strstr(name, "RaspBee") || strstr(name, "Hive"))) return "This is RaspBee/Hive firmware, not ConBee";

    if (f->type < 30) {  // gcfflasher: file type < 30 -> V1 bootloader
        f->proto = 1;
        if (f->type == 1 && f->target == 0) return "This file is for a ConBee I / RaspBee I (AVR), not a ConBee II";
        if (f->target != 0x5000) return why("Unexpected target address 0x%X for ConBee II firmware (0x5000)", (unsigned)f->target);
        f->device = GF_DEV_CONBEE2;
        f->platform = PLATFORM_R21;
    } else if (f->type == 60) {  // FLASH_TYPE_APP_ENCRYPTED
        f->proto = 3;
        if (size < GCF_HEADER_SIZE + 28 + 4) return "The file is cut short (no encrypted container)";
        if (io->image_read(io->ctx, GCF_HEADER_SIZE, h + GCF_HEADER_SIZE, 28) != 0) return "Reading the stored image failed";
        // u32 product magic, u32 total size, then per image: u32 image size, u32 type, u32 target,
        // u32 plain size, u32 plain crc, data; a u32 CRC-32 over everything before it closes the container.
        const uint8_t *c = h + GCF_HEADER_SIZE;
        f->product = rd32(c);
        uint32_t total = rd32(c + 4), img = rd32(c + 8);
        f->crc32 = rd32(c + 24);
        if (f->product == PRODUCT_HIVE) return "This is Hive firmware (0xDEC0DE02), not ConBee III";
        if (f->product != PRODUCT_CONBEE3)
            return why("Unknown product 0x%08X in the encrypted container (ConBee III is 0xDEC0DE03)", (unsigned)f->product);
        if (total != f->data_size) return why("The container says %u bytes, the header %u", (unsigned)total, (unsigned)f->data_size);
        if (total < 32 || img < 20 || img > total - 12) return "The encrypted container is malformed (image size)";
        f->device = GF_DEV_CONBEE3;
        f->platform = PLATFORM_MG21;
    } else if (f->type == 90) {  // FLASH_TYPE_APP_ENCRYPTED_2: CRC-32 after the header, not in the size
        f->proto = 3;
        if (size < GCF_HEADER_SIZE + 4) return "The file is cut short";
        if (io->image_read(io->ctx, GCF_HEADER_SIZE, h + GCF_HEADER_SIZE, 4) != 0) return "Reading the stored image failed";
        f->crc32 = rd32(h + GCF_HEADER_SIZE);
        f->data_offset = GCF_HEADER_SIZE + 4;
        if (name_plat != PLATFORM_MG21)
            return "GCF type 90 without a ConBee III version (0x....0900) in the file name: can't tell which stick it's for";
        f->device = GF_DEV_CONBEE3;
        f->platform = PLATFORM_MG21;
    } else if (f->type == 70) {
        return "Compressed GCF images (type 70) aren't supported";
    } else if (f->type == 80) {
        return "This GCF updates the bootloader itself (type 80): refusing";
    } else {
        return why("Unknown GCF file type %u", f->type);
    }

    if (name_plat && name_plat != f->platform)
        return why("The file name says platform 0x%02X but the header is %s firmware (0x%02X)", name_plat,
                   dev_name(f->device), f->platform);
    if (want != GF_DEV_UNKNOWN && f->device != want)
        return why("This file is for a %s (platform 0x%02X); the attached stick is a %s", dev_name(f->device), f->platform,
                   dev_name(want));
    if (f->data_size == 0) return "There's no firmware in the file";
    if (f->data_size != size - f->data_offset)
        return why("The file is cut short or has junk at the end (header: %u bytes of firmware, file: %u)",
                   (unsigned)f->data_size, (unsigned)(size - f->data_offset));

    if (f->type == 60) {
        // Checked against the published files: the header's CRC-8 (gcfflasher's CRC8_Dallas) covers the whole
        // container, and the container's last 4 bytes are a zlib CRC-32 over the rest of it.
        uint8_t buf[256], crc8 = 0, tail[4];
        uint32_t crc32 = 0, body = f->data_size - 4;
        for (uint32_t p = 0; p < f->data_size;) {
            uint32_t n = f->data_size - p < sizeof(buf) ? f->data_size - p : sizeof(buf);
            if (io->image_read(io->ctx, f->data_offset + p, buf, n) != 0) return "Reading the stored image failed";
            crc8 = gf_crc8(crc8, buf, n);
            if (p < body) crc32 = gf_crc32(crc32, buf, (body - p < n) ? body - p : n);
            for (uint32_t i = 0; i < n; i++)
                if (p + i >= body) tail[p + i - body] = buf[i];
            p += n;
        }
        if (crc8 != f->crc8) return "The header checksum (CRC-8) doesn't match the firmware: the file is damaged";
        if (crc32 != rd32(tail)) return "The container's CRC-32 doesn't match: the file is damaged";
        f->crc_checked = true;
    }
    return NULL;
}

// ---------------------------------------------------------------- the link: SLIP + text, one byte at a time

enum { W_UNKNOWN, W_APP, W_BL };

typedef struct {
    const gf_io_t *io;
    uint8_t in[64];
    size_t inpos, inlen;
    uint8_t sbuf[256];  // PROT_RxState; a finished packet stays here until the next byte
    size_t spos, pktlen;
    bool sesc, pkt_ready;
    char text[128];  // the last printable output (V1 bootloader talks ASCII)
    size_t tlen;
    uint8_t shift[6];  // the last 6 raw bytes: "GET" u16 ";"
    char banner[48];
    bool gone;
    uint8_t seq;
    int where;
    uint32_t reopens, crc_errors;
} link_t;

static void link_flush(link_t *l) {
    l->inpos = l->inlen = l->spos = l->tlen = 0;
    l->sesc = l->pkt_ready = false;
    l->text[0] = 0;
    memset(l->shift, 0, sizeof(l->shift));
}

static void link_init(link_t *l, const gf_io_t *io) {
    memset(l, 0, sizeof(*l));
    l->io = io;
    l->seq = 1;
}

static void text_clear(link_t *l) {
    l->tlen = 0;
    l->text[0] = 0;
    memset(l->shift, 0, sizeof(l->shift));
}

static void feed(link_t *l, uint8_t c) {
    memmove(l->shift, l->shift + 1, sizeof(l->shift) - 1);
    l->shift[sizeof(l->shift) - 1] = c;
    if (l->tlen >= sizeof(l->text) - 1) {
        size_t keep = sizeof(l->text) / 2;
        memmove(l->text, l->text + l->tlen - keep, keep);
        l->tlen = keep;
    }
    l->text[l->tlen++] = c ? (char)c : '.';
    l->text[l->tlen] = 0;

    // PROT_ReceiveFlagged(): 0xC0 ends a frame, 0xDB escapes, 16-bit two's-complement sum last (LE)
    l->pkt_ready = false;
    if (c == 0xC0) {
        if (!l->sesc && l->spos > 2) {
            uint16_t sum = 0;
            for (size_t i = 0; i < l->spos - 2; i++) sum = (uint16_t)(sum + l->sbuf[i]);
            uint16_t crc = (uint16_t)(~sum + 1);
            if (rd16(l->sbuf + l->spos - 2) == crc) {
                l->pktlen = l->spos - 2;
                l->pkt_ready = true;
            } else {
                l->crc_errors++;
            }
        }
        l->spos = 0;
        l->sesc = false;
        return;
    }
    if (c == 0xDB) {
        l->sesc = true;
        return;
    }
    if (l->sesc) {
        l->sesc = false;
        if (c == 0xDC) c = 0xC0;
        else if (c == 0xDD) c = 0xDB;
        else l->spos = 0;  // invalid
    }
    if (l->spos < sizeof(l->sbuf)) l->sbuf[l->spos++] = c;
    else l->spos = 0;
}

// One byte by the deadline: 1, 0 on timeout, -1 when the port is gone.
static int rx_byte(link_t *l, uint32_t deadline, uint8_t *out) {
    const gf_io_t *io = l->io;
    if (l->gone) return -1;
    while (l->inpos >= l->inlen) {
        int32_t rem = (int32_t)(deadline - now(io));
        if (rem <= 0) return 0;
        int n = io->read(io->ctx, l->in, sizeof(l->in), (uint32_t)rem);
        if (n < 0) {
            l->gone = true;
            return -1;
        }
        l->inpos = 0;
        l->inlen = (size_t)n;
    }
    *out = l->in[l->inpos++];
    feed(l, *out);
    return 1;
}

static int raw_write(link_t *l, const uint8_t *d, size_t n) {
    if (l->gone) return -1;
    if (l->io->write(l->io->ctx, d, n) != 0) {
        l->gone = true;
        return -1;
    }
    return 0;
}

typedef struct {  // PROT_SendFlagged(), streamed through a small buffer
    link_t *l;
    uint8_t out[96];
    size_t n;
    uint16_t sum;
    bool fail;
} slipw_t;

static void sw_raw(slipw_t *w, uint8_t b) {
    if (w->n == sizeof(w->out)) {
        if (raw_write(w->l, w->out, w->n) != 0) w->fail = true;
        w->n = 0;
    }
    w->out[w->n++] = b;
}
static void sw_esc(slipw_t *w, uint8_t b) {
    if (b == 0xC0) {
        sw_raw(w, 0xDB);
        sw_raw(w, 0xDC);
    } else if (b == 0xDB) {
        sw_raw(w, 0xDB);
        sw_raw(w, 0xDD);
    } else {
        sw_raw(w, b);
    }
}
static void sw_begin(slipw_t *w, link_t *l) {
    memset(w, 0, sizeof(*w));
    w->l = l;
    sw_raw(w, 0xC0);
}
static void sw_put(slipw_t *w, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) {
        w->sum = (uint16_t)(w->sum + d[i]);
        sw_esc(w, d[i]);
    }
}
static int sw_end(slipw_t *w) {
    uint16_t crc = (uint16_t)(~w->sum + 1);
    sw_esc(w, (uint8_t)crc);
    sw_esc(w, (uint8_t)(crc >> 8));
    sw_raw(w, 0xC0);
    if (w->n && raw_write(w->l, w->out, w->n) != 0) w->fail = true;
    w->n = 0;
    return w->fail ? -1 : 0;
}
static int slip_send(link_t *l, const uint8_t *d, size_t n) {
    slipw_t w;
    sw_begin(&w, l);
    sw_put(&w, d, n);
    return sw_end(&w);
}

static int send_version(link_t *l) {  // gcfCommandQueryFirmwareVersion()
    uint8_t c[9] = {CMD_FIRMWARE_VERSION, l->seq++, 0x00, 0x09, 0x00, 0, 0, 0, 0};
    return slip_send(l, c, sizeof(c));
}
static int send_watchdog(link_t *l) {  // gcfCommandResetUart(): watchdog timeout 2 s
    uint8_t c[12] = {CMD_WRITE_PARAMETER, l->seq++, 0x00, 0x0C, 0x00, 0x05, 0x00, PARAM_WATCHDOG_TIMEOUT, 0x02, 0, 0, 0};
    return slip_send(l, c, sizeof(c));
}
static int send_v3_id(link_t *l) {
    uint8_t c[2] = {BTL_MAGIC, BTL_ID_REQUEST};
    return slip_send(l, c, sizeof(c));
}

// What the stick said.
enum {
    E_TIMEOUT = 0,
    E_GONE = 1 << 0,
    E_VERSION = 1 << 1,  // deCONZ firmware version reply
    E_WDOG = 1 << 2,     // write-parameter (watchdog) reply
    E_BL1 = 1 << 3,      // V1 banner ("Bootloader" ... '\n')
    E_BL3 = 1 << 4,      // V3 ID response
    E_BTL = 1 << 5,      // any other V3 packet (0x81 ...)
    E_READY = 1 << 6,    // V1 "READY"
    E_GET = 1 << 7,      // V1 "GET" u16 ";"
    E_VALID = 1 << 8,    // V1 "#VALID CRC"
};

typedef struct {
    uint32_t version, btl, appcrc;
    bool wdog_ack;
} signs_t;

static void grab_banner(link_t *l, const char *at) {
    const char *s = at;
    while (s > l->text && s[-1] != '\n' && s[-1] != '\r') s--;
    size_t n = 0;
    while (s[n] && s[n] != '\r' && s[n] != '\n' && n < sizeof(l->banner) - 1) n++;
    memcpy(l->banner, s, n);
    l->banner[n] = 0;
}

static int watch_until(link_t *l, uint32_t deadline, unsigned want, signs_t *s) {
    uint8_t c;
    for (;;) {
        int rc = rx_byte(l, deadline, &c);
        if (rc < 0) return E_GONE;
        if (rc == 0) return E_TIMEOUT;
        if (l->pkt_ready) {
            const uint8_t *p = l->sbuf;
            size_t n = l->pktlen;
            if (p[0] == CMD_FIRMWARE_VERSION && n >= 9 && p[2] == 0x00) {  // PROT_Packet()
                s->version = rd32(p + 5);
                l->where = W_APP;
                if (want & E_VERSION) return E_VERSION;
            } else if (p[0] == CMD_WRITE_PARAMETER && n >= 8 && p[7] == PARAM_WATCHDOG_TIMEOUT) {
                s->wdog_ack = true;
                l->where = W_APP;
                if (want & E_WDOG) return E_WDOG;
            } else if (p[0] == BTL_MAGIC && n >= 10 && p[1] == BTL_ID_RESPONSE) {
                s->btl = rd32(p + 2);
                s->appcrc = rd32(p + 6);
                l->where = W_BL;
                if (want & E_BL3) return E_BL3;
            } else if (p[0] == BTL_MAGIC && n >= 2) {
                l->where = W_BL;
                if (want & E_BTL) return E_BTL;
            }
        }
        if (c == '\n' && (want & E_BL1)) {
            const char *b = strstr(l->text, "Bootloader");
            if (b) {
                grab_banner(l, b);
                text_clear(l);
                l->where = W_BL;
                return E_BL1;
            }
        }
        if ((want & E_READY) && strstr(l->text, "READY")) {
            text_clear(l);
            return E_READY;
        }
        if ((want & E_VALID) && strstr(l->text, "#VALID CRC")) {
            text_clear(l);
            return E_VALID;
        }
        if ((want & E_GET) && l->shift[0] == 'G' && l->shift[1] == 'E' && l->shift[2] == 'T' && l->shift[5] == ';') return E_GET;
    }
}

static int watch(link_t *l, uint32_t ms, unsigned want, signs_t *s) { return watch_until(l, now(l->io) + ms, want, s); }

bool gf_app_version(const gf_io_t *io, uint32_t *version) {
    link_t l;
    link_init(&l, io);
    signs_t s = {0};
    if (send_version(&l) != 0) return false;
    if (watch(&l, T_PROBE, E_VERSION, &s) != E_VERSION) return false;
    *version = s.version;
    return true;
}

// ---------------------------------------------------------------- the job

typedef enum { R_OK, R_RETRY, R_FATAL } res_t;

typedef struct {
    const gf_io_t *io;
    link_t l;
    gf_file_t f;
    gf_result_t *r;
    char *err;
    size_t el;
} job_t;

// The port vanished: wait for the stick to enumerate again.
static bool ride_reopen(job_t *j, const char *when) {
    const gf_io_t *io = j->io;
    if (io->reopen == NULL) return false;
    logf_(io, "%s: the stick dropped off USB, waiting for it to come back", when);
    if (io->reopen(io->ctx, T_REOPEN) != 0) return false;
    if (io->set_baud) io->set_baud(io->ctx, GF_BAUD);
    link_flush(&j->l);
    j->l.gone = false;
    j->r->reopens++;
    return true;
}

// Waits for the bootloader the file needs to show itself: V1 is asked with "ID" and answers with its banner,
// V3 with the 0x81 0x02 ID request. Three windows, the first only listening (ST_BootloaderQuery).
static res_t query_bootloader(job_t *j, signs_t *s) {
    link_t *l = &j->l;
    bool v1 = j->f.proto == 1;
    int reopened = 0;
    for (int w = 0; w < ID_WINDOWS; w++) {
        if (w > 0) {
            if (v1) {
                logf_(j->io, "asking the V1 bootloader for its ID");
                raw_write(l, (const uint8_t *)"ID", 2);
            } else {
                logf_(j->io, "asking the V3 bootloader for its ID");
                send_v3_id(l);
            }
        }
        int ev = watch(l, v1 ? T_V1_ID : T_V3_ID, v1 ? E_BL1 : E_BL3, s);
        if (ev == E_BL1 || ev == E_BL3) return R_OK;
        if (ev == E_GONE) {
            if (++reopened > 3 || !ride_reopen(j, "bootloader query")) {
                seterr(j->err, j->el, "The stick went away and didn't come back");
                return R_FATAL;
            }
            w = -1;  // a fresh port: start listening again
        }
    }
    return R_RETRY;
}

static res_t enter_bootloader(job_t *j) {
    const gf_io_t *io = j->io;
    link_t *l = &j->l;
    gf_result_t *r = j->r;
    signs_t s = {0};
    unsigned bl = j->f.proto == 1 ? E_BL1 : E_BL3;
    bool app = false;
    r->entered = NULL;
    text_clear(l);

    int ev = E_GONE;
    for (int k = 0; k < 2 && ev == E_GONE; k++) {
        if (l->gone && !ride_reopen(j, "start")) break;
        ev = send_version(l) == 0 ? watch(l, T_PROBE, E_VERSION | E_BL1 | E_BL3, &s) : E_GONE;
    }
    if (ev == E_GONE) {
        seterr(j->err, j->el, "The stick went away and didn't come back");
        return R_FATAL;
    }
    if (ev == E_BL1 || ev == E_BL3) {
        if (ev != (int)bl) {
            seterr(j->err, j->el, "The stick's bootloader speaks %s, but the file is for a %s", ev == E_BL1 ? "V1" : "V3",
                   dev_name(j->f.device));
            return R_FATAL;
        }
        r->entered = "already in the bootloader";
        if (ev == E_BL3) r->bootloader = s.btl;
        return R_OK;
    }
    if (ev == E_VERSION) {
        app = true;
        if (r->before == 0) r->before = s.version;
        uint8_t plat = (uint8_t)(s.version >> 8);
        if (plat != j->f.platform) {
            seterr(j->err, j->el, "The stick runs firmware 0x%08X (platform 0x%02X), not %s firmware (0x%02X): refusing",
                   (unsigned)s.version, plat, dev_name(j->f.device), j->f.platform);
            return R_FATAL;
        }
        logf_(io, "firmware 0x%08X answers; telling it to stop feeding its watchdog (2 s)", (unsigned)s.version);
        send_watchdog(l);
        ev = watch(l, T_RESET, E_BL1 | E_BL3, &s);  // ST_ResetUart: disconnect, ID response or timeout
        if (ev == E_GONE) {
            if (!ride_reopen(j, "watchdog reset")) {
                seterr(j->err, j->el, "The stick dropped off USB for its reset and didn't come back within %u s", T_REOPEN / 1000);
                return R_FATAL;
            }
            r->entered = "watchdog reset (USB re-enumerated)";
        } else {
            r->entered = "watchdog reset";
            if (!s.wdog_ack) logf_(io, "no reply to the watchdog request");
            if (ev == (int)bl) {
                if (ev == E_BL3) r->bootloader = s.btl;
                return R_OK;
            }
        }
    }

    res_t q = query_bootloader(j, &s);
    if (q == R_OK) {
        if (r->entered == NULL) r->entered = "already in the bootloader";
        if (j->f.proto == 3) r->bootloader = s.btl;
        return R_OK;
    }
    if (q == R_FATAL) return q;
    // Nothing that looks like the bootloader. Is the old firmware still there?
    if (send_version(l) == 0 && watch(l, T_PROBE, E_VERSION, &s) == E_VERSION) {
        seterr(j->err, j->el, "The stick never entered its bootloader (firmware 0x%08X still running)", (unsigned)s.version);
    } else {
        seterr(j->err, j->el, app ? "The stick never entered its bootloader" : "Nothing answers: neither deCONZ firmware nor the bootloader");
    }
    return R_RETRY;
}

static res_t gone_mid_upload(job_t *j, const char *what) {
    if (!ride_reopen(j, what)) {
        seterr(j->err, j->el, "The stick went away during the %s and didn't come back", what);
        return R_FATAL;
    }
    seterr(j->err, j->el, "The stick went away during the %s", what);
    return R_RETRY;
}

// ST_V1ProgramSync / WriteHeader / Upload / Validate
static res_t v1_upload(job_t *j) {
    const gf_io_t *io = j->io;
    link_t *l = &j->l;
    const gf_file_t *f = &j->f;
    signs_t s = {0};
    if (l->banner[0]) {
        memcpy(j->r->banner, l->banner, sizeof(j->r->banner));
        logf_(io, "bootloader: %s", l->banner);
    }
    text_clear(l);
    static const uint8_t sync[4] = {0x1A, 0x1C, 0xA9, 0xAE};
    if (raw_write(l, sync, sizeof(sync)) != 0) return gone_mid_upload(j, "bootloader sync");
    int ev = watch(l, T_V1_SYNC, E_READY, &s);
    if (ev == E_GONE) return gone_mid_upload(j, "bootloader sync");
    if (ev != E_READY) {
        seterr(j->err, j->el, "The bootloader didn't answer the sync (no READY)");
        return R_RETRY;
    }
    // u32 size, u32 target address, u8 file type, u8 header checksum (LE)
    uint8_t h[10], *p = h;
    p = wr32(p, f->data_size);
    p = wr32(p, f->target);
    *p++ = f->type;
    *p++ = f->crc8;
    if (raw_write(l, h, sizeof(h)) != 0) return gone_mid_upload(j, "upload");

    uint32_t pages = (f->data_size + V1_PAGESIZE - 1) / V1_PAGESIZE, next = 0, served = 0;
    bool last_sent = false, any = false;
    uint8_t page[V1_PAGESIZE];
    for (;;) {
        ev = watch(l, last_sent ? T_V1_COMMIT : any ? T_V1_PAGE : T_V1_FIRST, E_GET | E_VALID, &s);
        if (ev == E_GET) {
            uint32_t pg = (uint32_t)l->shift[3] | ((uint32_t)l->shift[4] << 8);
            memset(l->shift, 0, sizeof(l->shift));
            if (pg >= pages) {
                seterr(j->err, j->el, "The bootloader asked for page %u, past the end (%u pages)", (unsigned)pg, (unsigned)pages);
                return R_RETRY;
            }
            uint32_t off = pg * V1_PAGESIZE, n = f->data_size - off < V1_PAGESIZE ? f->data_size - off : V1_PAGESIZE;
            if (io->image_read(io->ctx, f->data_offset + off, page, n) != 0) {
                seterr(j->err, j->el, "Reading the stored image failed");
                return R_FATAL;
            }
            if (raw_write(l, page, n) != 0) return gone_mid_upload(j, "upload");
            any = true;
            if (pg < next) j->r->resent++;
            if (pg + 1 > next) next = pg + 1;
            if (off + n > served) served = off + n;
            if (io->progress && (pg % 20 == 0 || off + n == f->data_size)) io->progress(io->ctx, "upload", served, f->data_size);
            if (off + n == f->data_size && !last_sent) {
                last_sent = true;
                logf_(io, "all %u pages sent, waiting for the bootloader to commit (up to %u s)", (unsigned)pages, T_V1_COMMIT / 1000);
            }
        } else if (ev == E_VALID) {
            if (last_sent) {
                logf_(io, "bootloader: #VALID CRC");
                return R_OK;
            }
        } else if (ev == E_GONE) {
            if (last_sent) {  // ST_V1ProgramValidate: the chip resets after committing, tearing the port down
                logf_(io, "the stick reset before its verdict (gcfflasher counts that as written)");
                return R_OK;
            }
            return gone_mid_upload(j, "upload");
        } else {
            if (last_sent) seterr(j->err, j->el, "No \"#VALID CRC\" from the bootloader within %u s of the last page", T_V1_COMMIT / 1000);
            else if (!any) seterr(j->err, j->el, "The bootloader didn't ask for the first page");
            else seterr(j->err, j->el, "The bootloader stopped asking for pages at %u%%", (unsigned)((uint64_t)served * 100 / f->data_size));
            return R_RETRY;
        }
    }
}

// ST_V3ProgramSync / Upload / WaitID
static res_t v3_upload(job_t *j) {
    const gf_io_t *io = j->io;
    link_t *l = &j->l;
    const gf_file_t *f = &j->f;
    gf_result_t *r = j->r;
    signs_t s = {0};
    logf_(io, "V3 bootloader 0x%08X", (unsigned)r->bootloader);
    io->sleep_ms(io->ctx, T_V3_SETTLE);
    uint8_t cmd[15], *p = cmd;
    *p++ = BTL_MAGIC;
    *p++ = BTL_FW_UPDATE_REQUEST;
    p = wr32(p, f->data_size);
    p = wr32(p, f->target);
    *p++ = f->type;
    p = wr32(p, f->crc32);
    if (slip_send(l, cmd, sizeof(cmd)) != 0) return gone_mid_upload(j, "update request");
    uint32_t deadline = now(io) + T_V3_UPDATE;
    for (;;) {
        int ev = watch_until(l, deadline, E_BTL, &s);
        if (ev == E_GONE) return gone_mid_upload(j, "update request");
        if (ev == E_TIMEOUT) {
            seterr(j->err, j->el, "The bootloader didn't answer the update request");
            return R_RETRY;
        }
        if (l->sbuf[1] == BTL_FW_UPDATE_RESPONSE && l->pktlen >= 3) {
            if (l->sbuf[2] != 0x00) {
                seterr(j->err, j->el, "The bootloader refused the update (status %u)", l->sbuf[2]);
                return R_FATAL;
            }
            break;
        }
    }

    uint32_t next = 0;
    bool last_sent = false;
    deadline = now(io) + T_V3_FIRST;
    for (;;) {
        int ev = watch_until(l, deadline, E_BTL | E_BL3, &s);
        if (ev == E_BTL && l->sbuf[1] == BTL_FW_DATA_REQUEST && l->pktlen == 8) {
            uint32_t off = rd32(l->sbuf + 2);
            uint16_t len = rd16(l->sbuf + 6);
            uint8_t status = 0;
            if ((uint64_t)off + len > f->data_size) status = 1;
            else if (len > V3_MAX_CHUNK) status = 2;
            else if (len == 0) status = 3;
            // 0x81 0x84 u8 status, u32 offset, u16 length, data (status 0 only)
            uint8_t hd[9], *q = hd;
            *q++ = BTL_MAGIC;
            *q++ = BTL_FW_DATA_RESPONSE;
            *q++ = status;
            q = wr32(q, off);
            q = wr16(q, len);
            slipw_t w;
            sw_begin(&w, l);
            sw_put(&w, hd, sizeof(hd));
            if (status == 0) {
                uint8_t buf[64];
                for (uint32_t k = 0; k < len;) {
                    uint32_t n = len - k < sizeof(buf) ? len - k : sizeof(buf);
                    if (io->image_read(io->ctx, f->data_offset + off + k, buf, n) != 0) {
                        seterr(j->err, j->el, "Reading the stored image failed");
                        return R_FATAL;
                    }
                    sw_put(&w, buf, n);
                    k += n;
                }
            }
            if (sw_end(&w) != 0) return gone_mid_upload(j, "upload");
            if (status) {
                logf_(io, "data request %u+%u answered with status %u", (unsigned)off, len, status);
            } else {
                if (off < next) r->resent++;
                if (off + len > next) next = off + len;
                if (io->progress) io->progress(io->ctx, "upload", next, f->data_size);
                if (off + len == f->data_size && !last_sent) {
                    last_sent = true;
                    logf_(io, "all data sent, waiting (up to %u s) for the bootloader's verdict", T_V3_VERIFY / 1000);
                }
            }
            deadline = now(io) + (last_sent ? T_V3_VERIFY : T_V3_DATA);
        } else if (ev == E_BL3) {
            if (last_sent) {
                r->bootloader = s.btl;
                r->app_crc = s.appcrc;
                r->app_crc_ok = s.appcrc == f->crc32;
                logf_(io, "app checksum 0x%08X %s", (unsigned)s.appcrc, r->app_crc_ok ? "(OK)" : "(doesn't match the file's)");
                return R_OK;
            }
        } else if (ev == E_GONE) {
            return gone_mid_upload(j, "upload");
        } else if (ev == E_TIMEOUT) {
            if (last_sent) seterr(j->err, j->el, "The bootloader didn't confirm the image within %u s", T_V3_VERIFY / 1000);
            else seterr(j->err, j->el, "The bootloader stopped asking for data at %u%%", (unsigned)((uint64_t)next * 100 / f->data_size));
            return R_RETRY;
        }
    }
}

// The new firmware has to answer a VERSION request; a ConBee II re-enumerates on the way.
static bool confirm_app(job_t *j) {
    const gf_io_t *io = j->io;
    link_t *l = &j->l;
    signs_t s = {0};
    uint32_t deadline = now(io) + T_APP_BACK;
    while (!past(io, deadline)) {
        if (l->gone && !ride_reopen(j, "restart")) {
            seterr(j->err, j->el, "The stick dropped off USB for its restart and didn't come back");
            return false;
        }
        if (send_version(l) != 0) continue;
        if (watch(l, T_PROBE, E_VERSION, &s) == E_VERSION) {
            j->r->after = s.version;
            return true;
        }
    }
    seterr(j->err, j->el, "The new firmware doesn't answer");
    return false;
}

static void fail_flags(job_t *j) {
    j->r->in_bootloader = j->l.where == W_BL;
    j->r->app_left_running = j->l.where == W_APP;
}

bool gf_flash(const gf_io_t *io, const gf_flash_opts_t *o, gf_result_t *r, char *err, size_t el) {
    memset(r, 0, sizeof(*r));
    if (el) err[0] = 0;
    job_t j = {.io = io, .r = r, .err = err, .el = el};
    const char *bad = gf_file_check(io, o->size, o->file_name, o->device, &j.f);
    if (bad) {
        seterr(err, el, "%s", bad);
        return false;
    }
    r->expected = j.f.name_version;
    logf_(io, "%s firmware 0x%08X, GCF type %u, %u bytes at 0x%X, V%u bootloader", dev_name(j.f.device),
          (unsigned)j.f.name_version, j.f.type, (unsigned)j.f.data_size, (unsigned)j.f.target, j.f.proto);
    link_init(&j.l, io);
    if (io->set_baud && io->set_baud(io->ctx, GF_BAUD) != 0) {
        seterr(err, el, "Couldn't open the stick at %u baud", GF_BAUD);
        return false;
    }
    uint32_t budget = o->budget_ms ? o->budget_ms : DEFAULT_BUDGET, start = now(io);
    for (;;) {
        r->attempts++;
        res_t x = enter_bootloader(&j);
        if (x == R_OK) {
            j.l.where = W_BL;
            logf_(io, "in the bootloader (%s)", r->entered);
            x = j.f.proto == 1 ? v1_upload(&j) : v3_upload(&j);
        }
        if (x == R_OK) break;
        uint32_t spent = now(io) - start;
        if (x == R_FATAL || spent >= budget || r->attempts >= MAX_ATTEMPTS) {
            fail_flags(&j);
            return false;
        }
        logf_(io, "attempt %u failed: %s; retrying (%u s left)", (unsigned)r->attempts, err, (unsigned)((budget - spent) / 1000));
        io->sleep_ms(io->ctx, T_RETRY_PAUSE);
    }
    if (el) err[0] = 0;
    if (!confirm_app(&j)) {
        fail_flags(&j);
        return false;
    }
    if (r->expected && r->after != r->expected) {
        seterr(err, el, "The stick answers with firmware 0x%08X, not the file's 0x%08X", (unsigned)r->after, (unsigned)r->expected);
        r->app_left_running = true;
        return false;
    }
    logf_(io, "done: firmware 0x%08X -> 0x%08X", (unsigned)r->before, (unsigned)r->after);
    return true;
}

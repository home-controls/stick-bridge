#include "tiflash_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// ROM bootloader commands (cc2538_bsl.py CommandInterface; TI CC13x2/CC26x2 TRM, "Bootloader")
#define CMD_PING 0x20
#define CMD_DOWNLOAD 0x21
#define CMD_GET_STATUS 0x23
#define CMD_SEND_DATA 0x24
#define CMD_RESET 0x25
#define CMD_SECTOR_ERASE 0x26
#define CMD_CRC32 0x27
#define CMD_GET_CHIP_ID 0x28
#define CMD_MEMORY_READ 0x2A
#define CMD_BANK_ERASE 0x2C

#define RET_SUCCESS 0x40
#define RET_UNKNOWN_CMD 0x41
#define RET_INVALID_CMD 0x42
#define RET_INVALID_ADR 0x43
#define RET_FLASH_FAIL 0x44

#define ACK 0xCC
#define NACK 0x33

// Registers read with MEMORY_READ to identify the chip (cc2538_bsl.py CC26xx.__init__)
#define ICEPICK_DEVICE_ID 0x50001318u
#define FLASH_SIZE_REG 0x4003002Cu

#define TRIES 5            // per command
#define T_ACK 1000         // ms to wait for ACK/NACK of an ordinary command
#define T_DOWNLOAD 2000
#define T_SEND_DATA 3000
#define T_ERASE_BANK 10000 // cc2538-bsl waits 10 s too
#define T_ERASE_SECTOR 3000
#define T_CRC 5000
#define T_PACKET 1000

// Negative results from the command layer
#define E_NOANSWER (-1)
#define E_GONE (-2)

// ---------------------------------------------------------------- small helpers

static void logf_(const tf_io_t *io, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(const tf_io_t *io, const char *fmt, ...) {
    if (io->log == NULL) return;
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    io->log(io->ctx, b);
}

static void progress(const tf_io_t *io, const char *step, uint32_t done, uint32_t total) {
    if (io->progress) io->progress(io->ctx, step, done, total);
}

static void seterr(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void seterr(char *err, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static inline uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline void wr32be(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t left(const tf_io_t *io, uint32_t start, uint32_t ms) {
    uint32_t e = io->now_ms(io->ctx) - start;
    return e >= ms ? 0 : ms - e;
}

static void drain(const tf_io_t *io) {
    uint8_t b[64];
    for (int i = 0; i < 64 && io->read(io->ctx, b, sizeof(b), 20) > 0; i++) {
    }
}

uint32_t tf_crc32(uint32_t crc, const uint8_t *d, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

static const char *status_name(int st) {
    switch (st) {
        case RET_SUCCESS: return "success";
        case RET_UNKNOWN_CMD: return "unknown command";
        case RET_INVALID_CMD: return "invalid command";
        case RET_INVALID_ADR: return "invalid address";
        case RET_FLASH_FAIL: return "flash fail";
        case E_NOANSWER: return "no answer";
        case E_GONE: return "stick gone";
        default: return "unexpected status";
    }
}

static bool all_ff(const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (d[i] != 0xFF) return false;
    return true;
}

// ---------------------------------------------------------------- image check

const char *tf_image_check(const tf_io_t *io, uint32_t len, uint32_t chip_flash, tf_image_t *g) {
    static char why[192];
    memset(g, 0, sizeof(*g));
    if (chip_flash) {
        if (chip_flash != TF_FLASH_352K && chip_flash != TF_FLASH_704K) {
            snprintf(why, sizeof(why), "Unsupported chip flash size %lu (only 352 KB and 704 KB parts)", (unsigned long)chip_flash);
            return why;
        }
        if (len != chip_flash) {
            snprintf(why, sizeof(why), "The image is %lu bytes but this chip's flash is %lu: wrong firmware for this stick",
                     (unsigned long)len, (unsigned long)chip_flash);
            return why;
        }
    } else if (len != TF_FLASH_352K && len != TF_FLASH_704K) {
        snprintf(why, sizeof(why),
                 "The image is %lu bytes; a CC2652/CC1352 image covers the whole flash (360448 or 720896 bytes, CCFG included)",
                 (unsigned long)len);
        return why;
    }
    g->flash_size = len;

    uint8_t vt[8];
    if (io->image_read(io->ctx, 0, vt, sizeof(vt)) != 0) return "Reading the stored image failed";
    uint32_t sp = rd32le(vt), rv = rd32le(vt + 4);
    if (sp < 0x20000000u || sp > 0x20040000u || (rv & 1) == 0 || rv >= len)
        return "The image doesn't start with a Cortex-M vector table at address 0 (not a flat CC26x2 image?)";

    uint8_t c[TF_CCFG_LEN];
    if (io->image_read(io->ctx, len - TF_CCFG_LEN, c, sizeof(c)) != 0) return "Reading the stored image failed";
    uint32_t bl = rd32le(c + TF_CCFG_BL_CONFIG);
    g->bl_config = bl;
    g->bl_pin = (uint8_t)(bl >> 8);
    g->bl_level = (uint8_t)((bl >> 16) & 1);
    g->erase_conf = rd32le(c + TF_CCFG_ERASE_CONF);
    g->image_valid = rd32le(c + TF_CCFG_IMAGE_VALID);
    if ((bl >> 24) != 0xC5) {
        snprintf(why, sizeof(why),
                 "The image disables the ROM bootloader (CCFG BL_CONFIG 0x%08lx): the stick couldn't be flashed again without a debugger",
                 (unsigned long)bl);
        return why;
    }
    if ((bl & 0xFF) != 0xC5) {
        snprintf(why, sizeof(why),
                 "The image disables the bootloader backdoor (CCFG BL_CONFIG 0x%08lx): the stick couldn't be flashed again without a debugger",
                 (unsigned long)bl);
        return why;
    }
    if (g->bl_pin > 30) {
        snprintf(why, sizeof(why), "The image's bootloader backdoor watches DIO%u, which doesn't exist (CCFG BL_CONFIG 0x%08lx)",
                 g->bl_pin, (unsigned long)bl);
        return why;
    }
    if ((g->erase_conf & 0x1) == 0) return "The image disables bank erase (CCFG ERASE_CONF): the bootloader couldn't erase it again";
    if ((g->erase_conf & 0x100) == 0) return "The image disables chip erase (CCFG ERASE_CONF): a debugger couldn't recover the stick";
    uint32_t sectors = len / TF_SECTOR;
    for (uint32_t s = 0; s < sectors; s++) {
        uint32_t w = rd32le(c + TF_CCFG_PROT + (s / 32) * 4);
        if ((w & (1u << (s % 32))) == 0) {
            snprintf(why, sizeof(why), "The image write-protects flash sector %lu (CCFG_PROT): it couldn't be erased again",
                     (unsigned long)s);
            return why;
        }
    }

    // One pass over the whole image: CRC-32 (what the chip's CRC32 must say) and the 0xFF chunks.
    uint8_t buf[TF_CHUNK];
    uint32_t crc = 0;
    for (uint32_t off = 0; off < len; off += TF_CHUNK) {
        uint32_t n = len - off < TF_CHUNK ? len - off : TF_CHUNK;
        if (io->image_read(io->ctx, off, buf, n) != 0) return "Reading the stored image failed";
        crc = tf_crc32(crc, buf, n);
        if (all_ff(buf, n)) g->chunks_ff++;
    }
    g->crc32 = crc;
    return NULL;
}

// ---------------------------------------------------------------- bootloader entry (cc2538_bsl.py invoke_bootloader)

typedef struct {
    bool dtr, rts;
} lines_t;

// pyserial changes one line per call; so do we, so the gated SONOFF circuit sees the same intermediate
// states the script produces.
static void set_pin(const tf_io_t *io, lines_t *l, bool use_rts, bool v) {
    if (use_rts) l->rts = v;
    else l->dtr = v;
    io->set_lines(io->ctx, l->dtr, l->rts);
}

static void enter_bsl(const tf_io_t *io, const tf_opts_t *o) {
    // Lines start as pyserial leaves them right after open(): both asserted.
    lines_t l = {.dtr = true, .rts = true};
    bool bsl_rts = o->invert_lines;   // set_bootloader_pin = setRTS when inverted, else setDTR
    bool rst_rts = !o->invert_lines;  // set_reset_pin = setDTR when inverted, else setRTS
    if (o->entry == TF_ENTRY_SONOFF) {
        set_pin(io, &l, bsl_rts, false);
        set_pin(io, &l, rst_rts, true);
        set_pin(io, &l, bsl_rts, true);
        set_pin(io, &l, rst_rts, false);
        io->sleep_ms(io->ctx, 2);
        set_pin(io, &l, rst_rts, true);
    } else if (o->entry == TF_ENTRY_DEFAULT) {
        set_pin(io, &l, bsl_rts, !o->active_high);
        set_pin(io, &l, rst_rts, false);
        set_pin(io, &l, rst_rts, true);
        set_pin(io, &l, rst_rts, false);
        io->sleep_ms(io->ctx, 2);
        set_pin(io, &l, bsl_rts, o->active_high);
    } else {
        return;
    }
    io->sleep_ms(io->ctx, 100);  // the script's settle time (contiki-os/contiki#1533)
}

// ---------------------------------------------------------------- packets

static int write_all(const tf_io_t *io, const uint8_t *d, size_t n) { return io->write(io->ctx, d, n) == 0 ? 0 : E_GONE; }

static int send_packet(const tf_io_t *io, uint8_t cmd, const uint8_t *payload, size_t plen) {
    uint8_t p[3 + TF_CHUNK + 4];
    if (plen > sizeof(p) - 3) return E_GONE;
    uint8_t sum = cmd;
    for (size_t i = 0; i < plen; i++) sum = (uint8_t)(sum + payload[i]);
    p[0] = (uint8_t)(plen + 3);
    p[1] = sum;
    p[2] = cmd;
    if (plen) memcpy(p + 3, payload, plen);
    return write_all(io, p, plen + 3);
}

// 00 CC = 1, 00 33 = 0, else E_NOANSWER / E_GONE. Bytes before the pair are skipped (as _wait_for_ack).
// Reads one byte at a time so nothing after the pair (a response packet) is consumed.
static int wait_ack(const tf_io_t *io, uint32_t timeout_ms) {
    uint32_t t0 = io->now_ms(io->ctx);
    uint8_t prev = 0xFF;
    for (;;) {
        uint32_t w = left(io, t0, timeout_ms);
        if (w == 0) return E_NOANSWER;
        uint8_t b;
        int r = io->read(io->ctx, &b, 1, w);
        if (r < 0) return E_GONE;
        if (r == 0) continue;
        if (prev == 0x00 && b == ACK) return 1;
        if (prev == 0x00 && b == NACK) return 0;
        prev = b;
    }
}

static int read_exact(const tf_io_t *io, uint8_t *d, size_t n, uint32_t timeout_ms) {
    uint32_t t0 = io->now_ms(io->ctx);
    size_t got = 0;
    while (got < n) {
        uint32_t w = left(io, t0, timeout_ms);
        if (w == 0) return E_NOANSWER;
        int r = io->read(io->ctx, d + got, n - got, w);
        if (r < 0) return E_GONE;
        got += (size_t)r;
    }
    return 0;
}

// A response packet [size][checksum][data]; ACKs or NACKs it. Data length, or E_NOANSWER (timeout or bad
// checksum: the caller asks again) / E_GONE.
static int recv_packet(const tf_io_t *io, uint8_t *out, size_t max) {
    uint32_t t0 = io->now_ms(io->ctx);
    uint8_t size = 0, chk = 0;
    for (;;) {  // zero bytes before the size are padding
        uint32_t w = left(io, t0, T_PACKET);
        if (w == 0) return E_NOANSWER;
        int r = io->read(io->ctx, &size, 1, w);
        if (r < 0) return E_GONE;
        if (r == 1 && size != 0) break;
    }
    if (size < 3) return E_NOANSWER;
    int r = read_exact(io, &chk, 1, T_PACKET);
    if (r < 0) return r;
    size_t n = (size_t)size - 2;
    uint8_t d[253];
    r = read_exact(io, d, n, T_PACKET);
    if (r < 0) return r;
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) sum = (uint8_t)(sum + d[i]);
    static const uint8_t ack[2] = {0x00, ACK}, nack[2] = {0x00, NACK};
    if (sum != chk || n > max) {
        if (write_all(io, nack, 2) < 0) return E_GONE;
        return E_NOANSWER;
    }
    if (write_all(io, ack, 2) < 0) return E_GONE;
    memcpy(out, d, n);
    return (int)n;
}

// After a lost byte the bootloader sits in the middle of a packet waiting for the rest. Feed it zeros
// until it completes the packet and NACKs it (the checksum won't match). Zeros while it's idle are taken
// as padding. 1 = it answered (in sync again), 0 = silence, E_GONE.
static int resync(const tf_io_t *io) {
    static const uint8_t z[16] = {0};
    for (int i = 0; i < 18; i++) {
        if (write_all(io, z, sizeof(z)) < 0) return E_GONE;
        int a = wait_ack(io, 20);
        if (a == E_GONE) return E_GONE;
        if (a >= 0) {
            drain(io);
            return 1;
        }
    }
    return 0;
}

static int get_status(const tf_io_t *io, tf_result_t *r) {
    for (int t = 0; t < TRIES; t++) {
        if (t) r->retries++;
        if (send_packet(io, CMD_GET_STATUS, NULL, 0) < 0) return E_GONE;
        int a = wait_ack(io, T_ACK);
        if (a == E_GONE) return E_GONE;
        if (a == 0) continue;
        if (a < 0) {
            if (resync(io) == E_GONE) return E_GONE;
            continue;
        }
        uint8_t s[4];
        int n = recv_packet(io, s, sizeof(s));
        if (n == E_GONE) return E_GONE;
        if (n == 1) return s[0];
        drain(io);
    }
    return E_NOANSWER;
}

// A command that is safe to send again (everything but SEND_DATA). Optional response packet of exactly
// rlen bytes, then GET_STATUS. The bootloader's status (RET_*), or E_NOANSWER / E_GONE.
static int command(const tf_io_t *io, tf_result_t *r, uint8_t cmd, const uint8_t *payload, size_t plen, uint32_t ack_ms,
                   uint8_t *resp, size_t rlen, bool status) {
    for (int t = 0; t < TRIES; t++) {
        if (t) r->retries++;
        if (send_packet(io, cmd, payload, plen) < 0) return E_GONE;
        int a = wait_ack(io, ack_ms);
        if (a == E_GONE) return E_GONE;
        if (a == 0) continue;  // NACK: it threw the packet away, send it again
        if (a < 0) {
            if (resync(io) == E_GONE) return E_GONE;
            continue;
        }
        if (resp) {
            int n = recv_packet(io, resp, rlen);
            if (n == E_GONE) return E_GONE;
            if (n != (int)rlen) {
                drain(io);
                continue;
            }
        }
        if (!status) return RET_SUCCESS;
        int st = get_status(io, r);
        if (st == E_NOANSWER) continue;
        return st;
    }
    return E_NOANSWER;
}

static int mem_read32(const tf_io_t *io, tf_result_t *r, uint32_t addr, uint32_t *v) {
    uint8_t p[6], d[4];
    wr32be(p, addr);
    p[4] = 1;  // access width: 32-bit
    p[5] = 1;  // one read
    int st = command(io, r, CMD_MEMORY_READ, p, sizeof(p), T_ACK, d, sizeof(d), true);
    if (st == RET_SUCCESS) *v = rd32le(d);  // returned in memory order
    return st;
}

// ---------------------------------------------------------------- sync

// 0x55 0x55, the ROM measures the bit time from it (autobaud) and answers 00 CC. If it was synced
// already (an earlier run left it in the bootloader) it takes the 0x55s as the start of a packet; the
// zero padding in resync() makes it NACK that, which proves it's listening too.
static bool sync_bsl(const tf_io_t *io, tf_result_t *r, uint32_t baud) {
    if (io->set_baud(io->ctx, baud) != 0) return false;
    drain(io);
    int a = E_NOANSWER;
    // Only 0x55s until it has had three chances: anything else before the autobaud locks could make it
    // lock at a wrong rate.
    for (int t = 0; t < 3; t++) {
        static const uint8_t s[2] = {0x55, 0x55};
        if (write_all(io, s, 2) < 0) return false;
        a = wait_ack(io, 500);
        if (a == E_GONE) return false;
        if (a == 1) return true;
        if (a == 0) break;  // a NACK: it's synced already and took the 0x55s for a packet
    }
    if (a == 0 || resync(io) == 1) {  // in the bootloader already (mid-packet): make sure with a PING
        if (command(io, r, CMD_PING, NULL, 0, T_ACK, NULL, 0, true) == RET_SUCCESS) return true;
    }
    return false;
}

static const char *entry_name(const tf_opts_t *o) {
    switch (o->entry) {
        case TF_ENTRY_SONOFF: return o->invert_lines ? "SONOFF DTR/RTS pattern (lines inverted)" : "SONOFF DTR/RTS pattern";
        case TF_ENTRY_DEFAULT:
            if (o->invert_lines) return o->active_high ? "DTR/RTS reset (inverted, active high)" : "DTR/RTS reset (inverted)";
            return o->active_high ? "DTR/RTS reset (active high)" : "DTR/RTS reset";
        default: return "already in the bootloader";
    }
}

// ---------------------------------------------------------------- erase / write / verify

static bool erase(const tf_io_t *io, const tf_opts_t *o, tf_result_t *r, uint32_t len, char *err, size_t el) {
    bool sector = o->sector_erase;
    if (!sector && r->device_info_ok && (r->device_erase_conf & 1) == 0) {
        logf_(io, "The chip's current CCFG disables bank erase: erasing sector by sector");
        sector = true;
    }
    if (!sector) {
        progress(io, "erase", 0, len);
        r->erase_how = "bank erase";
        r->erased = true;
        int st = command(io, r, CMD_BANK_ERASE, NULL, 0, T_ERASE_BANK, NULL, 0, true);
        if (st == RET_SUCCESS) {
            progress(io, "erase", len, len);
            return true;
        }
        if (st == E_GONE) {
            seterr(err, el, "The stick went away during the erase");
            return false;
        }
        logf_(io, "Bank erase failed (%s): erasing sector by sector", status_name(st));
    }
    r->erase_how = "sector erase";
    for (uint32_t a = 0; a < len; a += TF_SECTOR) {
        uint8_t p[4];
        wr32be(p, a);
        r->erased = true;
        int st = command(io, r, CMD_SECTOR_ERASE, p, sizeof(p), T_ERASE_SECTOR, NULL, 0, true);
        if (st != RET_SUCCESS) {
            seterr(err, el, "Erasing the flash sector at 0x%05lx failed (%s)", (unsigned long)a, status_name(st));
            return false;
        }
        progress(io, "erase", a + TF_SECTOR, len);
    }
    return true;
}

static int download(const tf_io_t *io, tf_result_t *r, uint32_t addr, uint32_t size) {
    uint8_t p[8];
    wr32be(p, addr);
    wr32be(p + 4, size);
    r->downloads++;
    return command(io, r, CMD_DOWNLOAD, p, sizeof(p), T_DOWNLOAD, NULL, 0, true);
}

// cc2538-bsl's writeMemory, with the runs made explicit: each stretch of chunks that aren't all 0xFF gets
// one DOWNLOAD(start, exact length) and then SEND_DATA per 248 bytes; all-0xFF chunks are skipped (the
// erase left them 0xFF). On a NACK the packet is sent again as is (the bootloader dropped it). On a
// timeout we can't know whether it was written, so after resync a fresh DOWNLOAD restarts the run at this
// chunk and the chunk goes again (programming the same bytes twice is harmless).
static bool write_image(const tf_io_t *io, tf_result_t *r, uint32_t len, char *err, size_t el) {
    uint8_t buf[TF_CHUNK], next[TF_CHUNK];
    progress(io, "write", 0, len);
    uint32_t off = 0;
    uint32_t n = len < TF_CHUNK ? len : TF_CHUNK;
    if (io->image_read(io->ctx, 0, buf, n) != 0) {
        seterr(err, el, "Reading the stored image failed at byte 0");
        return false;
    }
    uint32_t run_end = 0;  // end of the run being written; 0 = between runs
    bool dl_active = false;  // a DOWNLOAD covering this chunk is in progress
    uint32_t reported = 0;
    while (off < len) {
        uint32_t padded = (n + 3u) & ~3u;
        for (uint32_t i = n; i < padded; i++) buf[i] = 0xFF;
        if (all_ff(buf, padded)) {
            r->chunks_skipped++;
            run_end = 0;
            dl_active = false;
        } else {
            if (run_end == 0) {
                // Find where this run ends: the next all-0xFF chunk or the end of the image.
                uint32_t e = off + padded;
                while (e < len) {
                    uint32_t k = len - e < TF_CHUNK ? len - e : TF_CHUNK;
                    if (io->image_read(io->ctx, e, next, k) != 0) {
                        seterr(err, el, "Reading the stored image failed at byte %lu", (unsigned long)e);
                        return false;
                    }
                    uint32_t kp = (k + 3u) & ~3u;
                    for (uint32_t i = k; i < kp; i++) next[i] = 0xFF;
                    if (all_ff(next, kp)) break;
                    e += kp;
                }
                run_end = e;
            }
            bool need_dl = !dl_active;
            int fails = 0;
            for (;;) {
                if (need_dl) {
                    int st = download(io, r, off, run_end - off);
                    if (st != RET_SUCCESS) {
                        if (st == E_GONE) seterr(err, el, "The stick went away while writing (at 0x%05lx)", (unsigned long)off);
                        else seterr(err, el, "The bootloader refused DOWNLOAD at 0x%05lx (%s)", (unsigned long)off, status_name(st));
                        return false;
                    }
                    need_dl = false;
                }
                if (send_packet(io, CMD_SEND_DATA, buf, padded) < 0) {
                    seterr(err, el, "The stick went away while writing (at 0x%05lx)", (unsigned long)off);
                    return false;
                }
                int a = wait_ack(io, T_SEND_DATA);
                int st = E_NOANSWER;
                if (a == 1) {
                    st = get_status(io, r);
                    if (st == RET_SUCCESS) break;
                    if (st == RET_FLASH_FAIL) {
                        seterr(err, el, "Writing the flash at 0x%05lx failed (flash fail)", (unsigned long)off);
                        return false;
                    }
                    need_dl = true;  // status unreadable or the download state got lost: restart the run here
                } else if (a == 0) {
                    // NACK: dropped, the download state didn't move
                } else if (a == E_NOANSWER) {
                    if (resync(io) == E_GONE) a = E_GONE;
                    need_dl = true;
                }
                if (a == E_GONE || st == E_GONE) {
                    seterr(err, el, "The stick went away while writing (at 0x%05lx)", (unsigned long)off);
                    return false;
                }
                r->retries++;
                if (++fails >= TRIES) {
                    seterr(err, el, "Writing kept failing at 0x%05lx (%s)", (unsigned long)off,
                           a == 0 ? "refused" : a == 1 ? status_name(st) : "no answer");
                    return false;
                }
            }
            dl_active = true;
            r->chunks_written++;
            r->bytes_written += padded;
        }
        off += padded;
        if (off - reported >= 16 * 1024 || off >= len) {
            progress(io, "write", off < len ? off : len, len);
            reported = off;
        }
        if (off < len) {
            n = len - off < TF_CHUNK ? len - off : TF_CHUNK;
            if (io->image_read(io->ctx, off, buf, n) != 0) {
                seterr(err, el, "Reading the stored image failed at byte %lu", (unsigned long)off);
                return false;
            }
        }
    }
    return true;
}

static int device_crc(const tf_io_t *io, tf_result_t *r, uint32_t len, uint32_t *crc) {
    uint8_t p[12], d[4];
    wr32be(p, 0);
    wr32be(p + 4, len);
    wr32be(p + 8, 0);  // read repeat count (cmdCRC32CC26xx)
    int st = command(io, r, CMD_CRC32, p, sizeof(p), T_CRC, d, sizeof(d), true);
    if (st == RET_SUCCESS) *crc = rd32be(d);
    return st;
}

// ---------------------------------------------------------------- the job

bool tf_flash(const tf_io_t *io, const tf_opts_t *o, tf_result_t *r, char *err, size_t el) {
    memset(r, 0, sizeof(*r));
    err[0] = 0;
    uint32_t len = o->image_len;

    // 1. The image, before the dongle is touched.
    progress(io, "check", 0, 0);
    const char *why = tf_image_check(io, len, o->chip_flash, &r->image);
    if (why) {
        seterr(err, el, "%s", why);
        return false;
    }
    logf_(io, "Image: %lu KB, backdoor on DIO%u (active %s), CRC-32 0x%08lx, %lu of %lu chunks blank",
          (unsigned long)(len >> 10), r->image.bl_pin, r->image.bl_level ? "high" : "low", (unsigned long)r->image.crc32,
          (unsigned long)r->image.chunks_ff, (unsigned long)((len + TF_CHUNK - 1) / TF_CHUNK));
    if (r->image.image_valid != 0)
        logf_(io, "Warning: the image's CCFG IMAGE_VALID_CONF is 0x%08lx, so the ROM may not start it",
              (unsigned long)r->image.image_valid);

    // 2. Into the bootloader and in sync.
    progress(io, "bootloader", 0, 0);
    uint32_t baud = o->baud ? o->baud : TF_DEFAULT_BAUD;
    uint32_t fb = o->fallback_baud ? o->fallback_baud : TF_FALLBACK_BAUD;
    enter_bsl(io, o);
    bool synced = sync_bsl(io, r, baud);
    if (!synced && fb != baud) {
        logf_(io, "No answer from the bootloader at %lu baud; trying %lu", (unsigned long)baud, (unsigned long)fb);
        enter_bsl(io, o);  // autobaud only happens once per bootloader start: start it again
        synced = sync_bsl(io, r, fb);
        if (synced) baud = fb;
    }
    if (!synced) {
        if (o->entry == TF_ENTRY_NONE)
            seterr(err, el, "No answer from the stick's bootloader (hold its BOOT button while plugging it in, or pick a DTR/RTS method)");
        else
            seterr(err, el, "No answer from the stick's bootloader after the %s (wrong method for this stick, or its firmware has the backdoor off)",
                   entry_name(o));
        return false;
    }
    r->entered = entry_name(o);
    r->baud = baud;
    logf_(io, "Bootloader in sync at %lu baud (%s)", (unsigned long)baud, r->entered);

    // 3. Which chip, how much flash, how its CCFG is set now.
    uint8_t id[4];
    int st = command(io, r, CMD_GET_CHIP_ID, NULL, 0, T_ACK, id, sizeof(id), true);
    if (st != RET_SUCCESS) {
        seterr(err, el, "The bootloader didn't answer GET_CHIP_ID (%s)", status_name(st));
        return false;
    }
    r->chip_id_ok = true;
    r->chip_id = rd32be(id);
    uint32_t icepick = 0, fsize = 0;
    if (mem_read32(io, r, ICEPICK_DEVICE_ID, &icepick) == RET_SUCCESS && mem_read32(io, r, FLASH_SIZE_REG, &fsize) == RET_SUCCESS) {
        r->wafer_id = (icepick >> 12) & 0xFFFF;
        r->pg_rev = (uint8_t)(icepick >> 28);
        if (r->wafer_id == 0xB99A || r->wafer_id == 0xB9BE) {
            seterr(err, el, "This is a CC26x0/CC13x0 (wafer 0x%04lx), not a CC2652/CC1352", (unsigned long)r->wafer_id);
            return false;
        }
        r->device_flash = (fsize & 0xFF) * TF_SECTOR;
        if (r->device_flash >= TF_CCFG_LEN && mem_read32(io, r, r->device_flash - TF_CCFG_LEN + TF_CCFG_BL_CONFIG, &r->device_bl_config) == RET_SUCCESS &&
            mem_read32(io, r, r->device_flash - TF_CCFG_LEN + TF_CCFG_ERASE_CONF, &r->device_erase_conf) == RET_SUCCESS)
            r->device_info_ok = true;
    }
    logf_(io, "Chip id 0x%08lx, wafer 0x%04lx PG%u, %lu KB flash", (unsigned long)r->chip_id, (unsigned long)r->wafer_id,
          r->pg_rev, (unsigned long)(r->device_flash >> 10));
    if (r->device_flash && r->device_flash != len) {
        seterr(err, el, "The chip has %lu KB of flash but the image is for %lu KB: wrong firmware for this stick",
               (unsigned long)(r->device_flash >> 10), (unsigned long)(len >> 10));
        return false;
    }
    if (!r->device_flash && !o->skip_device_size) {
        seterr(err, el, "Couldn't read the chip's flash size, so it can't be checked against the image");
        return false;
    }
    if (r->device_info_ok) {
        uint32_t cur = r->device_bl_config;
        bool cur_on = (cur >> 24) == 0xC5 && (cur & 0xFF) == 0xC5;
        uint8_t pin = (uint8_t)(cur >> 8), lvl = (uint8_t)((cur >> 16) & 1);
        if (cur_on && (pin != r->image.bl_pin || lvl != r->image.bl_level)) {
            if (!o->allow_bl_pin_change) {
                seterr(err, el,
                       "The image's bootloader backdoor is DIO%u active %s but this stick's is DIO%u active %s: firmware for another board "
                       "(its button / DTR-RTS entry would stop working)",
                       r->image.bl_pin, r->image.bl_level ? "high" : "low", pin, lvl ? "high" : "low");
                return false;
            }
            logf_(io, "Backdoor moves from DIO%u to DIO%u (allowed)", pin, r->image.bl_pin);
        }
    }

    // 4. Erase, write, verify. A CRC mismatch earns one more full pass.
    for (int pass = 0; pass < 2; pass++) {
        r->passes++;
        if (!erase(io, o, r, len, err, el)) return false;
        logf_(io, "Flash erased (%s)", r->erase_how);
        if (!write_image(io, r, len, err, el)) return false;
        logf_(io, "Written: %lu chunks, %lu blank skipped, %lu DOWNLOADs, %lu retries", (unsigned long)r->chunks_written,
              (unsigned long)r->chunks_skipped, (unsigned long)r->downloads, (unsigned long)r->retries);
        progress(io, "verify", 0, len);
        st = device_crc(io, r, len, &r->crc_device);
        if (st != RET_SUCCESS) {
            seterr(err, el, "The bootloader didn't answer CRC32 (%s)", status_name(st));
            return false;
        }
        if (r->crc_device == r->image.crc32) {
            r->verified = true;
            break;
        }
        logf_(io, "CRC32 mismatch: chip 0x%08lx, image 0x%08lx", (unsigned long)r->crc_device, (unsigned long)r->image.crc32);
        if (pass == 0) {
            logf_(io, "Erasing and writing once more");
            r->chunks_written = r->chunks_skipped = 0;
            r->bytes_written = 0;
        }
    }
    if (!r->verified) {
        seterr(err, el, "The flash doesn't match the image after writing it twice (chip CRC32 0x%08lx, image 0x%08lx)",
               (unsigned long)r->crc_device, (unsigned long)r->image.crc32);
        return false;
    }
    progress(io, "verify", len, len);
    logf_(io, "CRC32 verified (0x%08lx)", (unsigned long)r->crc_device);

    // 5. Start it.
    if (o->no_reset) return true;
    progress(io, "reset", 0, 0);
    st = command(io, r, CMD_RESET, NULL, 0, T_ACK, NULL, 0, false);
    if (st != RET_SUCCESS) {
        seterr(err, el, "Written and verified, but the bootloader didn't take RESET (%s): unplug and replug the stick",
               status_name(st));
        return false;
    }
    r->reset_sent = true;
    logf_(io, "Reset: the new firmware is starting");
    return true;
}

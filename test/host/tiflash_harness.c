// Runs the firmware's own tiflash_core.c and ihex_core.c against the simulated TI dongle in
// test_tiflash.py. Every tf_io_t call is a request over stdout, answered on stdin; the simulator keeps a
// virtual clock. Same wire format as ncpflash_harness.c, plus 'D' (a data record from the hex parser).
//
//   tiflash_harness check <chip_flash> <size>
//   tiflash_harness flash <entry> <flags> <baud> <fallback_baud> <chip_flash> <size>
//       entry: 0 none, 1 sonoff, 2 default; flags: 1 invert, 2 active high, 4 skip device size,
//       8 allow backdoor pin change, 16 sector erase, 32 no reset
//   tiflash_harness ihex <maxchunk> <seed> <size>
//       feeds the "image" (the .hex text) to ihex_core in random pieces of 1..maxchunk bytes
//       (maxchunk 0 = all at once), reporting each data record
//
// Ends with an 'X' message carrying the result as JSON.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ihex_core.h"
#include "tiflash_core.h"

static void put(const void *p, size_t n) {
    if (fwrite(p, 1, n, stdout) != n) exit(3);
}
static void get(void *p, size_t n) {
    if (fread(p, 1, n, stdin) != n) exit(4);
}
static void put32(uint32_t v) { put(&v, 4); }
static int32_t reply(void) {
    int32_t r;
    fflush(stdout);
    get(&r, 4);
    return r;
}

static int io_read(void *ctx, uint8_t *buf, size_t max, uint32_t timeout_ms) {
    (void)ctx;
    put("R", 1);
    put32((uint32_t)max);
    put32(timeout_ms);
    int32_t n = reply();
    if (n > (int32_t)max) exit(5);
    if (n > 0) get(buf, (size_t)n);
    return n;
}
static int io_write(void *ctx, const uint8_t *buf, size_t len) {
    (void)ctx;
    put("W", 1);
    put32((uint32_t)len);
    put(buf, len);
    return reply();
}
static int io_baud(void *ctx, uint32_t baud) {
    (void)ctx;
    put("B", 1);
    put32(baud);
    return reply();
}
static int io_lines(void *ctx, bool dtr, bool rts) {
    (void)ctx;
    uint8_t b[3] = {'L', dtr, rts};
    put(b, 3);
    return reply();
}
static void io_sleep(void *ctx, uint32_t ms) {
    (void)ctx;
    put("S", 1);
    put32(ms);
    reply();
}
static uint32_t io_now(void *ctx) {
    (void)ctx;
    put("T", 1);
    return (uint32_t)reply();
}
static int io_image(void *ctx, uint32_t off, uint8_t *buf, size_t len) {
    (void)ctx;
    put("I", 1);
    put32(off);
    put32((uint32_t)len);
    int32_t r = reply();
    if (r == 0) get(buf, len);
    return r;
}
static void io_progress(void *ctx, const char *step, uint32_t done, uint32_t total) {
    (void)ctx;
    uint8_t n = (uint8_t)strlen(step);
    put("P", 1);
    put(&n, 1);
    put(step, n);
    put32(done);
    put32(total);
    reply();
}
static void io_log(void *ctx, const char *msg) {
    (void)ctx;
    uint16_t n = (uint16_t)strlen(msg);
    put("G", 1);
    put(&n, 2);
    put(msg, n);
    reply();
}

static void json_str(char *out, size_t n, const char *s) {
    size_t o = 0;
    out[o++] = '"';
    for (; s && *s && o + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o++] = '"';
    out[o] = 0;
}

static void finish(const char *json) {
    uint32_t n = (uint32_t)strlen(json);
    put("X", 1);
    put32(n);
    put(json, n);
    fflush(stdout);
    exit(0);
}

static int on_data(void *ctx, uint32_t addr, const uint8_t *data, size_t len) {
    (void)ctx;
    put("D", 1);
    put32(addr);
    put32((uint32_t)len);
    put(data, len);
    return reply();  // the test can make the "caller" refuse
}

static const tf_io_t IO = {
    .ctx = NULL,
    .read = io_read,
    .write = io_write,
    .set_baud = io_baud,
    .set_lines = io_lines,
    .sleep_ms = io_sleep,
    .now_ms = io_now,
    .image_read = io_image,
    .progress = io_progress,
    .log = io_log,
};

static void run_ihex(uint32_t maxchunk, uint32_t seed, uint32_t size) {
    static ihex_t h;  // big-ish: keep it off the stack like the firmware would
    ihex_init(&h, on_data, NULL);
    uint32_t rng = seed * 2654435761u + 1;
    const char *e = NULL;
    char buf[4096];
    uint32_t chunks = 0;
    for (uint32_t off = 0; off < size && !e;) {
        uint32_t n = size - off;
        if (maxchunk) {
            rng = rng * 1103515245u + 12345u;
            uint32_t k = 1 + (rng >> 8) % maxchunk;
            if (k < n) n = k;
        }
        if (n > sizeof(buf)) n = sizeof(buf);
        if (io_image(NULL, off, (uint8_t *)buf, n) != 0) exit(6);
        e = ihex_feed(&h, buf, n);
        off += n;
        chunks++;
    }
    if (!e) e = ihex_finish(&h);
    char es[200], out[600];
    json_str(es, sizeof(es), e ? e : "");
    snprintf(out, sizeof(out),
             "{\"err\":%s,\"min\":%u,\"end\":%u,\"total\":%u,\"records\":%u,\"lines\":%u,\"eof\":%d,\"chunks\":%u}", es,
             h.min_addr, h.end_addr, h.total_bytes, h.records, h.line, h.eof_seen, chunks);
    finish(out);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    uint32_t size = (uint32_t)strtoul(argv[argc - 1], NULL, 10);
    char out[3000], e[400];
    if (strcmp(argv[1], "ihex") == 0) {
        if (argc != 5) return 2;
        run_ihex((uint32_t)strtoul(argv[2], NULL, 10), (uint32_t)strtoul(argv[3], NULL, 10), size);
    }
    if (strcmp(argv[1], "check") == 0) {
        tf_image_t g;
        const char *why = tf_image_check(&IO, size, (uint32_t)strtoul(argv[2], NULL, 10), &g);
        json_str(e, sizeof(e), why ? why : "");
        snprintf(out, sizeof(out),
                 "{\"check\":%s,\"flash\":%u,\"bl_config\":%u,\"bl_pin\":%u,\"bl_level\":%u,\"erase_conf\":%u,"
                 "\"image_valid\":%u,\"crc\":%u,\"ff\":%u}",
                 e, g.flash_size, g.bl_config, g.bl_pin, g.bl_level, g.erase_conf, g.image_valid, g.crc32, g.chunks_ff);
        finish(out);
    }
    if (strcmp(argv[1], "flash") != 0 || argc != 8) return 2;
    uint32_t flags = (uint32_t)strtoul(argv[3], NULL, 10);
    tf_opts_t o = {
        .entry = (tf_entry_t)strtoul(argv[2], NULL, 10),
        .invert_lines = flags & 1,
        .active_high = (flags & 2) != 0,
        .skip_device_size = (flags & 4) != 0,
        .allow_bl_pin_change = (flags & 8) != 0,
        .sector_erase = (flags & 16) != 0,
        .no_reset = (flags & 32) != 0,
        .baud = (uint32_t)strtoul(argv[4], NULL, 10),
        .fallback_baud = (uint32_t)strtoul(argv[5], NULL, 10),
        .chip_flash = (uint32_t)strtoul(argv[6], NULL, 10),
        .image_len = size,
    };
    tf_result_t r;
    char err[300];
    bool ok = tf_flash(&IO, &o, &r, err, sizeof(err));
    json_str(e, sizeof(e), err);
    char ent[80], how[40];
    json_str(ent, sizeof(ent), r.entered);
    json_str(how, sizeof(how), r.erase_how);
    snprintf(out, sizeof(out),
             "{\"ok\":%d,\"err\":%s,\"entered\":%s,\"baud\":%u,\"chip_id_ok\":%d,\"chip_id\":%u,\"device_info_ok\":%d,"
             "\"wafer\":%u,\"pg\":%u,\"device_flash\":%u,\"device_bl\":%u,\"device_erase_conf\":%u,\"erase_how\":%s,"
             "\"erased\":%d,\"bytes\":%u,\"written\":%u,\"skipped\":%u,\"downloads\":%u,\"retries\":%u,\"passes\":%u,"
             "\"crc_device\":%u,\"crc_image\":%u,\"verified\":%d,\"reset\":%d,\"bl_pin\":%u,\"ff\":%u}",
             ok, e, ent, r.baud, r.chip_id_ok, r.chip_id, r.device_info_ok, r.wafer_id, r.pg_rev, r.device_flash,
             r.device_bl_config, r.device_erase_conf, how, r.erased, r.bytes_written, r.chunks_written, r.chunks_skipped,
             r.downloads, r.retries, r.passes, r.crc_device, r.image.crc32, r.verified, r.reset_sent, r.image.bl_pin,
             r.image.chunks_ff);
    finish(out);
}

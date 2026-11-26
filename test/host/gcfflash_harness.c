// Runs the firmware's own gcfflash_core.c against the simulated ConBee sticks in test_gcfflash.py.
// Every gf_io_t call is a request over stdout, answered on stdin; the simulator keeps a virtual clock.
//
//   gcfflash_harness check <device 2|3> <file name> <size>
//   gcfflash_harness flash <device 2|3> <file name> <budget_ms> <size>
//
// Ends with an 'X' message carrying the result as JSON.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gcfflash_core.h"

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
static int io_reopen(void *ctx, uint32_t timeout_ms) {
    (void)ctx;
    put("O", 1);
    put32(timeout_ms);
    return reply();
}

static void json_str(char *out, size_t n, const char *s) {
    size_t o = 0;
    out[o++] = '"';
    for (; s && *s && o + 7 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20 || c >= 0x7F) {
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

int main(int argc, char **argv) {
    gf_io_t io = {
        .ctx = NULL,
        .read = io_read,
        .write = io_write,
        .set_baud = io_baud,
        .sleep_ms = io_sleep,
        .now_ms = io_now,
        .image_read = io_image,
        .progress = io_progress,
        .log = io_log,
        .reopen = io_reopen,
    };
    if (argc < 5) return 2;
    gf_device_t dev = (gf_device_t)atoi(argv[2]);
    const char *name = argv[3];
    uint32_t size = (uint32_t)strtoul(argv[argc - 1], NULL, 10);
    char out[2048], e1[400], banner[120];
    if (strcmp(argv[1], "check") == 0) {
        gf_file_t f;
        const char *why = gf_file_check(&io, size, name, dev, &f);
        json_str(e1, sizeof(e1), why ? why : "");
        snprintf(out, sizeof(out),
                 "{\"check\":%s,\"type\":%u,\"proto\":%u,\"device\":%d,\"platform\":%u,\"target\":%u,\"data_size\":%u,"
                 "\"data_offset\":%u,\"crc8\":%u,\"crc32\":%u,\"name_version\":%u,\"product\":%u,\"crc_checked\":%d}",
                 e1, f.type, f.proto, (int)f.device, f.platform, f.target, f.data_size, f.data_offset, f.crc8, f.crc32,
                 f.name_version, f.product, f.crc_checked);
        finish(out);
    }
    if (argc < 6) return 2;
    gf_flash_opts_t o = {
        .device = dev,
        .file_name = name,
        .size = size,
        .budget_ms = (uint32_t)strtoul(argv[4], NULL, 10),
    };
    gf_result_t r;
    char err[300];
    bool ok = gf_flash(&io, &o, &r, err, sizeof(err));
    json_str(e1, sizeof(e1), err);
    json_str(banner, sizeof(banner), r.banner);
    snprintf(out, sizeof(out),
             "{\"ok\":%d,\"err\":%s,\"entered\":\"%s\",\"before\":%u,\"after\":%u,\"expected\":%u,\"bootloader\":%u,"
             "\"app_crc\":%u,\"app_crc_ok\":%d,\"banner\":%s,\"attempts\":%u,\"reopens\":%u,\"resent\":%u,"
             "\"app_left_running\":%d,\"in_bootloader\":%d}",
             ok, e1, r.entered ? r.entered : "", r.before, r.after, r.expected, r.bootloader, r.app_crc, r.app_crc_ok,
             banner, r.attempts, r.reopens, r.resent, r.app_left_running, r.in_bootloader);
    finish(out);
}

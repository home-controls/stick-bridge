// Runs the firmware's own ncpflash_core.c against the simulated dongle in test_ncpflash.py.
// Every nf_io_t call is a request over stdout, answered on stdin; the simulator keeps a virtual clock.
//
//   ncpflash_harness check <size>
//   ncpflash_harness flash <app_baud> <new_baud> <size>
//
// Ends with an 'X' message carrying the result as JSON.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ncpflash_core.h"

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
    for (; *s && o + 7 < n; s++) {
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

int main(int argc, char **argv) {
    nf_io_t io = {
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
    if (argc < 3) return 2;
    char out[2048], e1[600], e2[300], meta[1100];
    uint32_t size = (uint32_t)strtoul(argv[argc - 1], NULL, 10);
    nf_gbl_t g;
    uint8_t expect = getenv("NF_EXPECT_SPINEL") ? NF_EXPECT_SPINEL : NF_EXPECT_EZSP;
    const char *why = nf_gbl_check(&io, size, &g);
    if (!why) why = nf_gbl_app_problem(&g, expect);
    json_str(e1, sizeof(e1), why ? why : "");
    json_str(meta, sizeof(meta), g.metadata);
    if (strcmp(argv[1], "check") == 0 || why) {
        snprintf(out, sizeof(out),
                 "{\"check\":%s,\"tag_len\":%u,\"trailing\":%u,\"app_type\":%u,\"app_version\":%u,\"prog\":%u,"
                 "\"signed\":%d,\"encrypted\":%d,\"meta\":%s,\"meta_cut\":%d}",
                 e1, g.tag_len, g.trailing, g.app_type, g.app_version, g.prog_bytes, g.is_signed, g.encrypted, meta,
                 g.metadata_cut);
        finish(out);
    }
    nf_flash_opts_t o = {
        .app_baud = (uint32_t)strtoul(argv[2], NULL, 10),
        .new_baud = (uint32_t)strtoul(argv[3], NULL, 10),
        .upload_len = g.tag_len,
        .expect = expect,
    };
    nf_result_t r;
    char err[200];
    static char sp1[128], sp2[128];
    bool ok = nf_flash(&io, &o, &r, err, sizeof(err));
    json_str(e2, sizeof(e2), err);
    snprintf(out, sizeof(out),
             "{\"check\":\"\",\"ok\":%d,\"err\":%s,\"entered\":\"%s\",\"bootloader\":\"%s\",\"before\":[%d,%u,%u],"
             "\"after\":[%d,%u,%u],\"app_left_running\":%d,\"in_bootloader\":%d,\"retries\":%u,\"tag_len\":%u,"
             "\"before_spinel\":%s,\"after_spinel\":%s}",
             ok, e2, r.entered ? r.entered : "", r.bootloader, r.before.ok, r.before.ezsp_version, r.before.stack_version,
             r.after.ok, r.after.ezsp_version, r.after.stack_version, r.app_left_running, r.in_bootloader, r.retries,
             g.tag_len, (json_str(sp1, sizeof(sp1), r.before_spinel), sp1), (json_str(sp2, sizeof(sp2), r.after_spinel), sp2));
    finish(out);
}

// Runs the firmware's own nrfdfu_core.c against the simulated nRF52840 Dongle bootloader in test_nrfdfu.py.
// Every nd_io_t call is a request over stdout, answered on stdin; the simulator keeps a virtual clock.
//
//   nrfdfu_harness build <fw_version> <hw_version> <size>     init packet for the image
//   nrfdfu_harness parse <dat_hex> <size>                     decode + policy + image check
//   nrfdfu_harness flash <dat_hex|-> <fw_version> <size>      '-' = build the init packet here
//   nrfdfu_harness hexbin <size>                              Intel HEX -> .bin
//   nrfdfu_harness hexflash <fw_version> <size>               Intel HEX -> .bin -> init packet -> flash
//
// <size> is the length of the image the simulator serves ('I'). Ends with an 'X' message carrying the
// result as JSON.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nrfdfu_core.h"

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

static uint8_t *local_img;  // hexflash: the converted .bin, served from here instead of the simulator
static uint32_t local_len;

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
    if (local_img) {
        if ((uint64_t)off + len > local_len) return -1;
        memcpy(buf, local_img + off, len);
        return 0;
    }
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
static int io_enter(void *ctx) {
    (void)ctx;
    put("E", 1);
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
        } else if (c < 0x20) {
            o += (size_t)snprintf(out + o, n - o, "\\u%04x", c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o++] = '"';
    out[o] = 0;
}

static void hexstr(char *out, const uint8_t *d, size_t n) {
    for (size_t i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", d[i]);
    out[2 * n] = 0;
}

static size_t unhex(const char *s, uint8_t *out, size_t max) {
    size_t n = 0;
    if (strcmp(s, "x") == 0) return 0;  // empty
    for (; s[0] && s[1] && n < max; s += 2) {
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) exit(6);
        out[n++] = (uint8_t)v;
    }
    return n;
}

static void finish(const char *json) {
    uint32_t n = (uint32_t)strlen(json);
    put("X", 1);
    put32(n);
    put(json, n);
    fflush(stdout);
    exit(0);
}

static nd_io_t io = {
    .ctx = NULL,
    .read = io_read,
    .write = io_write,
    .set_lines = io_lines,
    .sleep_ms = io_sleep,
    .now_ms = io_now,
    .image_read = io_image,
    .progress = io_progress,
    .log = io_log,
    .enter_dfu = io_enter,
};

static char out[8192];

static void do_flash(const uint8_t *dat, size_t dat_len, uint32_t size, const char *extra) {
    nd_flash_opts_t o = {.init = dat, .init_len = dat_len, .bin_len = size};
    nd_result_t r;
    char err[300], e[700], dh[2 * ND_INIT_MAX + 1];
    bool ok = nd_flash(&io, &o, &r, err, sizeof(err));
    json_str(e, sizeof(e), err);
    hexstr(dh, dat, dat_len);
    snprintf(out, sizeof(out),
             "{\"ok\":%d,\"err\":%s,\"entered\":\"%s\",\"mtu\":%u,\"chunk\":%u,\"obj_max\":%u,\"retries\":%u,"
             "\"app_erased\":%d,\"result\":%u,\"ext\":%u,\"init\":\"%s\"%s}",
             ok, e, r.entered ? r.entered : "", r.mtu, r.chunk, r.obj_max, r.retries, r.app_erased, r.result, r.ext_error,
             dh, extra ? extra : "");
    finish(out);
}

// Builds the init packet for size bytes of image through io (fw/hw versions given).
static size_t build(uint32_t size, uint32_t fw, uint32_t hw, uint8_t *dat) {
    uint8_t sha[32];
    const char *e = nd_image_sha256(&io, size, sha);
    if (e) {
        char j[200], m[160];
        json_str(m, sizeof(m), e);
        snprintf(j, sizeof(j), "{\"ok\":0,\"err\":%s}", m);
        finish(j);
    }
    return nd_init_build(size, sha, fw, hw, dat, ND_INIT_MAX);
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    uint32_t size = (uint32_t)strtoul(argv[argc - 1], NULL, 10);
    uint8_t dat[ND_INIT_MAX + 64];
    const char *cmd = argv[1];

    if (strcmp(cmd, "build") == 0 && argc == 5) {
        size_t n = build(size, (uint32_t)strtoul(argv[2], NULL, 0), (uint32_t)strtoul(argv[3], NULL, 0), dat);
        char h[2 * ND_INIT_MAX + 1];
        hexstr(h, dat, n);
        snprintf(out, sizeof(out), "{\"init\":\"%s\"}", h);
        finish(out);
    }
    if (strcmp(cmd, "parse") == 0 && argc == 4) {
        size_t n = unhex(argv[2], dat, sizeof(dat));
        nd_init_t in;
        const char *pe = nd_init_parse(dat, n, &in);
        const char *po = pe ? "" : nd_init_policy(&in);
        const char *ie = (pe || po) ? "" : nd_image_check(&io, size, &in);
        char a[300], b[300], c[300], hh[65], sd[200] = "";
        json_str(a, sizeof(a), pe ? pe : "");
        json_str(b, sizeof(b), po ? po : "");
        json_str(c, sizeof(c), ie ? ie : "");
        hexstr(hh, in.hash, 32);
        for (int i = 0; i < in.sd_req_count; i++)
            snprintf(sd + strlen(sd), sizeof(sd) - strlen(sd), "%s%u", i ? "," : "", (unsigned)in.sd_req[i]);
        snprintf(out, sizeof(out),
                 "{\"parse\":%s,\"policy\":%s,\"image\":%s,\"signed\":%d,\"type\":%u,\"has_type\":%d,\"fw\":%u,\"hw\":%u,"
                 "\"has_hw\":%d,\"app_size\":%u,\"hash_type\":%u,\"hash\":\"%s\",\"sd_req\":[%s],\"is_debug\":%d}",
                 a, b, c, in.is_signed, in.type, in.has_type, in.fw_version, in.hw_version, in.has_hw_version, in.app_size,
                 in.hash_type, hh, sd, in.is_debug);
        finish(out);
    }
    if (strcmp(cmd, "flash") == 0 && argc == 5) {
        size_t n = strcmp(argv[2], "-") == 0 ? build(size, (uint32_t)strtoul(argv[3], NULL, 0), ND_HW_VERSION, dat)
                                             : unhex(argv[2], dat, sizeof(dat));
        do_flash(dat, n, size, NULL);
    }
    if ((strcmp(cmd, "hexbin") == 0 && argc == 3) || (strcmp(cmd, "hexflash") == 0 && argc == 4)) {
        uint32_t max = ND_APP_LIMIT;
        uint8_t *bin = malloc(max);
        if (!bin) return 7;
        uint32_t len = 0, start = 0, dropped = 0;
        const char *e = nd_hex_to_bin(&io, size, bin, max, &len, &start, &dropped);
        char m[300], sh[65] = "";
        json_str(m, sizeof(m), e ? e : "");
        if (!e) {
            nd_sha256_t s;
            uint8_t d[32];
            nd_sha256_init(&s);
            nd_sha256_update(&s, bin, len);
            nd_sha256_final(&s, d);
            hexstr(sh, d, 32);
        }
        char extra[600];
        snprintf(extra, sizeof(extra), ",\"hexerr\":%s,\"len\":%u,\"start\":%u,\"dropped\":%u,\"sha\":\"%s\"", m, len, start,
                 dropped, sh);
        if (cmd[3] == 'b' || e) {
            snprintf(out, sizeof(out), "{%s}", extra + 1);
            free(bin);
            finish(out);
        }
        local_img = bin, local_len = len;
        size_t n = build(len, (uint32_t)strtoul(argv[2], NULL, 0), ND_HW_VERSION, dat);
        do_flash(dat, n, len, extra);
    }
    return 2;
}

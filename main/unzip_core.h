#pragma once
// Streaming reader for a single-file ZIP (Koenkk's Z-Stack releases ship one .hex per .zip), no ESP-IDF here
// (test/host/test_unzip.py runs it on real release files with zlib as the inflater). Fed the archive in
// pieces; hands the file's bytes to a callback as they come out; checks the stored CRC-32 at the end.
// The raw-deflate engine is plugged in: the ROM's tinfl on the device, zlib on the host.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Raw deflate (no zlib header). Consumes from in (sets *in_used), produces into out (sets *out_made).
// Returns 0 to go on, 1 when the stream ended, <0 on corrupt data.
typedef struct {
    void *ctx;
    int (*inflate)(void *ctx, const uint8_t *in, size_t in_len, size_t *in_used, uint8_t *out, size_t out_cap, size_t *out_made);
} uz_inflater_t;

// The most a zip may unpack to: a 704 KB CC26x2 image is ~2 MB as Intel HEX.
#define UZ_MAX_OUT (4u * 1024 * 1024)

typedef int (*uz_out_cb)(void *ctx, const uint8_t *data, size_t len);  // 0 ok, else stop with that error

typedef struct {
    uz_inflater_t inf;
    uz_out_cb out;
    void *out_ctx;
    int st;            // internal state
    uint8_t hdr[30];
    size_t hlen;
    uint16_t method, flags;
    uint32_t skip;                 // name + extra bytes still to drop
    uint32_t crc_want, csize, usize;
    uint32_t cin, uout, crc;       // compressed bytes consumed, bytes produced, running CRC-32
    char name[96];
    size_t nlen;
    uint8_t obuf[1024];
    const char *err;
} uz_t;

void uz_init(uz_t *z, const uz_inflater_t *inf, uz_out_cb out, void *out_ctx);
// NULL while fine; the error once something's wrong (then stays).
const char *uz_feed(uz_t *z, const uint8_t *data, size_t len);
// After the last piece: NULL when the file came out whole with the right CRC-32.
const char *uz_finish(uz_t *z);
uint32_t uz_crc32(uint32_t crc, const uint8_t *d, size_t n);  // zlib's; start with 0

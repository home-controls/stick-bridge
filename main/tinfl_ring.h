#pragma once
// Adapter from miniz's tinfl (in the ESP32-P4 ROM; the host tests build it from miniz's source) to
// unzip_core's pull interface: tinfl decompresses into a 32 KB ring (its LZ dictionary), and this hands the
// ring's new bytes out in whatever pieces the caller asks for. Include after miniz.h.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The ROM's tinfl writes into a 32 KB ring (the LZ dictionary): this hands the ring's new bytes out in pieces.
typedef struct {
    tinfl_decompressor d;
    uint8_t dict[TINFL_LZ_DICT_SIZE];
    size_t pos, pend_start, pend_len;
    bool done;
} rom_inf_t;

static inline int rom_inflate(void *ctx, const uint8_t *in, size_t in_len, size_t *used, uint8_t *out, size_t cap, size_t *made) {
    rom_inf_t *r = ctx;
    *used = 0;
    *made = 0;
    if (r->pend_len == 0 && !r->done) {
        size_t isz = in_len, osz = TINFL_LZ_DICT_SIZE - r->pos;
        tinfl_status st = tinfl_decompress(&r->d, in, &isz, r->dict, r->dict + r->pos, &osz, in_len ? TINFL_FLAG_HAS_MORE_INPUT : 0);
        *used = isz;
        r->pend_start = r->pos;
        r->pend_len = osz;
        r->pos = (r->pos + osz) & (TINFL_LZ_DICT_SIZE - 1);
        if (st < 0) return -1;
        if (st == TINFL_STATUS_DONE) r->done = true;
    }
    size_t k = r->pend_len < cap ? r->pend_len : cap;
    memcpy(out, r->dict + r->pend_start, k);
    r->pend_start += k;
    r->pend_len -= k;
    *made = k;
    return (r->done && r->pend_len == 0) ? 1 : 0;
}


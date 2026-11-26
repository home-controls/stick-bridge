#include "unzip_core.h"

#include <string.h>

enum { UZ_HDR, UZ_NAME, UZ_DATA, UZ_DONE, UZ_FAIL };

uint32_t uz_crc32(uint32_t crc, const uint8_t *d, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

void uz_init(uz_t *z, const uz_inflater_t *inf, uz_out_cb out, void *out_ctx) {
    memset(z, 0, sizeof(*z));
    z->inf = *inf;
    z->out = out;
    z->out_ctx = out_ctx;
    z->st = UZ_HDR;
}

static const char *fail(uz_t *z, const char *e) {
    z->err = e;
    z->st = UZ_FAIL;
    return e;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static const char *emit(uz_t *z, const uint8_t *d, size_t n) {
    if (!n) return NULL;
    z->crc = uz_crc32(z->crc, d, n);
    z->uout += (uint32_t)n;
    if (z->usize && z->uout > z->usize) return fail(z, "the zip's file is bigger than its header says");
    if (z->out && z->out(z->out_ctx, d, n) != 0) return fail(z, "the unpacked data was refused");
    return NULL;
}

const char *uz_feed(uz_t *z, const uint8_t *data, size_t len) {
    size_t i = 0;
    while (i < len && z->st != UZ_FAIL && z->st != UZ_DONE) {
        if (z->st == UZ_HDR) {
            size_t k = 30 - z->hlen;
            if (k > len - i) k = len - i;
            memcpy(z->hdr + z->hlen, data + i, k);
            z->hlen += k;
            i += k;
            if (z->hlen < 30) break;
            if (le32(z->hdr) != 0x04034B50) return fail(z, "not a zip file");
            z->flags = le16(z->hdr + 6);
            z->method = le16(z->hdr + 8);
            z->crc_want = le32(z->hdr + 14);
            z->csize = le32(z->hdr + 18);
            z->usize = le32(z->hdr + 22);
            z->skip = (uint16_t)(le16(z->hdr + 26) + le16(z->hdr + 28));
            z->nlen = 0;
            if (z->flags & 0x0001) return fail(z, "the zip is encrypted");
            if (z->method != 0 && z->method != 8) return fail(z, "the zip uses a compression other than deflate");
            if (z->flags & 0x0008) return fail(z, "the zip streams its sizes after the data (not supported)");
            if (z->method == 0 && z->csize != z->usize) return fail(z, "stored zip entry with mismatched sizes");
            z->st = UZ_NAME;
        } else if (z->st == UZ_NAME) {
            uint16_t name_len = le16(z->hdr + 26);
            while (i < len && z->skip) {
                if (z->nlen < name_len && z->nlen + 1 < sizeof(z->name)) z->name[z->nlen] = (char)data[i];
                if (z->nlen < name_len) z->nlen++;
                z->skip--;
                i++;
            }
            if (z->skip == 0) {
                z->name[z->nlen < sizeof(z->name) ? z->nlen : sizeof(z->name) - 1] = 0;
                z->st = UZ_DATA;
            }
        } else {  // UZ_DATA
            size_t avail = len - i;
            uint32_t left = z->csize - z->cin;
            if (avail > left) avail = left;  // never read past this entry
            if (z->method == 0) {
                if (emit(z, data + i, avail)) return z->err;
                z->cin += (uint32_t)avail;
                i += avail;
                if (z->cin == z->csize) z->st = UZ_DONE;
                continue;
            }
            size_t used = 0, made = 0;
            int r = z->inf.inflate(z->inf.ctx, data + i, avail, &used, z->obuf, sizeof(z->obuf), &made);
            if (r < 0) return fail(z, "the zip's data is damaged");
            if (emit(z, z->obuf, made)) return z->err;
            z->cin += (uint32_t)used;
            i += used;
            if (r == 1) z->st = UZ_DONE;
            else if (used == 0 && made == 0 && avail > 0) return fail(z, "the inflater stalled");
            if (avail == 0 && z->cin == z->csize && r != 1) {
                // all compressed bytes are in: drain what the inflater still holds
                for (int guard = 0; guard < 100000 && z->st == UZ_DATA; guard++) {
                    r = z->inf.inflate(z->inf.ctx, NULL, 0, &used, z->obuf, sizeof(z->obuf), &made);
                    if (r < 0) return fail(z, "the zip's data is damaged");
                    if (emit(z, z->obuf, made)) return z->err;
                    if (r == 1) z->st = UZ_DONE;
                    else if (made == 0) break;
                }
                break;
            }
        }
    }
    // compressed input complete but the stream didn't say "end" yet: drain
    if (z->st == UZ_DATA && z->method == 8 && z->cin == z->csize) {
        for (int guard = 0; guard < 100000 && z->st == UZ_DATA; guard++) {
            size_t used = 0, made = 0;
            int r = z->inf.inflate(z->inf.ctx, NULL, 0, &used, z->obuf, sizeof(z->obuf), &made);
            if (r < 0) return fail(z, "the zip's data is damaged");
            if (emit(z, z->obuf, made)) return z->err;
            if (r == 1) z->st = UZ_DONE;
            else if (made == 0) break;
        }
    }
    return z->err;
}

const char *uz_finish(uz_t *z) {
    if (z->err) return z->err;
    if (z->st != UZ_DONE) return fail(z, "the zip ended early");
    if (z->usize && z->uout != z->usize) return fail(z, "the zip's file came out the wrong size");
    if (z->crc != z->crc_want) return fail(z, "the zip's CRC-32 doesn't match: damaged download");
    return NULL;
}

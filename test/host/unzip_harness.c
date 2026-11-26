// Runs unzip_core.c with zlib as the raw-deflate engine (built by test_unzip.py).
//   unzip_harness <zip> <seed>  -> writes the unpacked file to stdout; exit 0 on success, error on stderr
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "unzip_core.h"

static z_stream zs;
static int inf(void *ctx, const uint8_t *in, size_t in_len, size_t *in_used, uint8_t *out, size_t cap, size_t *made) {
    (void)ctx;
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    zs.next_out = out;
    zs.avail_out = (uInt)cap;
    int r = inflate(&zs, Z_NO_FLUSH);
    *in_used = in_len - zs.avail_in;
    *made = cap - zs.avail_out;
    if (r == Z_STREAM_END) return 1;
    if (r == Z_OK || r == Z_BUF_ERROR) return 0;
    return -1;
}

static int out(void *ctx, const uint8_t *d, size_t n) {
    (void)ctx;
    return fwrite(d, 1, n, stdout) == n ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) return 3;
    fclose(f);
    inflateInit2(&zs, -15);
    uz_t z;
    uz_inflater_t i = {.ctx = NULL, .inflate = inf};
    uz_init(&z, &i, out, NULL);
    srand((unsigned)atoi(argv[2]));
    long off = 0;
    const char *e = NULL;
    while (off < n && !e) {
        long k = 1 + rand() % 3000;
        if (off + k > n) k = n - off;
        e = uz_feed(&z, b + off, (size_t)k);
        off += k;
    }
    if (!e) e = uz_finish(&z);
    free(b);
    if (e) {
        fprintf(stderr, "%s\n", e);
        return 1;
    }
    fprintf(stderr, "ok %s\n", z.name);
    return 0;
}

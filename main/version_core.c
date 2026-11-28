#include "version_core.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    unsigned long num[3];
    const char *pre;  // after '-', up to '+' or the end; NULL for a release
    size_t pre_len;
} ver_t;

#define VER_CAP 1000000000ul  // numbers saturate here: no overflow, and nobody ships version 10^9

static unsigned long add_digit(unsigned long v, char c) {
    return v < VER_CAP / 10 ? v * 10 + (unsigned long)(c - '0') : VER_CAP;
}

static unsigned long digits(const char **s) {
    unsigned long v = 0;
    while (**s >= '0' && **s <= '9') v = add_digit(v, *(*s)++);
    return v;
}

static ver_t parse(const char *s) {
    ver_t v = {{0, 0, 0}, NULL, 0};
    if (!s) return v;
    for (int i = 0; i < 3; i++) {
        v.num[i] = digits(&s);
        if (*s != '.') break;
        s++;
    }
    while (*s && *s != '-' && *s != '+') s++;  // anything else after the numbers is ignored
    if (*s == '-') {
        v.pre = ++s;
        while (*s && *s != '+') s++;
        v.pre_len = (size_t)(s - v.pre);
    }
    return v;
}

// Pre-release suffixes: letters first ("rc" < "rd"), then the number after them ("rc9" < "rc10"), then the rest.
static int pre_cmp(const char *a, size_t al, const char *b, size_t bl) {
    size_t i = 0, j = 0;
    while (i < al && !(a[i] >= '0' && a[i] <= '9')) i++;
    while (j < bl && !(b[j] >= '0' && b[j] <= '9')) j++;
    size_t n = i < j ? i : j;
    int c = memcmp(a, b, n);
    if (c) return c;
    if (i != j) return i < j ? -1 : 1;
    unsigned long x = 0, y = 0;
    for (; i < al && a[i] >= '0' && a[i] <= '9'; i++) x = add_digit(x, a[i]);
    for (; j < bl && b[j] >= '0' && b[j] <= '9'; j++) y = add_digit(y, b[j]);
    if (x != y) return x < y ? -1 : 1;
    size_t ar = al - i, br = bl - j, m = ar < br ? ar : br;
    c = memcmp(a + i, b + j, m);
    if (c) return c;
    return ar == br ? 0 : (ar < br ? -1 : 1);
}

int ver_cmp(const char *a, const char *b) {
    ver_t x = parse(a), y = parse(b);
    for (int i = 0; i < 3; i++) {
        if (x.num[i] != y.num[i]) return x.num[i] < y.num[i] ? -1 : 1;
    }
    if (!x.pre && !y.pre) return 0;
    if (!x.pre) return 1;   // a release is newer than its own release candidates
    if (!y.pre) return -1;
    int c = pre_cmp(x.pre, x.pre_len, y.pre, y.pre_len);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

int ver_is_prerelease(const char *v) { return parse(v).pre != NULL; }

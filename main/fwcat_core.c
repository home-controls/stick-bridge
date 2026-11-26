#include "fwcat_core.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ---------------------------------------------------------------- streaming JSON scanner

void fc_json_init(fc_json_t *j, fc_asset_cb cb, void *ctx) {
    memset(j, 0, sizeof(*j));
    j->cb = cb;
    j->ctx = ctx;
    j->rel.index = -1;
}

static void put_s(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// A scalar value (string, number or literal) finished at the current depth.
static void on_value(fc_json_t *j, const char *v, bool is_str) {
    if (j->depth <= 0 || j->depth > FC_DEPTH || j->kind[j->depth - 1] != '{') return;
    const char *k = j->key[j->depth - 1];
    if (j->depth == j->rel_depth) {
        if (strcmp(k, "tag_name") == 0 && is_str) put_s(j->rel.tag, sizeof(j->rel.tag), v);
        else if (strcmp(k, "prerelease") == 0 && !is_str) j->rel.prerelease = strcmp(v, "true") == 0;
    } else if (j->assets_depth && j->depth == j->assets_depth + 1) {
        if (strcmp(k, "name") == 0 && is_str) put_s(j->asset.name, sizeof(j->asset.name), v);
        else if (strcmp(k, "browser_download_url") == 0 && is_str) put_s(j->asset.url, sizeof(j->asset.url), v);
        else if (strcmp(k, "digest") == 0 && is_str && strncmp(v, "sha256:", 7) == 0 && strlen(v) == 71)
            put_s(j->asset.sha256, sizeof(j->asset.sha256), v + 7);
        else if (strcmp(k, "size") == 0 && !is_str) j->asset.size = (uint32_t)strtoul(v, NULL, 10);
    }
}

static void open_c(fc_json_t *j, char c) {
    if (j->depth == 0) j->rel_depth = (c == '[') ? 2 : 1;  // array of releases, or one release
    if (j->depth >= FC_DEPTH) {
        j->bad = true;
        return;
    }
    // "assets": [ inside a release object
    if (c == '[' && j->depth == j->rel_depth && j->kind[j->depth - 1] == '{' && strcmp(j->key[j->depth - 1], "assets") == 0)
        j->assets_depth = j->depth + 1;
    j->kind[j->depth] = c;
    j->key[j->depth][0] = 0;
    j->depth++;
    if (c == '{' && j->depth == j->rel_depth) {  // new release
        memset(&j->rel.tag, 0, sizeof(j->rel.tag));
        j->rel.prerelease = false;
        j->rel.index++;
    }
    if (c == '{' && j->assets_depth && j->depth == j->assets_depth + 1) memset(&j->asset, 0, sizeof(j->asset));
}

static void close_c(fc_json_t *j) {
    if (j->depth <= 0) {
        j->bad = true;
        return;
    }
    if (j->kind[j->depth - 1] == '{' && j->assets_depth && j->depth == j->assets_depth + 1) {
        if (j->asset.name[0] && j->cb) j->cb(j->ctx, &j->rel, &j->asset);
    }
    if (j->kind[j->depth - 1] == '[' && j->depth == j->assets_depth) j->assets_depth = 0;
    j->depth--;
}

static void end_lit(fc_json_t *j) {
    j->lit[j->llen] = 0;
    on_value(j, j->lit, false);
    j->llen = 0;
    j->st = FC_IDLE;
}

void fc_json_feed(fc_json_t *j, const char *buf, size_t n) {
    for (size_t i = 0; i < n && !j->bad; i++) {
        char c = buf[i];
        switch (j->st) {
            case FC_STR:
                if (c == '\\') {
                    j->st = FC_STR_ESC;
                } else if (c == '"') {
                    j->sval[j->slen] = 0;
                    if (j->str_is_key) {
                        if (j->depth > 0) put_s(j->key[j->depth - 1], sizeof(j->key[0]), j->sval);
                    } else {
                        on_value(j, j->sval, true);
                    }
                    j->st = FC_IDLE;
                } else if (j->slen + 1 < sizeof(j->sval)) {
                    j->sval[j->slen++] = c;
                }
                break;
            case FC_STR_ESC:
                if (c == 'u') {
                    j->st = FC_STR_U;
                    j->u_left = 4;
                } else {
                    char o = c == 'n' ? '\n' : c == 't' ? '\t' : c == 'r' ? '\r' : c == 'b' ? '\b' : c == 'f' ? '\f' : c;
                    if (j->slen + 1 < sizeof(j->sval)) j->sval[j->slen++] = o;
                    j->st = FC_STR;
                }
                break;
            case FC_STR_U:
                if (--j->u_left == 0) {
                    if (j->slen + 1 < sizeof(j->sval)) j->sval[j->slen++] = '?';
                    j->st = FC_STR;
                }
                break;
            case FC_LIT:
                if (isalnum((unsigned char)c) || c == '.' || c == '-' || c == '+') {
                    if (j->llen + 1 < sizeof(j->lit)) j->lit[j->llen++] = c;
                    break;
                }
                end_lit(j);
                __attribute__((fallthrough));  // c still needs handling
            case FC_IDLE:
                if (c == '"') {
                    // a string is a key when it directly follows '{' or ',' inside an object
                    j->str_is_key = j->depth > 0 && j->kind[j->depth - 1] == '{' && !j->after_colon;
                    j->slen = 0;
                    j->st = FC_STR;
                    j->after_colon = false;
                } else if (c == ':') {
                    j->after_colon = true;
                } else if (c == ',') {
                    j->after_colon = false;
                } else if (c == '{' || c == '[') {
                    open_c(j, c);
                    j->after_colon = false;
                } else if (c == '}' || c == ']') {
                    close_c(j);
                    j->after_colon = false;
                } else if (isalnum((unsigned char)c) || c == '-') {
                    j->st = FC_LIT;
                    j->llen = 0;
                    j->lit[j->llen++] = c;
                    j->after_colon = false;
                }
                break;
        }
    }
}

// ---------------------------------------------------------------- names

static const char *const TYPES[] = {"zigbee_ncp", "zigbee_router", "openthread_rcp", "bootloader"};

bool fc_parse_name(const char *name, fc_name_t *o) {
    memset(o, 0, sizeof(*o));
    size_t nl = strlen(name);
    if (nl < 6 || strcasecmp(name + nl - 4, ".gbl") != 0) return false;
    for (size_t t = 0; t < sizeof(TYPES) / sizeof(TYPES[0]); t++) {
        char pat[24];
        snprintf(pat, sizeof(pat), "_%s_", TYPES[t]);
        const char *p = strstr(name, pat);
        if (!p || p == name) continue;
        size_t bl = (size_t)(p - name);
        if (bl >= sizeof(o->board)) return false;
        memcpy(o->board, name, bl);
        o->board[bl] = 0;
        strcpy(o->type, TYPES[t]);
        // rest: sdk_version[_baud_flow].gbl
        char rest[96];
        const char *r = p + strlen(pat);
        size_t rl = (size_t)(name + nl - 4 - r);
        if (rl == 0 || rl >= sizeof(rest)) return false;
        memcpy(rest, r, rl);
        rest[rl] = 0;
        // sdk _ version [_ baud _ flow]; the flow itself has an underscore (sw_flow), so it's the rest
        char *parts[4] = {0};
        int np = 0;
        char *q = rest;
        while (np < 3) {
            parts[np++] = q;
            char *us = strchr(q, '_');
            if (!us) break;
            *us = 0;
            q = us + 1;
            if (np == 3) parts[np++] = q;
        }
        if (np < 2) return false;
        for (int k = 0; k < 2; k++)
            for (const char *q = parts[k]; *q; q++)
                if (!isdigit((unsigned char)*q) && *q != '.') return false;
        snprintf(o->sdk, sizeof(o->sdk), "%s", parts[0]);
        snprintf(o->version, sizeof(o->version), "%s", parts[1]);
        if (t == 3) return np == 2;  // bootloader: no baud/flow
        if (np != 4) return false;
        o->baud = (uint32_t)strtoul(parts[2], NULL, 10);
        if (o->baud < 9600) return false;
        if (strcmp(parts[3], "sw_flow") && strcmp(parts[3], "hw_flow") && strcmp(parts[3], "no_flow")) return false;
        snprintf(o->flow, sizeof(o->flow), "%s", parts[3]);
        return true;
    }
    return false;
}

int fc_vercmp(const char *a, const char *b) {
    while (*a || *b) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        else if (*a) break;
        if (*b == '.') b++;
        else if (*b) break;
    }
    return 0;
}

int fc_score(const fc_name_t *n, const char *board, const char *type, uint32_t bridge_baud, bool rtscts_wired) {
    if (strcasecmp(n->board, board) != 0 || strcmp(n->type, type) != 0) return -1;
    int s = 100;
    if (n->baud == bridge_baud) s += 20;
    if (rtscts_wired) {
        if (strcmp(n->flow, "hw_flow") == 0) s += 10;
    } else {
        if (strcmp(n->flow, "hw_flow") == 0) return -1;  // would wait forever for a CTS that isn't wired
        if (strcmp(n->flow, "sw_flow") == 0) s += 10;
        else if (strcmp(n->flow, "no_flow") == 0) s += 5;
    }
    return s;
}

bool fc_parse_ti_name(const char *name, fc_name_t *o) {
    memset(o, 0, sizeof(*o));
    size_t nl = strlen(name);
    if (nl < 20 || strcasecmp(name + nl - 4, ".zip") != 0) return false;
    static const char *const T[] = {"coordinator", "router"};
    for (int t = 0; t < 2; t++) {
        char pat[24];
        snprintf(pat, sizeof(pat), "_%s_", T[t]);
        const char *p = strstr(name, pat);
        if (!p || p == name) continue;
        const char *date = p + strlen(pat);
        if ((size_t)(name + nl - 4 - date) != 8) return false;
        for (int k = 0; k < 8; k++)
            if (!isdigit((unsigned char)date[k])) return false;
        size_t bl = (size_t)(p - name);
        if (bl >= sizeof(o->board)) return false;
        memcpy(o->board, name, bl);
        o->board[bl] = 0;
        strcpy(o->type, T[t]);
        memcpy(o->version, date, 8);
        o->version[8] = 0;
        o->baud = 115200;
        return true;
    }
    return false;
}

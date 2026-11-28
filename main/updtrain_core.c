#include "updtrain_core.h"

#include <string.h>

#include "version_core.h"

static const char *const NAMES[UT_COUNT] = {"stable", "rc", "custom"};

const char *ut_name(ut_train_t t) { return (unsigned)t < UT_COUNT ? NAMES[t] : "stable"; }

int ut_parse(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < UT_COUNT; i++)
        if (strcmp(name, NAMES[i]) == 0) return i;
    return -1;
}

void ut_pick_init(ut_pick_t *p) { memset(p, 0, sizeof(*p)); }

static const char *bare(const char *tag) { return (tag[0] == 'v' || tag[0] == 'V') ? tag + 1 : tag; }

void ut_consider(ut_pick_t *p, const char *tag, const char *asset_name, const char *asset_url) {
    if (!tag || !asset_name || !asset_url || strcmp(asset_name, "manifest.json") != 0) return;
    const char *v = bare(tag);
    if (!(v[0] >= '0' && v[0] <= '9')) return;                                  // not a version tag
    if (strncmp(asset_url, "https://", 8) != 0) return;                         // updates stay https
    if (strlen(tag) >= sizeof(p->tag) || strlen(asset_url) >= sizeof(p->url)) return;  // never a cut-off address
    if (p->found && ver_cmp(v, bare(p->tag)) <= 0) return;
    p->found = true;
    strcpy(p->tag, tag);
    strcpy(p->url, asset_url);
}

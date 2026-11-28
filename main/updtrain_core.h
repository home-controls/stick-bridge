#pragma once
// Update trains, the parts with no ESP-IDF in them (test/host/test_updtrain.py checks them):
//   stable  the latest release: releases/latest/download/manifest.json (GitHub never serves pre-releases there)
//   rc      release candidates too: the newest version, by version_core.c's order, among the repository's recent
//           releases (GitHub's releases list, read with fwcat_core.c's streaming parser) that have a manifest.json.
//           A release that follows its candidates is newer than all of them, so it is picked up too.
//   custom  the address in Settings (self-hosted updates)

#include <stdbool.h>

typedef enum { UT_STABLE = 0, UT_RC = 1, UT_CUSTOM = 2, UT_COUNT } ut_train_t;

// "stable", "rc", "custom"
const char *ut_name(ut_train_t t);
// The train for a name, or -1.
int ut_parse(const char *name);

typedef struct {
    bool found;
    char tag[48];   // the release's tag, e.g. "v2.2.0-rc2"
    char url[256];  // its manifest.json
} ut_pick_t;

void ut_pick_init(ut_pick_t *p);
// One asset of one release from the releases list. Keeps it when it is a manifest.json on https whose release is
// newer than the one kept so far. Tags may start with 'v'.
void ut_consider(ut_pick_t *p, const char *tag, const char *asset_name, const char *asset_url);

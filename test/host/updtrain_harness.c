// Checks the firmware's own updtrain_core.c, fed through fwcat_core.c's streaming releases parser the way
// updater.c does (test_updtrain.py builds it with ASan/UBSan).
//
//   updtrain_harness selftest

#include <stdio.h>
#include <string.h>

#include "fwcat_core.h"
#include "updtrain_core.h"

static int fails, checks;
#define CHECK(c, ...)                         \
    do {                                      \
        checks++;                             \
        if (!(c)) {                           \
            fails++;                          \
            printf("FAIL %s: ", #c);          \
            printf(__VA_ARGS__);              \
            printf("\n");                     \
        }                                     \
    } while (0)

static void on_asset(void *ctx, const fc_release_t *rel, const fc_asset_t *a) { ut_consider(ctx, rel->tag, a->name, a->url); }

// A releases list in GitHub's shape: newest first, assets with names and download URLs, other fields around them.
static void release(char *out, size_t cap, const char *tag, int pre, const char *assets) {
    char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\"url\":\"https://api.github.com/repos/o/r/releases/1\",\"tag_name\":\"%s\",\"name\":\"Stick Bridge %s\","
             "\"draft\":false,\"prerelease\":%s,\"author\":{\"login\":\"x\",\"id\":1},\"assets\":[%s],"
             "\"body\":\"notes with \\\"quotes\\\" and \\u00e9 and a ] and a }\"}",
             tag, tag, pre ? "true" : "false", assets);
    if (out[0]) strncat(out, ",", cap - strlen(out) - 1);
    strncat(out, buf, cap - strlen(out) - 1);
}

static void asset(char *out, size_t cap, const char *name, const char *url) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s{\"name\":\"%s\",\"size\":474,\"digest\":\"sha256:%064d\",\"browser_download_url\":\"%s\"}",
             out[0] ? "," : "", name, 0, url);
    strncat(out, buf, cap - strlen(out) - 1);
}

static ut_pick_t run(const char *json, int chunk) {
    ut_pick_t p;
    ut_pick_init(&p);
    static fc_json_t j;
    fc_json_init(&j, on_asset, &p);
    size_t n = strlen(json);
    for (size_t i = 0; i < n; i += (size_t)chunk) fc_json_feed(&j, json + i, n - i < (size_t)chunk ? n - i : (size_t)chunk);
    return p;
}

#define BASE "https://github.com/home-controls/stick-bridge/releases/download/"

int main(void) {
    static char list[32768], a[4096];
    // ---- names
    CHECK(ut_parse("stable") == UT_STABLE && ut_parse("rc") == UT_RC && ut_parse("custom") == UT_CUSTOM, "names");
    CHECK(ut_parse("beta") == -1 && ut_parse(NULL) == -1, "unknown names");
    CHECK(strcmp(ut_name(UT_RC), "rc") == 0 && strcmp(ut_name((ut_train_t)9), "stable") == 0, "ut_name");

    // ---- a list like the real one: RCs and releases, a stray file, a release without a manifest
    list[0] = 0;
    a[0] = 0; asset(a, sizeof a, "zigbee_eth_bridge-2.2.0-rc2.bin", BASE "v2.2.0-rc2/zigbee_eth_bridge-2.2.0-rc2.bin");
    asset(a, sizeof a, "manifest.json", BASE "v2.2.0-rc2/manifest.json");
    release(list, sizeof list, "v2.2.0-rc2", 1, a);
    a[0] = 0; asset(a, sizeof a, "manifest.json", BASE "v2.2.0-rc1/manifest.json");
    release(list, sizeof list, "v2.2.0-rc1", 1, a);
    a[0] = 0; asset(a, sizeof a, "manifest.json", BASE "v2.1.3/manifest.json");
    asset(a, sizeof a, "SHA256SUMS", BASE "v2.1.3/SHA256SUMS");
    release(list, sizeof list, "v2.1.3", 0, a);
    a[0] = 0; asset(a, sizeof a, "probe-175615.json", BASE "v2.0.2/probe-175615.json");
    release(list, sizeof list, "v2.0.2", 0, a);
    char json[sizeof list + 4];
    snprintf(json, sizeof json, "[%s]", list);
    int chunks[] = {1, 7, 64, 100000};
    for (size_t i = 0; i < sizeof chunks / sizeof *chunks; i++) {
        ut_pick_t p = run(json, chunks[i]);
        CHECK(p.found && strcmp(p.tag, "v2.2.0-rc2") == 0, "chunk %d: picked %s", chunks[i], p.found ? p.tag : "nothing");
        CHECK(strcmp(p.url, BASE "v2.2.0-rc2/manifest.json") == 0, "chunk %d: url %s", chunks[i], p.url);
    }

    // ---- the release after its candidates wins, even listed below them
    char list2[sizeof list];
    strcpy(list2, list);
    a[0] = 0; asset(a, sizeof a, "manifest.json", BASE "v2.2.0/manifest.json");
    release(list2, sizeof list2, "v2.2.0", 0, a);
    snprintf(json, sizeof json, "[%s]", list2);
    ut_pick_t p = run(json, 13);
    CHECK(p.found && strcmp(p.tag, "v2.2.0") == 0, "release beats its RCs: %s", p.tag);

    // ---- a hotfix of the old line doesn't beat the newer RC
    list2[0] = 0;
    a[0] = 0; asset(a, sizeof a, "manifest.json", BASE "v2.1.4/manifest.json");
    release(list2, sizeof list2, "v2.1.4", 0, a);
    a[0] = 0; asset(a, sizeof a, "manifest.json", BASE "v2.2.0-rc1/manifest.json");
    release(list2, sizeof list2, "v2.2.0-rc1", 1, a);
    snprintf(json, sizeof json, "[%s]", list2);
    p = run(json, 5);
    CHECK(p.found && strcmp(p.tag, "v2.2.0-rc1") == 0, "2.2.0-rc1 over 2.1.4: %s", p.tag);

    // ---- what's refused: no manifest, plain http, a tag that isn't a version, a cut-off address
    ut_pick_init(&p);
    ut_consider(&p, "v9.9.9", "notes.json", BASE "v9.9.9/notes.json");
    ut_consider(&p, "v9.9.9", "manifest.json", "http://example.com/manifest.json");
    ut_consider(&p, "nightly", "manifest.json", "https://example.com/nightly/manifest.json");
    char longurl[300];
    memset(longurl, 'a', sizeof longurl - 1);
    memcpy(longurl, "https://", 8);
    longurl[sizeof longurl - 1] = 0;
    ut_consider(&p, "v9.9.9", "manifest.json", longurl);
    CHECK(!p.found, "nothing usable: found %s", p.tag);
    ut_consider(&p, "2.0.0", "manifest.json", "https://example.com/2.0.0/manifest.json");  // tags without 'v' work
    CHECK(p.found && strcmp(p.tag, "2.0.0") == 0, "bare tag");
    ut_consider(&p, "v1.9.9", "manifest.json", "https://example.com/1.9.9/manifest.json");
    CHECK(strcmp(p.tag, "2.0.0") == 0, "older ignored");

    // ---- an empty list or an API error object finds nothing
    p = run("[]", 1);
    CHECK(!p.found, "empty list");
    p = run("{\"message\":\"API rate limit exceeded\",\"documentation_url\":\"https://docs.github.com\"}", 3);
    CHECK(!p.found, "rate limit answer");

    printf("%d checks, %d failure(s)\n", checks, fails);
    return fails ? 1 : 0;
}

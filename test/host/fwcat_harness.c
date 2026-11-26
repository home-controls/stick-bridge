// Host tests for fwcat_core.c (built by test_fwcat.py with ASan/UBSan).
//   fwcat_harness selftest
//   fwcat_harness scan <file> <seed>   -> one line per asset: idx|tag|pre|name|size|sha256|url
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fwcat_core.h"

static int g_fail;
#define CHECK(c, ...)                                   \
    do {                                                \
        if (!(c)) {                                     \
            printf("FAIL %s:%d: ", __func__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            g_fail++;                                   \
        }                                               \
    } while (0)

static void print_asset(void *ctx, const fc_release_t *r, const fc_asset_t *a) {
    (void)ctx;
    printf("%d|%s|%d|%s|%u|%s|%s\n", r->index, r->tag, r->prerelease, a->name, a->size, a->sha256, a->url);
}

static int scan(const char *path, unsigned seed) {
    FILE *f = fopen(path, "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) return 3;
    fclose(f);
    fc_json_t j;
    fc_json_init(&j, print_asset, NULL);
    srand(seed);
    long off = 0;
    while (off < n) {
        long k = 1 + rand() % 997;
        if (off + k > n) k = n - off;
        fc_json_feed(&j, b + off, (size_t)k);
        off += k;
    }
    free(b);
    if (j.bad || j.depth != 0) {
        fprintf(stderr, "parser state bad=%d depth=%d\n", j.bad, j.depth);
        return 4;
    }
    return 0;
}

static void t_names(void) {
    fc_name_t n;
    CHECK(fc_parse_name("sonoff_dongle-pmg24_zigbee_ncp_2026.6.2_9.1.2_460800_sw_flow.gbl", &n), "pmg24");
    CHECK(!strcmp(n.board, "sonoff_dongle-pmg24") && !strcmp(n.type, "zigbee_ncp") && !strcmp(n.sdk, "2026.6.2") &&
              !strcmp(n.version, "9.1.2") && n.baud == 460800 && !strcmp(n.flow, "sw_flow"),
          "fields %s %s %s %s %u %s", n.board, n.type, n.sdk, n.version, n.baud, n.flow);
    CHECK(fc_parse_name("easyiot_zb-gw04-1v2_zigbee_router_2026.6.2_9.1.2_115200_no_flow.gbl", &n) &&
              !strcmp(n.board, "easyiot_zb-gw04-1v2") && !strcmp(n.type, "zigbee_router"),
          "hyphens in board");
    CHECK(fc_parse_name("smlight_slzb06m_bootloader_2026.6.2_3.2.0.gbl", &n) && !strcmp(n.type, "bootloader") && n.baud == 0,
          "bootloader");
    CHECK(fc_parse_name("smlight_slzb07Mg24-noled_openthread_rcp_2026.6.2_3.1.2_921600_hw_flow.gbl", &n) &&
              !strcmp(n.board, "smlight_slzb07Mg24-noled"),
          "rcp mixed case");
    CHECK(!fc_parse_name("sonoff_zbdonglee_zigbee_ncp_8.0.3.0_sw_flow_115200.gbl", &n), "darkxst order refused");
    CHECK(!fc_parse_name("zigbee_ncp_2026.6.2_9.1.2_460800_sw_flow.gbl", &n), "no board");
    CHECK(!fc_parse_name("x_zigbee_ncp_2026.6.2_9.1.2_460800_odd_flow.gbl", &n), "bad flow");
    CHECK(!fc_parse_name("x_zigbee_ncp_2026.6.2_9.1.2_460800_sw_flow.bin", &n), "not gbl");
}

static void t_ti_names(void) {
    fc_name_t n;
    CHECK(fc_parse_ti_name("CC1352P2_CC2652P_launchpad_coordinator_20250321.zip", &n) &&
              !strcmp(n.board, "CC1352P2_CC2652P_launchpad") && !strcmp(n.type, "coordinator") && !strcmp(n.version, "20250321"),
          "launchpad %s %s %s", n.board, n.type, n.version);
    CHECK(fc_parse_ti_name("CC2652RB_router_20250403.zip", &n) && !strcmp(n.board, "CC2652RB") && !strcmp(n.type, "router"), "router");
    CHECK(!fc_parse_ti_name("CC2652R_coordinator_2025032.zip", &n), "7-digit date");
    CHECK(!fc_parse_ti_name("CC2652R_coordinator_20250321.hex", &n), "not zip");
    CHECK(fc_vercmp("20250321", "20240710") > 0, "dates compare");
}

static void t_vercmp(void) {
    CHECK(fc_vercmp("9.1.2", "9.1.2.0") == 0, "trailing zero");
    CHECK(fc_vercmp("9.1.1.0", "9.1.2") < 0, "older");
    CHECK(fc_vercmp("9.1.10", "9.1.2") > 0, "numeric not lexical");
    CHECK(fc_vercmp("8.2.2.0", "9.1.2") < 0, "major");
}

static void t_score(void) {
    fc_name_t sw, hw, no, other;
    fc_parse_name("sonoff_dongle-pmg24_zigbee_ncp_2026.6.2_9.1.2_460800_sw_flow.gbl", &sw);
    fc_parse_name("sonoff_dongle-pmg24_zigbee_ncp_2026.6.2_9.1.2_460800_hw_flow.gbl", &hw);
    fc_parse_name("sonoff_dongle-pmg24_zigbee_ncp_2026.6.2_9.1.2_115200_no_flow.gbl", &no);
    fc_parse_name("sonoff_zbdonglee_zigbee_ncp_2026.6.2_9.1.2_115200_sw_flow.gbl", &other);
    CHECK(fc_score(&hw, "sonoff_dongle-pmg24", "zigbee_ncp", 460800, false) < 0, "hw_flow on unwired board refused");
    CHECK(fc_score(&other, "sonoff_dongle-pmg24", "zigbee_ncp", 460800, false) < 0, "other board refused");
    CHECK(fc_score(&sw, "sonoff_dongle-pmg24", "zigbee_ncp", 460800, false) >
              fc_score(&no, "sonoff_dongle-pmg24", "zigbee_ncp", 460800, false),
          "same baud + sw_flow wins");
    CHECK(fc_score(&hw, "SONOFF_DONGLE-PMG24", "zigbee_ncp", 460800, true) > fc_score(&sw, "sonoff_dongle-pmg24", "zigbee_ncp", 460800, true),
          "hw_flow preferred when wired, board case-insensitive");
    CHECK(fc_score(&sw, "sonoff_dongle-pmg24", "zigbee_router", 460800, false) < 0, "type must match");
}

static void t_small_json(void) {
    // single release object, escapes, nested uploader with its own "name"-less fields, numbers/literals
    const char *js = "{\"tag_name\":\"v1\\\"x\",\"prerelease\":false,\"name\":\"rel \\u00e9\",\"assets\":[{\"name\":\"a_zigbee_ncp_1.0_2.0_115200_sw_flow.gbl\","
                     "\"uploader\":{\"login\":\"bot\",\"id\":5,\"site_admin\":false},\"size\":123,\"digest\":null,"
                     "\"browser_download_url\":\"https://x/a.gbl\"}],\"body\":\"[not] {json} \\\\\"}";
    fc_json_t j;
    fc_json_init(&j, NULL, NULL);
    fc_json_feed(&j, js, strlen(js));
    CHECK(!j.bad && j.depth == 0, "balanced (bad %d depth %d)", j.bad, j.depth);
    CHECK(!strcmp(j.rel.tag, "v1\"x"), "tag with escape: %s", j.rel.tag);
}

int main(int argc, char **argv) {
    if (argc >= 4 && strcmp(argv[1], "scan") == 0) return scan(argv[2], (unsigned)atoi(argv[3]));
    if (argc < 2 || strcmp(argv[1], "selftest") != 0) return 2;
    t_names();
    t_vercmp();
    t_ti_names();
    t_score();
    t_small_json();
    printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}

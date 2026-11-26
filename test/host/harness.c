// Host harness: the firmware's own ezsp_decode.c + stats.c, fed a recorded conversation.
//   harness <trace.bin> <seed>   (seed 0 = byte by byte, otherwise random chunk sizes 1..300)
// Trace records: dir(1) t_us(8, LE) len(2, LE) bytes. Prints the resulting stats as JSON.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "ezsp_decode.h"
#include "stats.h"

int64_t g_now_us;
esp_timer_cb_t g_tick_cb;
int g_lock_depth, g_lock_violations;

static void ash(const char *name, const ash_dir_stats_t *a, int last) {
    printf("\"%s\":{\"bytes\":%u,\"frames\":%u,\"data\":%u,\"ack\":%u,\"nak\":%u,\"rst\":%u,\"rstack\":%u,\"error\":%u,"
           "\"retx\":%u,\"crc_err\":%u,\"cancel\":%u,\"substitute\":%u,\"oversize\":%u}%s",
           name, a->bytes, a->frames, a->data, a->ack, a->nak, a->rst, a->rstack, a->error, a->retx, a->crc_err,
           a->cancel, a->substitute, a->oversize, last ? "" : ",");
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    unsigned seed = (unsigned)atoi(argv[2]);
    srand(seed);
    stats_begin();
    ezsp_decode_reset();
    uint8_t buf[70000];
    int64_t next_tick = 1000000;
    for (;;) {
        uint8_t dir;
        int64_t t;
        uint16_t len;
        if (fread(&dir, 1, 1, f) != 1) break;
        if (fread(&t, 8, 1, f) != 1 || fread(&len, 2, 1, f) != 1 || fread(buf, 1, len, f) != len) return 3;
        g_now_us = t;
        while (g_now_us >= next_tick && g_tick_cb) {  // the 1 s history tick
            g_tick_cb(NULL);
            next_tick += 1000000;
        }
        size_t off = 0;
        while (off < len) {
            size_t n = seed == 0 ? 1 : seed == 99999 ? len : 1 + (size_t)(rand() % 300);
            if (n > len - off) n = len - off;
            ezsp_decode_feed((ash_dir_t)dir, buf + off, n, t);
            off += n;
        }
    }
    fclose(f);
    if (g_tick_cb) g_tick_cb(NULL);

    bridge_stats_t *g = &g_stats;
    printf("{\"lock_violations\":%d,", g_lock_violations);
    ash("host", &g->host, 0);
    ash("ncp", &g->ncp, 0);
    printf("\"ezsp_version\":%u,\"stack_type\":%u,\"stack_version\":%u,\"last_reset_code\":%u,\"last_error_code\":%u,",
           g->ezsp_version, g->stack_type, g->stack_version, g->last_reset_code, g->last_error_code);
    printf("\"network_up_known\":%d,\"network_up\":%d,", g->network_up_known, g->network_up);
    printf("\"ezsp_cmds\":%u,\"ezsp_responses\":%u,\"ezsp_callbacks\":%u,\"zb_rx\":%u,\"zb_unicast\":%u,\"zb_broadcast\":%u,"
           "\"zb_multicast\":%u,\"zb_sent_ok\":%u,\"zb_sent_fail\":%u,\"route_errors\":%u,\"net_status\":%u,\"joins\":%u,"
           "\"leaves\":%u,\"id_conflicts\":%u,",
           g->ezsp_cmds, g->ezsp_responses, g->ezsp_callbacks, g->zb_rx, g->zb_unicast, g->zb_broadcast, g->zb_multicast,
           g->zb_sent_ok, g->zb_sent_fail, g->route_errors, g->net_status_events, g->joins, g->leaves, g->id_conflicts);
    printf("\"rtt_n\":%u,\"rtt_sum_us\":%llu,\"rtt_max_us\":%u,\"rtt_hist\":[", g->rtt_n, (unsigned long long)g->rtt_sum_us,
           g->rtt_max_us);
    for (int i = 0; i < RTT_BUCKETS; i++) printf("%u%s", g->rtt_hist[i], i + 1 < RTT_BUCKETS ? "," : "");
    printf("],\"ack_n\":%u,\"ack_sum_us\":%llu,\"ack_max_us\":%u,", g->ack_n, (unsigned long long)g->ack_sum_us, g->ack_max_us);

    static zb_device_t dv[DEV_MAX];
    int n = stats_copy_devices(dv, DEV_MAX);
    printf("\"devices\":[");
    for (int i = 0; i < n; i++) {
        const zb_device_t *d = &dv[i];
        printf("{\"short\":%u,\"ieee\":\"%02x%02x%02x%02x%02x%02x%02x%02x\",\"rx\":%u,\"tx_ok\":%u,\"tx_fail\":%u,"
               "\"route_err\":%u,\"lqi\":%u,\"rssi\":%d,\"last_fail_status\":%u}%s",
               d->short_id, d->ieee[7], d->ieee[6], d->ieee[5], d->ieee[4], d->ieee[3], d->ieee[2], d->ieee[1], d->ieee[0],
               d->rx, d->tx_ok, d->tx_fail, d->route_err, d->last_lqi, d->last_rssi, d->last_fail_status,
               i + 1 < n ? "," : "");
    }
    static event_t ev[EV_LEN];
    int ne = stats_copy_events(ev, EV_LEN, 0);
    printf("],\"events\":[");
    for (int i = 0; i < ne; i++) {
        printf("{\"level\":%u,\"text\":\"", ev[i].level);
        for (const char *p = ev[i].text; *p; p++) {
            if (*p == '"' || *p == '\\') putchar('\\');
            putchar(*p);
        }
        printf("\"}%s", i + 1 < ne ? "," : "");
    }
    static hist_t hs[HIST_LEN];
    int nh = stats_copy_history(hs, HIST_LEN);
    unsigned rx = 0, ok = 0, fail = 0, in = 0, out = 0;
    for (int i = 0; i < nh; i++) {
        rx += hs[i].zb_rx;
        ok += hs[i].zb_tx_ok;
        fail += hs[i].zb_tx_fail;
        in += hs[i].bps_in;
        out += hs[i].bps_out;
    }
    printf("],\"hist\":{\"n\":%d,\"rx\":%u,\"ok\":%u,\"fail\":%u,\"in\":%u,\"out\":%u}}\n", nh, rx, ok, fail, in, out);
    return 0;
}

#include "stats.h"

#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

bridge_stats_t g_stats;
radio_stats_t g_radio[RADIO_MAX];

static SemaphoreHandle_t s_lock;
// The dashboard's rings and the device table live in PSRAM (internal RAM is what the watchdog guards); plain heap
// on a board without it. Allocated once in stats_begin.
static hist_t *s_hist;
static int s_hist_head, s_hist_count;
// 10-second points for 24 h, folded from the per-second ring (PSRAM; NULL without it)
static hist_t *s_lhist;
static int s_lhist_head, s_lhist_count;
static struct {
    uint32_t in, out, rx, ok, fail, cmds, err, n;
    uint64_t rtt_w;  // sum of rtt_avg_x10 * commands
    uint32_t rtt_cmds, rtt_max;
    uint32_t r_in[RADIO_MAX], r_out[RADIO_MAX];
} s_lacc;
static event_t *s_ev;
static int s_ev_head, s_ev_count;
static uint32_t s_ev_next_id = 1;
static zb_device_t *s_dev;
static int s_dev_count;

// Rolling windows for the health score (last 5 minutes, from history).
static const uint32_t RTT_EDGES_US[RTT_BUCKETS - 1] = {1000,   2000,   5000,   10000,  20000, 50000,
                                                        100000, 200000, 500000, 1000000, 2000000};

uint32_t stats_uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

void stats_lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
void stats_unlock(void) { xSemaphoreGive(s_lock); }

static void tick(void *arg) {
    (void)arg;
    stats_lock();
    bridge_stats_t *g = &g_stats;
    hist_t *h = &s_hist[s_hist_head];
    h->t = stats_uptime_s();
    h->bps_in = g->acc_in > 0xFFFF ? 0xFFFF : g->acc_in;
    h->bps_out = g->acc_out > 0xFFFF ? 0xFFFF : g->acc_out;
    h->zb_rx = g->acc_rx;
    h->zb_tx_ok = g->acc_ok;
    h->zb_tx_fail = g->acc_fail;
    h->ezsp_cmds = g->acc_cmds;
    h->rtt_avg_x10 = g->acc_rtt_n ? (uint16_t)(g->acc_rtt_sum / g->acc_rtt_n / 100) : 0;
    uint32_t mx = g->acc_rtt_max / 100;
    h->rtt_max_x10 = mx > 0xFFFF ? 0xFFFF : mx;
    h->ash_err = g->acc_err;
    for (int i = 0; i < RADIO_MAX; i++) {
        h->r_in[i] = g_radio[i].acc_in > 0xFFFF ? 0xFFFF : (uint16_t)g_radio[i].acc_in;
        h->r_out[i] = g_radio[i].acc_out > 0xFFFF ? 0xFFFF : (uint16_t)g_radio[i].acc_out;
    }
    s_hist_head = (s_hist_head + 1) % HIST_LEN;
    if (s_hist_count < HIST_LEN) s_hist_count++;
    if (s_lhist) {
        s_lacc.in += h->bps_in;
        s_lacc.out += h->bps_out;
        s_lacc.rx += h->zb_rx;
        s_lacc.ok += h->zb_tx_ok;
        s_lacc.fail += h->zb_tx_fail;
        s_lacc.cmds += h->ezsp_cmds;
        s_lacc.err += h->ash_err;
        for (int i = 0; i < RADIO_MAX; i++) {
            s_lacc.r_in[i] += h->r_in[i];
            s_lacc.r_out[i] += h->r_out[i];
        }
        s_lacc.n++;
        if (h->rtt_avg_x10) {
            uint32_t c = g->acc_rtt_n ? g->acc_rtt_n : 1;
            s_lacc.rtt_w += (uint64_t)h->rtt_avg_x10 * c;
            s_lacc.rtt_cmds += c;
        }
        if (h->rtt_max_x10 > s_lacc.rtt_max) s_lacc.rtt_max = h->rtt_max_x10;
        // a point per whole 10 s of uptime (aligned, so the dashboard's buckets never shift)
        if (h->t % LHIST_STEP == 0 && s_lacc.n) {
            hist_t *l = &s_lhist[s_lhist_head];
            uint32_t n = s_lacc.n;
#define CLAMP16(v) ((v) > 0xFFFF ? 0xFFFF : (uint16_t)(v))
            l->t = h->t;
            l->bps_in = CLAMP16(s_lacc.in / n);
            l->bps_out = CLAMP16(s_lacc.out / n);
            l->zb_rx = CLAMP16(s_lacc.rx);
            l->zb_tx_ok = CLAMP16(s_lacc.ok);
            l->zb_tx_fail = CLAMP16(s_lacc.fail);
            l->ezsp_cmds = CLAMP16(s_lacc.cmds);
            l->ash_err = CLAMP16(s_lacc.err);
            l->rtt_avg_x10 = s_lacc.rtt_cmds ? CLAMP16(s_lacc.rtt_w / s_lacc.rtt_cmds) : 0;
            l->rtt_max_x10 = CLAMP16(s_lacc.rtt_max);
            for (int i = 0; i < RADIO_MAX; i++) {
                l->r_in[i] = CLAMP16(s_lacc.r_in[i] / n);
                l->r_out[i] = CLAMP16(s_lacc.r_out[i] / n);
            }
#undef CLAMP16
            s_lhist_head = (s_lhist_head + 1) % LHIST_LEN;
            if (s_lhist_count < LHIST_LEN) s_lhist_count++;
            memset(&s_lacc, 0, sizeof(s_lacc));
        }
    }
    g->acc_in = g->acc_out = g->acc_rx = g->acc_ok = g->acc_fail = g->acc_cmds = g->acc_err = 0;
    for (int i = 0; i < RADIO_MAX; i++) {
        radio_stats_t *r = &g_radio[i];
        r->bps_in = r->acc_in;
        r->bps_out = r->acc_out;
        r->acc_in = r->acc_out = 0;
    }
    g->acc_rtt_sum = 0;
    g->acc_rtt_n = g->acc_rtt_max = 0;
    stats_unlock();
}

static void *big(size_t n, size_t size) {
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = calloc(n, size);
    if (!p) abort();  // at boot: nothing works without these
    return p;
}

void stats_begin(void) {
    s_lock = xSemaphoreCreateMutex();
    s_hist = big(HIST_LEN, sizeof(hist_t));
    s_ev = big(EV_LEN, sizeof(event_t));
    s_dev = big(DEV_MAX, sizeof(zb_device_t));
    s_lhist = heap_caps_calloc(LHIST_LEN, sizeof(hist_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    memset(&g_stats, 0, sizeof(g_stats));
    const esp_timer_create_args_t args = {.callback = tick, .name = "stats_tick"};
    esp_timer_handle_t t;
    esp_timer_create(&args, &t);
    esp_timer_start_periodic(t, 1000000);
}

static void event_v(int radio, ev_level_t level, const char *fmt, va_list ap) {
    event_t e = {0};
    e.level = level;
    e.up_s = stats_uptime_s();
    struct timeval tv;
    gettimeofday(&tv, NULL);
    e.epoch = tv.tv_sec > 1700000000 ? tv.tv_sec : 0;
    vsnprintf(e.text, sizeof(e.text), fmt, ap);
    // "Radio 3: ..." / "Radio 3 firmware ..." belongs to radio 3 (index 2)
    if (radio == -1 && strncmp(e.text, "Radio ", 6) == 0 && e.text[6] >= '1' && e.text[6] < '1' + RADIO_MAX &&
        (e.text[7] == ':' || e.text[7] == ' ' || e.text[7] == 0))
        radio = e.text[6] - '1';
    e.radio = (int8_t)(radio >= 0 && radio < RADIO_MAX ? radio : -1);
    stats_lock();
    e.id = s_ev_next_id++;
    s_ev[s_ev_head] = e;
    s_ev_head = (s_ev_head + 1) % EV_LEN;
    if (s_ev_count < EV_LEN) s_ev_count++;
    stats_unlock();
}

void stats_event(ev_level_t level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    event_v(-1, level, fmt, ap);
    va_end(ap);
}

void stats_event_r(int radio, ev_level_t level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    event_v(radio < 0 ? -2 : radio, level, fmt, ap);  // -2: explicitly the bridge, no prefix guessing
    va_end(ap);
}

zb_device_t *stats_device(uint16_t short_id, bool create) {
    for (int i = 0; i < s_dev_count; i++) {
        if (s_dev[i].short_id == short_id) return &s_dev[i];
    }
    if (!create) return NULL;
    int slot;
    if (s_dev_count < DEV_MAX) {
        slot = s_dev_count++;
    } else {
        // Full: reuse the device heard least recently.
        slot = 0;
        for (int i = 1; i < DEV_MAX; i++) {
            if (s_dev[i].last_seen_s < s_dev[slot].last_seen_s) slot = i;
        }
    }
    memset(&s_dev[slot], 0, sizeof(s_dev[slot]));
    s_dev[slot].short_id = short_id;
    s_dev[slot].first_seen_s = stats_uptime_s();
    return &s_dev[slot];
}

void stats_add_rtt(uint32_t us) {
    bridge_stats_t *g = &g_stats;
    g->rtt_sum_us += us;
    g->rtt_n++;
    if (us > g->rtt_max_us) g->rtt_max_us = us;
    int b = 0;
    while (b < RTT_BUCKETS - 1 && us >= RTT_EDGES_US[b]) b++;
    g->rtt_hist[b]++;
    g->acc_rtt_sum += us;
    g->acc_rtt_n++;
    if (us > g->acc_rtt_max) g->acc_rtt_max = us;
}

void stats_reset_counters(void) {
    stats_lock();
    bridge_stats_t *g = &g_stats;
    memset(&g->host, 0, sizeof(g->host));
    memset(&g->ncp, 0, sizeof(g->ncp));
    g->ezsp_cmds = g->ezsp_responses = g->ezsp_callbacks = 0;
    g->zb_rx = g->zb_unicast = g->zb_broadcast = g->zb_multicast = g->zb_sent_ok = g->zb_sent_fail = 0;
    g->route_errors = g->net_status_events = g->joins = g->leaves = g->id_conflicts = 0;
    g->rtt_sum_us = 0;
    g->rtt_n = g->rtt_max_us = 0;
    memset(g->rtt_hist, 0, sizeof(g->rtt_hist));
    g->ack_sum_us = 0;
    g->ack_n = g->ack_max_us = 0;
    g->clients_accepted = g->clients_refused = g->client_drops = 0;
    g->dropped_no_client = g->dropped_overflow = g->dongle_tx_errors = 0;
    g->dongle_attach = g->dongle_detach = 0;
    g->stats_since_s = stats_uptime_s();
    for (int i = 0; i < RADIO_MAX; i++) {
        radio_stats_t *r = &g_radio[i];
        r->attach = r->detach = 0;
        r->clients_accepted = r->clients_refused = r->client_drops = 0;
        r->bytes_in = r->bytes_out = 0;
        r->dropped_no_client = r->dropped_overflow = r->tx_errors = 0;
    }
    s_dev_count = 0;
    s_hist_count = 0;
    s_hist_head = 0;
    s_lhist_count = 0;
    s_lhist_head = 0;
    memset(&s_lacc, 0, sizeof(s_lacc));
    stats_unlock();
    stats_event(EV_INFO, "Statistics reset");
}

int stats_copy_history(hist_t *out, int max) {
    stats_lock();
    int n = s_hist_count < max ? s_hist_count : max;
    int start = (s_hist_head - n + HIST_LEN) % HIST_LEN;
    for (int i = 0; i < n; i++) out[i] = s_hist[(start + i) % HIST_LEN];
    stats_unlock();
    return n;
}

int stats_long_history_len(void) { return s_lhist ? LHIST_LEN : 0; }

int stats_copy_long_history(hist_t *out, int max) {
    if (!s_lhist) return 0;
    stats_lock();
    int n = s_lhist_count < max ? s_lhist_count : max;
    int start = (s_lhist_head - n + LHIST_LEN) % LHIST_LEN;
    for (int i = 0; i < n; i++) out[i] = s_lhist[(start + i) % LHIST_LEN];
    stats_unlock();
    return n;
}

int stats_copy_events(event_t *out, int max, uint32_t since_id) {
    stats_lock();
    int n = 0;
    int start = (s_ev_head - s_ev_count + EV_LEN) % EV_LEN;
    for (int i = 0; i < s_ev_count && n < max; i++) {
        const event_t *e = &s_ev[(start + i) % EV_LEN];
        if (e->id > since_id) out[n++] = *e;
    }
    stats_unlock();
    return n;
}

int stats_copy_devices(zb_device_t *out, int max) {
    stats_lock();
    int n = s_dev_count < max ? s_dev_count : max;
    memcpy(out, s_dev, n * sizeof(zb_device_t));
    stats_unlock();
    return n;
}

int stats_health(char *reason, int reason_len) {
    // Sums over the last 5 minutes of history plus live state.
    uint32_t err = 0, ok = 0, fail = 0, rtt_sum = 0, rtt_n = 0, secs = 0;
    stats_lock();
    int n = s_hist_count < 300 ? s_hist_count : 300;
    for (int i = 0; i < n; i++) {
        const hist_t *h = &s_hist[(s_hist_head - 1 - i + HIST_LEN) % HIST_LEN];
        err += h->ash_err;
        ok += h->zb_tx_ok;
        fail += h->zb_tx_fail;
        if (h->rtt_avg_x10) {
            rtt_sum += h->rtt_avg_x10;
            rtt_n++;
        }
        secs++;
    }
    bool dongle = g_stats.dongle_open, client = g_stats.client_connected, link = g_stats.eth_link;
    bool net_down = g_stats.network_up_known && !g_stats.network_up;
    bool present[RADIO_MAX];
    for (int i = 0; i < RADIO_MAX; i++) present[i] = g_radio[i].present;
    stats_unlock();
    uint8_t role0 = settings_radio_role(0);
    int missing = -1;  // the first set-up radio (2-4) whose stick is gone
    for (int i = 1; i < RADIO_MAX && missing < 0; i++)
        if (!present[i] && settings_radio_is_set_up(i)) missing = i;
    static char missing_msg[48];
    if (missing > 0) snprintf(missing_msg, sizeof(missing_msg), "Radio %d: stick missing", missing + 1);

    int score = 100;
    const char *why = "All good";
    int worst = 0;
#define DEDUCT(pts, msg)          \
    do {                          \
        int _p = (pts);           \
        score -= _p;              \
        if (_p > worst) {         \
            worst = _p;           \
            why = (msg);          \
        }                         \
    } while (0)
    if (!link) DEDUCT(100, "Ethernet link is down");
    if (!dongle) DEDUCT(60, "Radio 1: no stick connected");
    if (!client)
        DEDUCT(40, role0 == RADIO_ROLE_THREAD  ? "Radio 1: border router not connected"
                   : role0 == RADIO_ROLE_ZWAVE ? "Radio 1: Z-Wave JS not connected"
                                               : "Zigbee2MQTT not connected");
    if (missing > 0) DEDUCT(15, missing_msg);
    if (net_down) DEDUCT(50, "Coordinator reports the Zigbee network down");
    if (secs > 0) {
        uint32_t per_min = err * 60 / secs;
        if (per_min > 0) DEDUCT(per_min > 20 ? 30 : (int)per_min + 2, "Serial link errors (CRC/NAK/retransmit)");
    }
    if (ok + fail >= 10) {
        uint32_t pct_fail = fail * 100 / (ok + fail);
        if (pct_fail >= 2) DEDUCT(pct_fail > 30 ? 35 : (int)pct_fail, "Zigbee delivery failures");
    }
    if (rtt_n > 0) {
        uint32_t avg_ms = rtt_sum / rtt_n / 10;
        if (avg_ms > 30) DEDUCT(avg_ms > 200 ? 20 : (int)(avg_ms / 10), "Slow coordinator responses");
    }
#undef DEDUCT
    if (score < 0) score = 0;
    if (reason) snprintf(reason, reason_len, "%s", why);
    return score;
}

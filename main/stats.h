#pragma once
// Everything the dashboard shows: live state, lifetime counters, a 15-minute per-second history,
// an event log and the table of Zigbee devices the coordinator heard. All of it is guarded by
// stats_lock()/stats_unlock(); writers are the bridge tasks, readers the web server.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#define HIST_LEN 900  // seconds of per-second history
#define LHIST_STEP 10   // seconds per point of the long history
#define LHIST_LEN 8640  // 24 h of 10-second points (in PSRAM)
#define EV_LEN 160    // event log entries
#define DEV_MAX 192   // Zigbee devices tracked
#define RTT_BUCKETS 12

typedef enum { EV_INFO = 0, EV_WARN = 1, EV_ERROR = 2 } ev_level_t;

typedef struct {
    uint32_t bytes, frames, data, ack, nak, rst, rstack, error, retx, crc_err, cancel, substitute, oversize;
} ash_dir_stats_t;

typedef struct {
    uint16_t short_id;
    uint8_t ieee[8];  // little-endian as on the wire; all zero when unknown
    uint32_t rx, tx_ok, tx_fail, route_err;
    uint8_t last_lqi;
    int8_t last_rssi;
    float rssi_avg, lqi_avg;
    uint32_t first_seen_s, last_seen_s, last_fail_s;  // uptime seconds
    uint32_t last_fail_status;
} zb_device_t;

typedef struct {
    uint32_t t;                      // uptime s
    uint16_t bps_in, bps_out;        // bytes/s from / to the dongle
    uint16_t zb_rx, zb_tx_ok, zb_tx_fail, ezsp_cmds;
    uint16_t rtt_avg_x10, rtt_max_x10;  // EZSP command round trip, ms * 10
    uint16_t ash_err;                // CRC + NAK + retransmit + cancel in that second
} hist_t;

typedef struct {
    uint32_t id;
    uint32_t up_s;
    int64_t epoch;  // 0 until SNTP synced
    uint8_t level;
    char text[112];
} event_t;

typedef struct {
    // live state
    bool eth_link, eth_has_ip;
    int eth_speed, eth_full_duplex;
    uint32_t ip, mask, gw;
    uint8_t mac[6];

    bool dongle_present, dongle_open;
    char dongle_mfr[48], dongle_product[48], dongle_serial[48];
    uint16_t dongle_vid, dongle_pid;
    uint32_t dongle_since_s;

    bool client_connected;
    uint32_t client_ip;
    uint16_t client_port;
    uint32_t client_since_s;

    // NCP identity (from the EZSP version exchange / RSTACK)
    uint8_t ezsp_version, stack_type;
    uint16_t stack_version;
    uint8_t last_reset_code, last_error_code;
    uint32_t network_status;  // last stackStatusHandler status (0x90 up, 0x91 down)
    bool network_up_known, network_up;

    // lifetime counters (since boot or the last reset)
    ash_dir_stats_t host, ncp;  // host = Z2M -> dongle, ncp = dongle -> Z2M
    uint32_t ezsp_cmds, ezsp_responses, ezsp_callbacks;
    uint32_t zb_rx, zb_unicast, zb_broadcast, zb_multicast, zb_sent_ok, zb_sent_fail;
    uint32_t route_errors, net_status_events, joins, leaves, id_conflicts;
    uint64_t rtt_sum_us;
    uint32_t rtt_n, rtt_max_us;
    uint32_t rtt_hist[RTT_BUCKETS];
    uint64_t ack_sum_us;
    uint32_t ack_n, ack_max_us;
    uint32_t clients_accepted, clients_refused, client_drops;
    uint32_t dropped_no_client, dropped_overflow, dongle_tx_errors;
    uint32_t dongle_attach, dongle_detach;
    uint32_t stats_since_s;

    // per-second accumulators, folded into history every second
    uint32_t acc_in, acc_out, acc_rx, acc_ok, acc_fail, acc_cmds, acc_err;
    uint64_t acc_rtt_sum;
    uint32_t acc_rtt_n, acc_rtt_max;
} bridge_stats_t;

extern bridge_stats_t g_stats;

void stats_begin(void);
void stats_lock(void);
void stats_unlock(void);
uint32_t stats_uptime_s(void);

// Takes the lock itself; do not call while holding it.
void stats_event(ev_level_t level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
// Caller holds the lock.
zb_device_t *stats_device(uint16_t short_id, bool create);
void stats_add_rtt(uint32_t us);
void stats_reset_counters(void);

// Copies for the web server (take the lock themselves).
int stats_copy_history(hist_t *out, int max);              // oldest first
// The last n points of the 10-second history (24 h), oldest first. Sums per 10 s for counts, average
// bytes/s, command-weighted average RTT and the slowest answer. 0 when PSRAM isn't there.
int stats_copy_long_history(hist_t *out, int max);
int stats_long_history_len(void);
int stats_copy_events(event_t *out, int max, uint32_t since_id);
int stats_copy_devices(zb_device_t *out, int max);

// 0-100 plus a short reason for the biggest deduction.
int stats_health(char *reason, int reason_len);

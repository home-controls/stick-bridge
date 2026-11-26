#include "ezsp_decode.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "stats.h"

// ASH reserved bytes (zigbee-herdsman uart/enums.ts)
#define ASH_FLAG 0x7E
#define ASH_ESCAPE 0x7D
#define ASH_XON 0x11
#define ASH_XOFF 0x13
#define ASH_SUBSTITUTE 0x18
#define ASH_CANCEL 0x1A
#define ASH_WAKE 0xFF
#define ASH_FLIP 0x20

#define ASH_MAX_FRAME 260  // control + up to 220 data + CRC, with margin

// EZSP frame IDs (ezsp/enums.ts)
#define EZSP_VERSION 0x0000
#define EZSP_STACK_STATUS_HANDLER 0x0019
#define EZSP_CHILD_JOIN_HANDLER 0x0023
#define EZSP_TRUST_CENTER_JOIN_HANDLER 0x0024
#define EZSP_SEND_UNICAST 0x0034
#define EZSP_SEND_BROADCAST 0x0036
#define EZSP_SEND_MULTICAST 0x0038
#define EZSP_MESSAGE_SENT_HANDLER 0x003F
#define EZSP_INCOMING_MESSAGE_HANDLER 0x0045
#define EZSP_ID_CONFLICT_HANDLER 0x007C
#define EZSP_INCOMING_ROUTE_ERROR_HANDLER 0x0080
#define EZSP_INCOMING_NETWORK_STATUS_HANDLER 0x00C4

// stackStatusHandler: from EZSP v14 (EmberZNet 8) an sl_status_t (4 bytes), before that one EmberStatus
// byte (zigbee-herdsman ember/ezsp/buffalo.ts readStatus).
#define EZSP_SL_STATUS_SINCE 14
#define SL_STATUS_NETWORK_UP 0x0015
#define SL_STATUS_NETWORK_DOWN 0x0016
#define EMBER_NETWORK_UP 0x90
#define EMBER_NETWORK_DOWN 0x91

typedef struct {
    uint8_t buf[ASH_MAX_FRAME];
    size_t len;
    bool escape, bad, overflow;
} ash_rx_t;

static ash_rx_t s_rx[2];
// ASH ack latency: host DATA frmNum -> time sent (0 = none outstanding)
static int64_t s_ack_pending[8];
// EZSP round trip: host command sequence -> time sent
static int64_t s_cmd_pending[256];
static uint16_t s_cmd_frame[256];

void ezsp_decode_reset(void) {
    memset(s_rx, 0, sizeof(s_rx));
    memset(s_ack_pending, 0, sizeof(s_ack_pending));
    memset(s_cmd_pending, 0, sizeof(s_cmd_pending));
}

static uint16_t crc16_ccitt(const uint8_t *d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}

static void derandomize(uint8_t *d, size_t n) {
    uint8_t seed = 0x42;
    for (size_t i = 0; i < n; i++) {
        d[i] ^= seed;
        seed = (seed & 1) ? (uint8_t)((seed >> 1) ^ 0xB8) : (uint8_t)(seed >> 1);
    }
}

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void fmt_ieee(char *out, const uint8_t *le) {
    static const char hx[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 8; i++) {
        out[2 + i * 2] = hx[le[7 - i] >> 4];
        out[3 + i * 2] = hx[le[7 - i] & 15];
    }
    out[18] = 0;
}

static const char *sl_status_name(uint32_t s) {
    switch (s) {
        case 0x0000: return "OK";
        case 0x0004: return "BUSY";
        case 0x0040: return "MAC_NO_ACK";
        case 0x004C: return "CCA_FAILURE";
        case 0x0054: return "TX_SCHEDULER_FAIL";
        case 0x0058: return "TX_BLOCKED";
        case 0x0C02: return "DELIVERY_FAILED";
        case 0x0C03: return "MAX_MESSAGE_LIMIT";
        case 0x0C13: return "SOURCE_ROUTE_FAILURE";
        case 0x0C14: return "MANY_TO_ONE_ROUTE_FAILURE";
        case 0x0C20: return "NO_APS_ACK";
        case 0x0C21: return "APS_MESSAGE_CANCELED";
        case 0x0C24: return "ROUTE_DISCOVERY_UNDERWAY";
        case 0x0C25: return "UNICAST_FAILURE";
        case 0x0C26: return "NO_ROUTE";
        case 0x0C27: return "SLEEPY_CHILD_TIMEOUT";
        case 0x0C28: return "BROADCAST_RELAY_FAILED";
        default: return NULL;
    }
}

// Events are logged after the stats lock is released (stats_event takes it again).
typedef struct {
    ev_level_t level;
    char text[112];
    bool set;
} pending_event_t;

static void ev(pending_event_t *pe, ev_level_t lvl, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void ev(pending_event_t *pe, ev_level_t lvl, const char *fmt, ...) {
    if (pe->set) return;  // one per frame is plenty
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(pe->text, sizeof(pe->text), fmt, ap);
    va_end(ap);
    pe->level = lvl;
    pe->set = true;
}

// EZSP payload of an ASH DATA frame (already derandomized). Caller holds the stats lock.
static void on_ezsp(ash_dir_t dir, const uint8_t *d, size_t n, bool retx, int64_t now, pending_event_t *pe) {
    bridge_stats_t *g = &g_stats;
    if (n < 3) return;
    uint8_t seq = d[0];
    uint8_t fc_lo = d[1];
    bool extended = n >= 5 && (d[2] & 0x03) == 0x01;
    uint16_t frame_id = extended ? rd16(&d[3]) : d[2];
    const uint8_t *p = d + (extended ? 5 : 3);
    size_t pn = n - (extended ? 5 : 3);

    if (dir == DIR_HOST) {
        if (retx) return;
        g->ezsp_cmds++;
        g->acc_cmds++;
        s_cmd_pending[seq] = now;
        s_cmd_frame[seq] = frame_id;
        if (frame_id == EZSP_SEND_UNICAST) g->zb_unicast++;
        else if (frame_id == EZSP_SEND_BROADCAST) g->zb_broadcast++;
        else if (frame_id == EZSP_SEND_MULTICAST) g->zb_multicast++;
        return;
    }

    bool is_response = (fc_lo & 0x80) != 0;
    bool async_cb = (fc_lo & 0x10) != 0;
    if (!is_response) return;
    if (!async_cb) {
        g->ezsp_responses++;
        if (s_cmd_pending[seq] != 0 && s_cmd_frame[seq] == frame_id) {
            int64_t rtt = now - s_cmd_pending[seq];
            if (rtt >= 0 && rtt < 30000000) stats_add_rtt((uint32_t)rtt);
            s_cmd_pending[seq] = 0;
        }
        if (frame_id == EZSP_VERSION && pn >= 4) {
            bool changed = g->ezsp_version != p[0] || g->stack_version != rd16(&p[2]);
            g->ezsp_version = p[0];
            g->stack_type = p[1];
            g->stack_version = rd16(&p[2]);
            if (changed) {
                uint16_t v = g->stack_version;
                ev(pe, EV_INFO, "Coordinator: EZSP v%u, stack %u.%u.%u.%u", p[0], (v >> 12) & 15, (v >> 8) & 15,
                   (v >> 4) & 15, v & 15);
            }
        }
        return;
    }

    g->ezsp_callbacks++;
    uint32_t now_s = stats_uptime_s();
    switch (frame_id) {
        case EZSP_INCOMING_MESSAGE_HANDLER: {
            g->zb_rx++;
            g->acc_rx++;
            // type(1) apsFrame(11) senderShortId(2) senderLongId(8) bindingIndex(1) addressIndex(1)
            // lastHopLqi(1) lastHopRssi(1) lastHopTimestamp(4) payload...
            if (pn >= 26) {
                uint16_t sender = rd16(&p[12]);
                zb_device_t *dev = stats_device(sender, true);
                dev->rx++;
                const uint8_t *ieee = &p[14];
                bool has_ieee = false;
                for (int i = 0; i < 8; i++) has_ieee |= ieee[i] != 0 && ieee[i] != 0xFF;
                if (has_ieee) memcpy(dev->ieee, ieee, 8);
                dev->last_lqi = p[24];
                dev->last_rssi = (int8_t)p[25];
                if (dev->rx == 1) {
                    dev->rssi_avg = dev->last_rssi;
                    dev->lqi_avg = dev->last_lqi;
                } else {
                    dev->rssi_avg += (dev->last_rssi - dev->rssi_avg) * 0.1f;
                    dev->lqi_avg += (dev->last_lqi - dev->lqi_avg) * 0.1f;
                }
                dev->last_seen_s = now_s;
            }
            break;
        }
        case EZSP_MESSAGE_SENT_HANDLER: {
            // status(4) type(1) indexOrDestination(2) apsFrame(11) messageTag(2)
            if (pn < 7) break;
            uint32_t status = rd32(&p[0]);
            uint8_t type = p[4];
            uint16_t dest = rd16(&p[5]);
            bool ok = status == 0;
            if (ok) {
                g->zb_sent_ok++;
                g->acc_ok++;
            } else {
                g->zb_sent_fail++;
                g->acc_fail++;
            }
            if (type == 0x00) {  // DIRECT: indexOrDestination is the node id
                zb_device_t *dev = stats_device(dest, !ok);
                if (dev) {
                    if (ok) {
                        dev->tx_ok++;
                    } else {
                        dev->tx_fail++;
                        dev->last_fail_status = status;
                        dev->last_fail_s = now_s;
                    }
                }
            }
            break;
        }
        case EZSP_INCOMING_ROUTE_ERROR_HANDLER: {
            g->route_errors++;
            if (pn >= 6) {
                uint32_t status = rd32(&p[0]);
                uint16_t target = rd16(&p[4]);
                zb_device_t *dev = stats_device(target, true);
                dev->route_err++;
                const char *nm = sl_status_name(status);
                if (nm) ev(pe, EV_WARN, "Route error to 0x%04x: %s", target, nm);
                else ev(pe, EV_WARN, "Route error to 0x%04x: status 0x%04lx", target, (unsigned long)status);
            }
            break;
        }
        case EZSP_INCOMING_NETWORK_STATUS_HANDLER: {
            g->net_status_events++;
            if (pn >= 3) {
                uint16_t target = rd16(&p[1]);
                zb_device_t *dev = stats_device(target, false);
                if (dev) dev->route_err++;
            }
            break;
        }
        case EZSP_STACK_STATUS_HANDLER: {
            // version not seen yet (bridge started mid-session): the length tells the two apart
            bool sl = g->ezsp_version ? g->ezsp_version >= EZSP_SL_STATUS_SINCE : pn >= 4;
            if (pn < (sl ? 4u : 1u)) break;
            uint32_t status = sl ? rd32(&p[0]) : p[0];
            g->network_status = status;
            if (status == (sl ? SL_STATUS_NETWORK_UP : EMBER_NETWORK_UP)) {
                g->network_up_known = true;
                g->network_up = true;
                ev(pe, EV_INFO, "Zigbee network is up");
            } else if (status == (sl ? SL_STATUS_NETWORK_DOWN : EMBER_NETWORK_DOWN)) {
                g->network_up_known = true;
                g->network_up = false;
                ev(pe, EV_ERROR, "Zigbee network is DOWN");
            } else {
                ev(pe, EV_INFO, "Stack status 0x%04lx", (unsigned long)status);
            }
            break;
        }
        case EZSP_TRUST_CENTER_JOIN_HANDLER: {
            // newNodeId(2) newNodeEui64(8) status(1) policyDecision(1) parentOfNewNodeId(2)
            if (pn >= 14) {
                uint16_t node = rd16(&p[0]);
                char ieee[20];
                fmt_ieee(ieee, &p[2]);
                uint8_t st = p[10];
                uint16_t parent = rd16(&p[12]);
                if (st == 2) {
                    g->leaves++;
                    ev(pe, EV_WARN, "Device 0x%04x (%s) left the network", node, ieee);
                } else {
                    g->joins++;
                    ev(pe, EV_INFO, "Device 0x%04x (%s) %s via 0x%04x", node, ieee,
                       st == 1 ? "joined" : "rejoined", parent);
                }
            }
            break;
        }
        case EZSP_ID_CONFLICT_HANDLER: {
            g->id_conflicts++;
            if (pn >= 2) ev(pe, EV_WARN, "Address conflict for 0x%04x", rd16(&p[0]));
            break;
        }
        default:
            break;
    }
}

// The NCP acknowledged every host frame before ackNum: their ACK latency.
static void acked(bridge_stats_t *g, uint8_t ack, int64_t now) {
    for (int k = 1; k < 8; k++) {
        int n = (ack - k) & 7;
        if (s_ack_pending[n]) {
            int64_t lat = now - s_ack_pending[n];
            if (lat >= 0 && lat < 5000000) {
                g->ack_sum_us += lat;
                g->ack_n++;
                if (lat > g->ack_max_us) g->ack_max_us = (uint32_t)lat;
            }
            s_ack_pending[n] = 0;
        }
    }
}

static void on_frame(ash_dir_t dir, ash_rx_t *rx, int64_t now, pending_event_t *pe) {
    bridge_stats_t *g = &g_stats;
    ash_dir_stats_t *ds = dir == DIR_HOST ? &g->host : &g->ncp;
    if (rx->len == 0) return;
    if (rx->overflow) {
        ds->oversize++;
        g->acc_err++;
        return;
    }
    if (rx->bad || rx->len < 3) {
        ds->crc_err++;
        g->acc_err++;
        return;
    }
    size_t body = rx->len - 2;
    uint16_t want = (uint16_t)((rx->buf[body] << 8) | rx->buf[body + 1]);
    if (crc16_ccitt(rx->buf, body) != want) {
        ds->crc_err++;
        g->acc_err++;
        return;
    }
    ds->frames++;
    uint8_t c = rx->buf[0];
    if ((c & 0x80) == 0) {  // DATA
        ds->data++;
        bool retx = (c & 0x08) != 0;
        uint8_t frm = (c >> 4) & 7, ack = c & 7;
        if (retx) {
            ds->retx++;
            g->acc_err++;
        }
        if (dir == DIR_HOST) {
            if (!retx) s_ack_pending[frm] = now;
        } else {
            // DATA from the NCP also acknowledges host frames (ackNum = next expected)
            acked(g, ack, now);
        }
        // derandomized in place: the frame buffer is reset right after this frame anyway
        uint8_t *ez = rx->buf + 1;
        size_t en = body - 1;
        derandomize(ez, en);
        on_ezsp(dir, ez, en, retx, now, pe);
    } else if ((c & 0xE0) == 0x80) {  // ACK
        ds->ack++;
        if (dir == DIR_NCP) {
            uint8_t ack = c & 7;
            acked(g, ack, now);
        }
    } else if ((c & 0xE0) == 0xA0) {  // NAK
        ds->nak++;
        g->acc_err++;
    } else if (c == 0xC0) {  // RST (host)
        ds->rst++;
        memset(s_ack_pending, 0, sizeof(s_ack_pending));
        memset(s_cmd_pending, 0, sizeof(s_cmd_pending));
        ev(pe, EV_INFO, "Zigbee2MQTT reset the coordinator link (ASH RST)");
    } else if (c == 0xC1) {  // RSTACK (ncp): version, reset code
        ds->rstack++;
        if (body >= 3) {
            g->last_reset_code = rx->buf[2];
            ev(pe, EV_INFO, "Coordinator reset acknowledged (ASH v%u, reset code %u)", rx->buf[1], rx->buf[2]);
        }
    } else if (c == 0xC2) {  // ERROR (ncp): version, error code
        ds->error++;
        g->acc_err++;
        if (body >= 3) {
            g->last_error_code = rx->buf[2];
            ev(pe, EV_ERROR, "Coordinator sent ASH ERROR (code 0x%02x)", rx->buf[2]);
        }
    }
}

#define PENDING_EVENTS 8

// Events are logged without the stats lock (stats_event takes it). Each direction is fed by one
// task only, so the lock can be let go mid-feed when the batch fills up.
static void flush_events(pending_event_t *pe, int *npe) {
    stats_unlock();
    for (int i = 0; i < *npe; i++) {
        stats_event_r(0, pe[i].level, "%s", pe[i].text);  // the decoder follows radio 1
        pe[i].set = false;  // ev() only looks at set
    }
    *npe = 0;
    stats_lock();
}

void ezsp_decode_feed(ash_dir_t dir, const uint8_t *data, size_t len, int64_t now_us) {
    pending_event_t pe[PENDING_EVENTS];
    int npe = 0;
    for (int i = 0; i < PENDING_EVENTS; i++) pe[i].set = false;  // ev() only looks at set
    ash_rx_t *rx = &s_rx[dir];

    stats_lock();
    ash_dir_stats_t *ds = dir == DIR_HOST ? &g_stats.host : &g_stats.ncp;
    ds->bytes += len;
    if (dir == DIR_HOST) g_stats.acc_out += len;
    else g_stats.acc_in += len;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        switch (b) {
            case ASH_FLAG:
                on_frame(dir, rx, now_us, &pe[npe]);
                if (pe[npe].set && ++npe == PENDING_EVENTS) flush_events(pe, &npe);
                rx->len = 0;
                rx->escape = rx->bad = rx->overflow = false;
                break;
            case ASH_CANCEL:
                if (rx->len) ds->cancel++;
                rx->len = 0;
                rx->escape = rx->bad = rx->overflow = false;
                break;
            case ASH_SUBSTITUTE:
                ds->substitute++;
                rx->bad = true;
                break;
            case ASH_XON:
            case ASH_XOFF:
                break;
            case ASH_ESCAPE:
                rx->escape = true;
                break;
            case ASH_WAKE:
                if (rx->len == 0) break;  // only valid between frames
                // fallthrough
            default:
                if (rx->escape) {
                    b ^= ASH_FLIP;
                    rx->escape = false;
                }
                if (rx->len < ASH_MAX_FRAME) rx->buf[rx->len++] = b;
                else rx->overflow = true;
                break;
        }
    }
    if (npe) flush_events(pe, &npe);
    stats_unlock();
}

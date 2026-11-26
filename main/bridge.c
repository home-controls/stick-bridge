#include "bridge.h"
#include "tasks.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "dongle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "ezsp_decode.h"
#include "protodetect.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "settings.h"
#include "stats.h"
#include "net.h"
#include "watchdog.h"

static const char *TAG = "bridge";

#define HOLD_MAX 1024

// One radio's TCP side.
typedef struct {
    int idx;
    dongle_t *d;
    uint16_t port;  // the port lfd listens on (0 = none: the radio isn't shown)
    uint16_t failed_port;  // last port a listen failed on (one event per port, not per retry)
    bool listened;  // has listened before: later port changes are worth an event
    uint16_t last_port;  // the port it last listened on
    int lfd;
    volatile int client;
    // Held while the tx task sends on `client` and while the rx task swaps or closes it: the tx task never sends
    // on a socket number that was closed and handed to another radio's new client meanwhile.
    SemaphoreHandle_t send_lock;
    volatile bool kick;
    char kick_why[64];
    bool dongle_was_ready;
    uint32_t last_refused_ip, last_refused_s;
    const char *last_refused_why;

    // Which protocol this session speaks (protodetect.c). Radio 0's EZSP decoder only sees EZSP traffic: a
    // dongle the list says is EZSP is decoded from the first byte; for one it doesn't know, bytes are held
    // until the protocol is clear and replayed into the decoder if it's EZSP. Other radios only detect.
    SemaphoreHandle_t pd_lock;
    pd_state_t pd;
    volatile bool pd_done, ezsp_on;
    volatile bool sniff;  // version sniffing: until a version is learnt (Spinel, EZSP, Z-Stack)
    proto_t pd_expected;
    uint8_t (*hold)[HOLD_MAX];  // [2][HOLD_MAX], radio 0 only
    uint16_t hold_len[2];
    pd_ver_t ver;  // fed with dongle -> host bytes (tx task only)
} link_t;

static link_t s_l[RADIO_MAX];
static char s_names[RADIO_MAX][2][12];  // task names live as long as the tasks (the task list keeps the pointer)
static uint8_t s_hold0[2][HOLD_MAX];

static void sniff_versions(link_t *l, const uint8_t *b, size_t n) {
    if (!pd_ver_feed(&l->ver, b, n)) return;
    char run[80], ver[24];
    if (!pd_ver_text(&l->ver, run, sizeof(run), ver, sizeof(ver))) return;
    dx_note_fw(l->d, run, ver);
    l->sniff = false;  // known until the next session (a flash or a new client starts one)
    stats_event(EV_INFO, "Radio %d firmware: %s", l->idx + 1, run);
}

static void session_begin(link_t *l) {
    dongle_info_t di;
    dx_info(l->d, &di);
    xSemaphoreTake(l->pd_lock, portMAX_DELAY);
    pd_reset(&l->pd);
    l->pd_done = false;
    l->pd_expected = di.model->proto;
    l->ezsp_on = l->idx == 0 && l->pd_expected == PROTO_EZSP;
    l->hold_len[0] = l->hold_len[1] = 0;
    pd_ver_reset(&l->ver);
    l->sniff = true;
    xSemaphoreGive(l->pd_lock);
    dx_note_proto(l->d, PROTO_UNKNOWN);
}

static void observe(link_t *l, ash_dir_t dir, const uint8_t *b, size_t n, int64_t now) {
    if (l->pd_done) {  // decided: no lock needed
        if (l->ezsp_on) ezsp_decode_feed(dir, b, n, now);
        return;
    }
    int di = dir == DIR_HOST ? 0 : 1;
    char msg[128] = "";
    xSemaphoreTake(l->pd_lock, portMAX_DELAY);
    if (!l->pd_done) {
        proto_t p = pd_feed(&l->pd, di, b, n);
        if (p != PROTO_UNKNOWN) {
            l->pd_done = true;
            if (p != PROTO_SPINEL && p != PROTO_ZNP && p != PROTO_EZSP) l->sniff = false;  // no version to read
            dx_note_proto(l->d, p);
            if (l->idx == 0 && p == PROTO_EZSP && !l->ezsp_on) {
                for (int k = 0; k < 2; k++)
                    if (l->hold_len[k]) ezsp_decode_feed(k ? DIR_NCP : DIR_HOST, l->hold[k], l->hold_len[k], now);
                l->ezsp_on = true;
            } else if (p != PROTO_EZSP) {
                l->ezsp_on = false;
            }
            if (l->pd_expected != PROTO_UNKNOWN && p != l->pd_expected)
                snprintf(msg, sizeof(msg), "Radio %d: the stick talks %s, not %s as expected for it", l->idx + 1,
                         proto_name(p), proto_name(l->pd_expected));
            else if (l->pd_expected == PROTO_UNKNOWN)
                snprintf(msg, sizeof(msg), "Radio %d: the stick talks %s%s%s", l->idx + 1, proto_name(p),
                         proto_z2m_adapter(p)[0] ? " (Zigbee2MQTT adapter " : "", proto_z2m_adapter(p)[0] ? proto_z2m_adapter(p) : "");
            if (msg[0] && proto_z2m_adapter(p)[0] && l->pd_expected == PROTO_UNKNOWN) strlcat(msg, ")", sizeof(msg));
        } else if (l->idx == 0 && !l->ezsp_on) {
            size_t room = HOLD_MAX - l->hold_len[di], k = n < room ? n : room;
            memcpy(l->hold[di] + l->hold_len[di], b, k);
            l->hold_len[di] += (uint16_t)k;
        }
    }
    if (l->ezsp_on) ezsp_decode_feed(dir, b, n, now);
    xSemaphoreGive(l->pd_lock);
    if (msg[0]) stats_event(EV_INFO, "%s", msg);
}

static void ip_str(uint32_t ip, char *out, size_t n) { inet_ntoa_r(*(struct in_addr *)&ip, out, n); }

static void fmt_duration(uint32_t s, char *out, size_t n) {
    if (s < 120) snprintf(out, n, "%lus", (unsigned long)s);
    else if (s < 7200) snprintf(out, n, "%lum", (unsigned long)(s / 60));
    else if (s < 172800) snprintf(out, n, "%luh%02lum", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
    else snprintf(out, n, "%lud%luh", (unsigned long)(s / 86400), (unsigned long)(s % 86400 / 3600));
}

void bridge_kick_radio(int radio, const char *why) {
    if (radio < 0 || radio >= RADIO_MAX) return;
    link_t *l = &s_l[radio];
    strlcpy(l->kick_why, why, sizeof(l->kick_why));
    l->kick = true;
}

void bridge_kick_client(const char *why) { bridge_kick_radio(0, why); }

// Only the accept/receive task closes a client socket.
// Who a radio's client is, for events. Read each time: a rename or role change shows without a reboot.
static void who(const link_t *l, char *out, size_t n) {
    char name[16];
    uint8_t role = settings_radio_role(l->idx);
    if (l->idx == 0) {
        strlcpy(out, role == RADIO_ROLE_THREAD ? "Border router" : role == RADIO_ROLE_ZWAVE ? "Z-Wave JS" : "Zigbee2MQTT", n);
        return;
    }
    settings_radio_name(l->idx, name, sizeof(name));
    snprintf(out, n, "Radio %d (%s) client", l->idx + 1, name[0] ? name : radio_role_name(role));
}

static void drop_client(link_t *l, const char *why) {
    int fd = l->client;
    if (fd < 0) return;
    shutdown(fd, SHUT_RDWR);  // ends a send the tx task may be blocked in, so the lock below comes free
    xSemaphoreTake(l->send_lock, portMAX_DELAY);
    l->client = -1;
    close(fd);
    xSemaphoreGive(l->send_lock);
    char ip[16], dur[24];
    stats_lock();
    radio_stats_t *rs = &g_radio[l->idx];
    ip_str(rs->client_ip, ip, sizeof(ip));
    fmt_duration(stats_uptime_s() - rs->client_since_s, dur, sizeof(dur));
    rs->client_connected = false;
    rs->client_drops++;
    if (l->idx == 0) {
        g_stats.client_connected = false;
        g_stats.client_drops++;
    }
    stats_unlock();
    ESP_LOGW(TAG, "Radio %d: client %s dropped: %s", l->idx + 1, ip, why);
    char w[64];
    who(l, w, sizeof(w));
    stats_event_r(l->idx, EV_WARN, "%s (%s) disconnected after %s: %s", w, ip, dur, why);
}

static int open_listener(link_t *l) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(l->port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 2) != 0) {
        ESP_LOGE(TAG, "Radio %d: listen on %u failed: errno %d", l->idx + 1, l->port, errno);
        close(fd);
        return -1;
    }
    ESP_LOGI(TAG, "Radio %d: listening on TCP %u", l->idx + 1, l->port);
    return fd;
}

static void setup_client_socket(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    // Notice a client host that vanished (power cut, cable) within ~25 s.
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    int idle = 10, intvl = 5, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void handle_accept(link_t *l) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd = accept(l->lfd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) return;
    uint32_t pip = peer.sin_addr.s_addr;
    char ip[16];
    ip_str(pip, ip, sizeof(ip));

    uint32_t now_s = stats_uptime_s();
    const char *refuse = NULL;
    if (!settings_allow_peer_radio(l->idx, pip)) refuse = "not this radio's allowed host";
    else if (dx_claimed(l->d)) refuse = "the stick's firmware is being updated";
    else if (!dx_ready(l->d)) refuse = "no stick connected to this radio";

    if (refuse == NULL && l->client >= 0) {
        stats_lock();
        uint32_t cur = g_radio[l->idx].client_ip;
        stats_unlock();
        if (cur == pip) {
            drop_client(l, "replaced by a new connection from the same host");
        } else {
            refuse = "another client is connected";
        }
    }
    if (refuse) {
        close(fd);
        stats_lock();
        g_radio[l->idx].clients_refused++;
        if (l->idx == 0) g_stats.clients_refused++;
        stats_unlock();
        if (pip != l->last_refused_ip || refuse != l->last_refused_why || now_s - l->last_refused_s >= 30) {
            l->last_refused_ip = pip;
            l->last_refused_why = refuse;
            l->last_refused_s = now_s;
            stats_event(EV_WARN, "Radio %d: refused TCP connection from %s: %s", l->idx + 1, ip, refuse);
        }
        ESP_LOGW(TAG, "Radio %d: refused %s: %s", l->idx + 1, ip, refuse);
        return;
    }

    setup_client_socket(fd);
    dx_flush_rx(l->d);
    if (l->idx == 0) ezsp_decode_reset();
    session_begin(l);
    xSemaphoreTake(l->send_lock, portMAX_DELAY);
    l->client = fd;
    xSemaphoreGive(l->send_lock);
    stats_lock();
    radio_stats_t *rs = &g_radio[l->idx];
    rs->client_connected = true;
    rs->client_ip = pip;
    rs->client_port = ntohs(peer.sin_port);
    rs->client_since_s = now_s;
    rs->clients_accepted++;
    if (l->idx == 0) {
        g_stats.client_connected = true;
        g_stats.client_ip = pip;
        g_stats.client_port = ntohs(peer.sin_port);
        g_stats.client_since_s = now_s;
        g_stats.clients_accepted++;
    }
    stats_unlock();
    ESP_LOGI(TAG, "Radio %d: client connected from %s:%u", l->idx + 1, ip, ntohs(peer.sin_port));
    char w[64];
    who(l, w, sizeof(w));
    stats_event_r(l->idx, EV_INFO, "%s connected from %s", w, ip);
    // From now on the radio is set up: if its stick goes, it waits for it (and says so) instead of being freed.
    // Saved by the dongle task: this task's stack is in PSRAM, which can't write flash.
    if (!settings_radio_is_set_up(l->idx)) dx_request_set_up(l->d);
}

static void client_to_stick(link_t *l, uint8_t *buf, size_t cap) {
    int cfd = l->client;
    int n = recv(cfd, buf, cap, 0);
    if (n <= 0) {
        drop_client(l, n == 0 ? "connection closed by the client" : "connection error");
        return;
    }
    observe(l, DIR_HOST, buf, n, esp_timer_get_time());
    esp_err_t e = dx_write(l->d, buf, n, 1000);
    stats_lock();
    radio_stats_t *rs = &g_radio[l->idx];
    if (e == ESP_OK) {
        rs->bytes_out += n;
        rs->acc_out += n;
    } else {
        rs->tx_errors++;
        if (l->idx == 0) g_stats.dongle_tx_errors++;
    }
    stats_unlock();
    if (e != ESP_OK) ESP_LOGW(TAG, "Radio %d: write to stick failed: %s", l->idx + 1, esp_err_to_name(e));
}

// Listens on the radio's port (following its settings), accepts clients and moves client -> stick, one task per
// radio (a slow or stalled stick only holds up its own client, never Zigbee2MQTT's).
static void rx_task(void *arg) {
    link_t *l = arg;
    uint8_t buf[1024];
    watchdog_task_add();
    for (;;) {
        watchdog_feed();
#ifdef CONFIG_BRIDGE_TEST_HOOKS
        if (g_wd_test_stall && l->idx == 0) vTaskDelay(pdMS_TO_TICKS(30000));  // not fed: the task watchdog must fire
#endif
        // The port follows the radio: none while it isn't shown, and a new one as soon as its settings change.
        uint16_t want = (settings_radio_shown_mask() >> l->idx) & 1u ? settings_radio_port(l->idx) : 0;
        if (l->lfd >= 0 && want != l->port) {
            if (l->client >= 0) drop_client(l, want ? "the radio's TCP port changed" : "the radio was removed");
            close(l->lfd);
            l->lfd = -1;
            ESP_LOGI(TAG, "Radio %d: stopped listening on TCP %u", l->idx + 1, l->port);
            if (!want) net_set_ports();
        }
        if (want == 0) {
            l->port = 0;
            l->kick = false;
            l->dongle_was_ready = false;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }
        if (l->lfd < 0) {
            bool changed = l->listened && want != l->last_port;
            l->port = want;
            l->lfd = open_listener(l);
            if (l->lfd < 0) {
                if (l->failed_port != want) {
                    l->failed_port = want;
                    stats_event(EV_ERROR, "Radio %d: can't listen on TCP %u (errno %d); trying again", l->idx + 1, want, errno);
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            l->failed_port = 0;
            if (changed) stats_event(EV_INFO, "Radio %d: now listening on TCP %u", l->idx + 1, want);
            l->listened = true;
            l->last_port = want;
            net_set_ports();
        }
        if (l->kick) {
            l->kick = false;
            if (l->client >= 0) drop_client(l, l->kick_why);
        }
        bool ready = dx_ready(l->d);
        if (l->dongle_was_ready && !ready && l->client >= 0)
            drop_client(l, "the stick went away");
        if (dx_claimed(l->d) && l->client >= 0) drop_client(l, "the stick's firmware is being updated");
        l->dongle_was_ready = ready;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(l->lfd, &rfds);
        int maxfd = l->lfd;
        int cfd = l->client;
        if (cfd >= 0) {
            FD_SET(cfd, &rfds);
            if (cfd > maxfd) maxfd = cfd;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = 250000};
        int r = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EBADF) continue;  // client closed underneath; loop rebuilds the set
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (r == 0) continue;
        if (FD_ISSET(l->lfd, &rfds)) handle_accept(l);
        if (cfd >= 0 && cfd == l->client && FD_ISSET(cfd, &rfds)) client_to_stick(l, buf, sizeof(buf));
    }
}

// Moves stick -> client, one task per radio.
static void tx_task(void *arg) {
    link_t *l = arg;
    uint8_t buf[1024];
    StreamBufferHandle_t sb = dx_rx_stream(l->d);
    watchdog_task_add();
    for (;;) {
        watchdog_feed();
        size_t n = xStreamBufferReceive(sb, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n == 0) continue;
        if (l->client < 0) {
            stats_lock();
            g_radio[l->idx].dropped_no_client += n;
            if (l->idx == 0) g_stats.dropped_no_client += n;
            stats_unlock();
            continue;
        }
        observe(l, DIR_NCP, buf, n, esp_timer_get_time());
        if (l->sniff) sniff_versions(l, buf, n);
        stats_lock();
        g_radio[l->idx].bytes_in += n;
        g_radio[l->idx].acc_in += n;
        stats_unlock();
        xSemaphoreTake(l->send_lock, portMAX_DELAY);
        int fd = l->client;  // read under the lock: still this radio's client while we send
        size_t off = 0;
        while (fd >= 0 && off < n) {
            int w = send(fd, buf + off, n - off, 0);
            if (w <= 0) {
                ESP_LOGW(TAG, "Radio %d: send to client failed: errno %d", l->idx + 1, errno);
                // Wake the receive task; it owns the close.
                shutdown(fd, SHUT_RDWR);
                break;
            }
            off += w;
        }
        xSemaphoreGive(l->send_lock);
    }
}

void bridge_start(void) {
    for (int i = 0; i < RADIO_MAX; i++) {
        link_t *l = &s_l[i];
        memset(l, 0, sizeof(*l));
        l->idx = i;
        l->d = dongle_at(i);
        l->lfd = -1;
        l->client = -1;
        l->pd_lock = xSemaphoreCreateMutex();
        l->send_lock = xSemaphoreCreateMutex();
        if (i == 0) l->hold = s_hold0;
        // Every radio's tasks run from boot (a stick may turn up any time); an idle radio's just sleep.
        snprintf(s_names[i][0], sizeof(s_names[i][0]), "tcp_tx%d", i);
        snprintf(s_names[i][1], sizeof(s_names[i][1]), "tcp_rx%d", i);
        // Stacks in PSRAM: these tasks never write flash (the one NVS write, a radio getting set up, is the
        // dongle task's: dx_request_set_up).
        task_start(tx_task, i == 0 ? "tcp_tx" : s_names[i][0], 6144, l, 9, true);  // up to 4.4 KB measured
        task_start(rx_task, i == 0 ? "tcp_rx" : s_names[i][1], 6144, l, 8, true);
    }
}

#include "bridge.h"

#include <errno.h>
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
#include "watchdog.h"

static const char *TAG = "bridge";

static uint16_t s_port;
static volatile int s_client = -1;
static SemaphoreHandle_t s_lock;
static volatile bool s_kick;
static char s_kick_why[64];

// Which protocol this session speaks (protodetect.c). The EZSP decoder only sees EZSP traffic: a dongle the
// list says is EZSP is decoded from the first byte; for one it doesn't know, bytes are held until the
// protocol is clear and replayed into the decoder if it's EZSP.
static SemaphoreHandle_t s_pd_lock;
static pd_state_t s_pd;
static volatile bool s_pd_done, s_ezsp_on;
static proto_t s_pd_expected;
#define HOLD_MAX 1024
static uint8_t s_hold[2][HOLD_MAX];
static uint16_t s_hold_len[2];
static pd_ver_t s_ver;  // fed with dongle -> host bytes (tx task only)

// The first x.y.z in s ("SL-OPENTHREAD/2.6.1_GitHub-..." -> "2.6.1").
static void first_version(const char *s, char *out, size_t n) {
    out[0] = 0;
    for (const char *p = s; *p; p++) {
        if (*p >= '0' && *p <= '9' && (p == s || p[-1] == '/' || p[-1] == ' ' || p[-1] == 'v' || p[-1] == '-')) {
            size_t k = 0;
            int dots = 0;
            while ((p[k] >= '0' && p[k] <= '9') || (p[k] == '.' && p[k + 1] >= '0' && p[k + 1] <= '9')) {
                if (p[k] == '.') dots++;
                if (k + 1 < n) out[k] = p[k];
                k++;
            }
            if (dots >= 1) {
                out[k < n ? k : n - 1] = 0;
                return;
            }
            out[0] = 0;
        }
    }
}

static void sniff_versions(const uint8_t *b, size_t n) {
    if (!pd_ver_feed(&s_ver, b, n)) return;
    char run[80], ver[24];
    if (s_ver.spinel_version[0]) {
        first_version(s_ver.spinel_version, ver, sizeof(ver));
        dongle_note_fw(s_ver.spinel_version, ver);
        stats_event(EV_INFO, "Dongle firmware: %s", s_ver.spinel_version);
    } else if (s_ver.znp_seen) {
        snprintf(run, sizeof(run), "Z-Stack %u.%u.%u build %lu", s_ver.znp_major, s_ver.znp_minor, s_ver.znp_maint,
                 (unsigned long)s_ver.znp_revision);
        snprintf(ver, sizeof(ver), "%lu", (unsigned long)s_ver.znp_revision);
        dongle_note_fw(run, ver);
        stats_event(EV_INFO, "Dongle firmware: %s", run);
    }
}

static void session_begin(void) {
    dongle_info_t d;
    dongle_info(&d);
    xSemaphoreTake(s_pd_lock, portMAX_DELAY);
    pd_reset(&s_pd);
    s_pd_done = false;
    s_pd_expected = d.model->proto;
    s_ezsp_on = s_pd_expected == PROTO_EZSP;
    s_hold_len[0] = s_hold_len[1] = 0;
    pd_ver_reset(&s_ver);
    xSemaphoreGive(s_pd_lock);
    dongle_note_proto(PROTO_UNKNOWN);
}

static void observe(ash_dir_t dir, const uint8_t *b, size_t n, int64_t now) {
    if (s_pd_done) {  // decided: no lock needed
        if (s_ezsp_on) ezsp_decode_feed(dir, b, n, now);
        return;
    }
    int di = dir == DIR_HOST ? 0 : 1;
    char msg[112] = "";
    xSemaphoreTake(s_pd_lock, portMAX_DELAY);
    if (!s_pd_done) {
        proto_t p = pd_feed(&s_pd, di, b, n);
        if (p != PROTO_UNKNOWN) {
            s_pd_done = true;
            dongle_note_proto(p);
            if (p == PROTO_EZSP && !s_ezsp_on) {
                for (int k = 0; k < 2; k++)
                    if (s_hold_len[k]) ezsp_decode_feed(k ? DIR_NCP : DIR_HOST, s_hold[k], s_hold_len[k], now);
                s_ezsp_on = true;
            } else if (p != PROTO_EZSP) {
                s_ezsp_on = false;
            }
            if (s_pd_expected != PROTO_UNKNOWN && p != s_pd_expected)
                snprintf(msg, sizeof(msg), "The dongle talks %s, not %s as expected for it", proto_name(p), proto_name(s_pd_expected));
            else if (s_pd_expected == PROTO_UNKNOWN)
                snprintf(msg, sizeof(msg), "Detected %s on the dongle%s%s", proto_name(p), proto_z2m_adapter(p)[0] ? ": Zigbee2MQTT adapter " : "",
                         proto_z2m_adapter(p));
        } else if (!s_ezsp_on) {
            size_t room = HOLD_MAX - s_hold_len[di], k = n < room ? n : room;
            memcpy(s_hold[di] + s_hold_len[di], b, k);
            s_hold_len[di] += (uint16_t)k;
        }
    }
    if (s_ezsp_on) ezsp_decode_feed(dir, b, n, now);
    xSemaphoreGive(s_pd_lock);
    if (msg[0]) stats_event(EV_INFO, "%s", msg);
}

static void ip_str(uint32_t ip, char *out, size_t n) { inet_ntoa_r(*(struct in_addr *)&ip, out, n); }

static void fmt_duration(uint32_t s, char *out, size_t n) {
    if (s < 120) snprintf(out, n, "%lus", (unsigned long)s);
    else if (s < 7200) snprintf(out, n, "%lum", (unsigned long)(s / 60));
    else if (s < 172800) snprintf(out, n, "%luh%02lum", (unsigned long)(s / 3600), (unsigned long)(s % 3600 / 60));
    else snprintf(out, n, "%lud%luh", (unsigned long)(s / 86400), (unsigned long)(s % 86400 / 3600));
}

void bridge_kick_client(const char *why) {
    strlcpy(s_kick_why, why, sizeof(s_kick_why));
    s_kick = true;
}

// Only the accept/receive task closes the client socket.
static void drop_client(const char *why) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int fd = s_client;
    s_client = -1;
    xSemaphoreGive(s_lock);
    if (fd < 0) return;
    shutdown(fd, SHUT_RDWR);
    close(fd);
    char ip[16], dur[24];
    stats_lock();
    ip_str(g_stats.client_ip, ip, sizeof(ip));
    fmt_duration(stats_uptime_s() - g_stats.client_since_s, dur, sizeof(dur));
    g_stats.client_connected = false;
    g_stats.client_drops++;
    stats_unlock();
    ESP_LOGW(TAG, "Client %s dropped: %s", ip, why);
    stats_event(EV_WARN, "Zigbee2MQTT (%s) disconnected after %s: %s", ip, dur, why);
}

static int open_listener(void) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(s_port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(fd, 2) != 0) {
        ESP_LOGE(TAG, "Listen on %u failed: errno %d", s_port, errno);
        close(fd);
        return -1;
    }
    ESP_LOGI(TAG, "Listening for Zigbee2MQTT on TCP %u", s_port);
    return fd;
}

static void setup_client_socket(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    // Notice a Z2M host that vanished (power cut, cable) within ~25 s.
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
    int idle = 10, intvl = 5, cnt = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static void handle_accept(int lfd) {
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd = accept(lfd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) return;
    uint32_t pip = peer.sin_addr.s_addr;
    char ip[16];
    ip_str(pip, ip, sizeof(ip));

    static uint32_t last_refused_ip, last_refused_s;
    static const char *last_refused_why;
    uint32_t now_s = stats_uptime_s();
    const char *refuse = NULL;
    if (!settings_allow_peer(pip)) refuse = "not the configured Z2M host";
    else if (!dongle_ready()) refuse = "no Zigbee dongle connected";
    else if (dongle_claimed()) refuse = "the dongle's firmware is being updated";

    if (refuse == NULL && s_client >= 0) {
        stats_lock();
        uint32_t cur = g_stats.client_ip;
        stats_unlock();
        if (cur == pip) {
            drop_client("replaced by a new connection from the same host");
        } else {
            refuse = "another client is connected";
        }
    }
    if (refuse) {
        close(fd);
        stats_lock();
        g_stats.clients_refused++;
        stats_unlock();
        if (pip != last_refused_ip || refuse != last_refused_why || now_s - last_refused_s >= 30) {
            last_refused_ip = pip;
            last_refused_why = refuse;
            last_refused_s = now_s;
            stats_event(EV_WARN, "Refused TCP connection from %s: %s", ip, refuse);
        }
        ESP_LOGW(TAG, "Refused %s: %s", ip, refuse);
        return;
    }

    setup_client_socket(fd);
    dongle_flush_rx();
    ezsp_decode_reset();
    session_begin();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_client = fd;
    xSemaphoreGive(s_lock);
    stats_lock();
    g_stats.client_connected = true;
    g_stats.client_ip = pip;
    g_stats.client_port = ntohs(peer.sin_port);
    g_stats.client_since_s = now_s;
    g_stats.clients_accepted++;
    stats_unlock();
    ESP_LOGI(TAG, "Zigbee2MQTT connected from %s:%u", ip, ntohs(peer.sin_port));
    stats_event(EV_INFO, "Zigbee2MQTT connected from %s", ip);
}

// Accepts clients and moves Z2M -> dongle.
static void rx_task(void *arg) {
    (void)arg;
    int lfd = -1;
    uint8_t buf[1024];
    bool dongle_was_ready = false;
    watchdog_task_add();
    for (;;) {
        watchdog_feed();
#ifdef CONFIG_BRIDGE_TEST_HOOKS
        if (g_wd_test_stall) vTaskDelay(pdMS_TO_TICKS(30000));  // not fed: the task watchdog must fire
#endif
        if (lfd < 0) {
            lfd = open_listener();
            if (lfd < 0) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }
        if (s_kick) {
            s_kick = false;
            if (s_client >= 0) drop_client(s_kick_why);
        }
        bool ready = dongle_ready();
        if (dongle_was_ready && !ready && s_client >= 0) drop_client("the Zigbee dongle went away");
        if (dongle_claimed() && s_client >= 0) drop_client("the dongle's firmware is being updated");
        dongle_was_ready = ready;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(lfd, &rfds);
        int maxfd = lfd;
        int cfd = s_client;
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
        if (FD_ISSET(lfd, &rfds)) handle_accept(lfd);
        if (cfd >= 0 && cfd == s_client && FD_ISSET(cfd, &rfds)) {
            int n = recv(cfd, buf, sizeof(buf), 0);
            if (n <= 0) {
                drop_client(n == 0 ? "connection closed by Zigbee2MQTT" : "connection error");
                continue;
            }
            observe(DIR_HOST, buf, n, esp_timer_get_time());
            esp_err_t e = dongle_write(buf, n, 1000);
            if (e != ESP_OK) {
                stats_lock();
                g_stats.dongle_tx_errors++;
                stats_unlock();
                ESP_LOGW(TAG, "Write to dongle failed: %s", esp_err_to_name(e));
            }
        }
    }
}

// Moves dongle -> Z2M.
static void tx_task(void *arg) {
    (void)arg;
    uint8_t buf[1024];
    StreamBufferHandle_t sb = dongle_rx_stream();
    watchdog_task_add();
    for (;;) {
        watchdog_feed();
        size_t n = xStreamBufferReceive(sb, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (n == 0) continue;
        int fd = s_client;
        if (fd < 0) {
            stats_lock();
            g_stats.dropped_no_client += n;
            stats_unlock();
            continue;
        }
        observe(DIR_NCP, buf, n, esp_timer_get_time());
        sniff_versions(buf, n);
        size_t off = 0;
        while (off < n) {
            int w = send(fd, buf + off, n - off, 0);
            if (w <= 0) {
                ESP_LOGW(TAG, "Send to Zigbee2MQTT failed: errno %d", errno);
                // Wake the receive task; it owns the close.
                shutdown(fd, SHUT_RDWR);
                break;
            }
            off += w;
        }
    }
}

void bridge_start(uint16_t port) {
    s_port = port;
    s_lock = xSemaphoreCreateMutex();
    s_pd_lock = xSemaphoreCreateMutex();
    xTaskCreate(rx_task, "tcp_rx", 6144, NULL, 8, NULL);
    xTaskCreate(tx_task, "tcp_tx", 6144, NULL, 9, NULL);
}

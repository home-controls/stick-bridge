#include "watchdog.h"
#include "tasks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "ping/ping_sock.h"
#include "settings.h"
#include "stats.h"

static const char *TAG = "watchdog";

#define WD_MAGIC 0x57444f47u  // "WDOG"
#define HEAP_MIN_FREE (24 * 1024)
#define HEAP_MIN_BLOCK (8 * 1024)
#define HEAP_LOW_S 30
static int NET_CHECK_S = 30;
static int NET_FAIL_CHECKS = 20;  // 20 x 30 s = 10 minutes
static uint32_t s_test_target;     // test builds: ping this instead

static RTC_NOINIT_ATTR uint32_t s_rtc_magic;
static RTC_NOINIT_ATTR char s_rtc_reason[96];
static char s_last_reason[96];

const char *watchdog_last_reason(void) { return s_last_reason; }

void watchdog_restart(const char *why) {
    ESP_LOGE(TAG, "Restarting: %s", why);
    strlcpy(s_rtc_reason, why, sizeof(s_rtc_reason));
    s_rtc_magic = WD_MAGIC;
    vTaskDelay(pdMS_TO_TICKS(300));  // let the log line out
    esp_restart();
    for (;;) {
    }
}

void watchdog_task_add(void) { esp_task_wdt_add(NULL); }
void watchdog_feed(void) { esp_task_wdt_reset(); }

// ---------------------------------------------------------------- network reachability

typedef struct {
    SemaphoreHandle_t done;
    uint32_t replies;
} ping_ctx_t;

static void on_ping_ok(esp_ping_handle_t h, void *arg) {
    (void)h;
    ((ping_ctx_t *)arg)->replies++;
}

static void on_ping_end(esp_ping_handle_t h, void *arg) {
    (void)h;
    xSemaphoreGive(((ping_ctx_t *)arg)->done);
}

// True if addr (lwIP order) answers at least one of 3 pings.
static bool answers(uint32_t addr) {
    if (addr == 0 || addr == IPADDR_NONE) return false;
    ping_ctx_t ctx = {.done = xSemaphoreCreateBinary(), .replies = 0};
    if (!ctx.done) return false;
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = addr;
    cfg.count = 3;
    cfg.interval_ms = 300;
    cfg.timeout_ms = 1000;
    cfg.task_stack_size = 3072;
    esp_ping_callbacks_t cbs = {.cb_args = &ctx, .on_ping_success = on_ping_ok, .on_ping_end = on_ping_end};
    esp_ping_handle_t h;
    bool ok = false;
    if (esp_ping_new_session(&cfg, &cbs, &h) == ESP_OK) {
        esp_ping_start(h);
        xSemaphoreTake(ctx.done, pdMS_TO_TICKS(6000));
        esp_ping_stop(h);
        esp_ping_delete_session(h);
        ok = ctx.replies > 0;
    }
    vSemaphoreDelete(ctx.done);
    return ok;
}

static void watchdog_task(void *arg) {
    (void)arg;
    int heap_low_s = 0, net_fail = 0, tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        tick++;

        // Internal RAM is what runs out (tasks, USB/Ethernet DMA, small buffers); PSRAM has megabytes.
        size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        size_t block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (free_b < HEAP_MIN_FREE || block < HEAP_MIN_BLOCK) {
            if (++heap_low_s == 5) ESP_LOGW(TAG, "Free memory low: %u free, largest block %u", (unsigned)free_b, (unsigned)block);
            if (heap_low_s >= HEAP_LOW_S) {
                char why[96];
                snprintf(why, sizeof(why), "free memory critically low for %d s (%u free)", HEAP_LOW_S, (unsigned)free_b);
                watchdog_restart(why);
            }
        } else {
            heap_low_s = 0;
        }

        if (tick % NET_CHECK_S) continue;
        stats_lock();
        bool client = false, ip = g_stats.eth_has_ip, link = g_stats.eth_link;
        for (int r = 0; r < RADIO_MAX; r++) client = client || g_radio[r].client_connected;  // any radio's client
        uint32_t gw = g_stats.gw;
        stats_unlock();
        if (!s_test_target && (client || !link || !ip)) {
            // A client connected (Zigbee2MQTT or another radio's): the network works. No link: a cable, not something a restart fixes.
            net_fail = 0;
            continue;
        }
        bridge_settings_t cfg;
        settings_get(&cfg);
        uint32_t z2m = 0, mask = 0;
        settings_parse_allow(cfg.allow, &z2m, &mask);
        bool single_host = mask == 0xFFFFFFFFu;
        if (s_test_target) {
            single_host = false;
            gw = s_test_target;
            client = false;
        }
        if ((single_host && answers(z2m)) || answers(gw)) {
            if (net_fail >= 4) stats_event(EV_INFO, "Network reachable again");
            net_fail = 0;
        } else if (++net_fail == 4) {
            stats_event(EV_WARN, "Neither the Zigbee2MQTT host nor the gateway answers (2 min); restarting at 10 min");
        } else if (net_fail >= NET_FAIL_CHECKS) {
            watchdog_restart(s_test_target ? "network unreachable (test)" : "no Zigbee2MQTT and the network unreachable for 10 min");
        }
    }
}

void watchdog_start(void) {
    if (s_rtc_magic == WD_MAGIC) {
        s_rtc_reason[sizeof(s_rtc_reason) - 1] = 0;
        strlcpy(s_last_reason, s_rtc_reason, sizeof(s_last_reason));
        stats_event(EV_WARN, "Restarted by the watchdog: %s", s_last_reason);
        ESP_LOGW(TAG, "Last restart was the watchdog's: %s", s_last_reason);
    } else if (esp_reset_reason() == ESP_RST_TASK_WDT || esp_reset_reason() == ESP_RST_INT_WDT) {
        strlcpy(s_last_reason, "a task stopped responding (task watchdog)", sizeof(s_last_reason));
        stats_event(EV_WARN, "Restarted by the watchdog: %s", s_last_reason);
    }
    s_rtc_magic = 0;
    task_start(watchdog_task, "watchdog", 3072, NULL, 2, false);
}

#ifdef CONFIG_BRIDGE_TEST_HOOKS
volatile bool g_wd_test_stall;

bool watchdog_test(const char *kind) {
    if (strcmp(kind, "task") == 0) {
        g_wd_test_stall = true;
    } else if (strcmp(kind, "heap") == 0) {
        while (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > 16 * 1024) {
            if (!heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)) break;  // kept on purpose: the watchdog should restart within ~30 s
        }
    } else if (strcmp(kind, "net") == 0) {
        s_test_target = ipaddr_addr("192.0.2.1");  // TEST-NET-1: never answers
        NET_CHECK_S = 5;
        NET_FAIL_CHECKS = 3;
    } else {
        return false;
    }
    ESP_LOGW(TAG, "Test: %s watchdog armed", kind);
    return true;
}
#endif
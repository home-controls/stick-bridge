// Zigbee Ethernet bridge (Waveshare ESP32-P4-POE-ETH)
//
// The Zigbee dongle (Sonoff Dongle Plus MG24, CP2102N) plugs into J3, the P4's USB 2.0 host.
// Zigbee2MQTT connects to tcp://<bridge>:6638 and talks EZSP to the dongle as if it were local;
// every byte is also decoded passively for the dashboard on port 80.

#include "bridge.h"
#include "dongle.h"
#include "fwcat.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "logbuf.h"
#include "logstore.h"
#include "ncpflash.h"
#include "net.h"
#include "sdkconfig.h"
#include "settings.h"
#include "stats.h"
#include "sysinfo.h"
#include "updater.h"
#include "watchdog.h"
#include "web.h"

static const char *TAG = "main";

// Factory reset without a button: power the board on 5 times in a row, each time for less than
// 10 s (with PoE: unplug and replug the cable). The BOOT button can't be used for this: its
// GPIO35 is also the Ethernet TXD1 line on this board.
#define POWER_CYCLES_FOR_RESET 5
static void clear_power_cycles(void *arg) {
    (void)arg;
    nvs_handle_t h;
    if (nvs_open("boot", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "pc", 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void power_cycle_check(void) {
    nvs_handle_t h;
    if (nvs_open("boot", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t n = 0;
    nvs_get_u8(h, "pc", &n);
    n = esp_reset_reason() == ESP_RST_POWERON ? n + 1 : 0;
    nvs_set_u8(h, "pc", n >= POWER_CYCLES_FOR_RESET ? 0 : n);
    nvs_commit(h);
    nvs_close(h);
    if (n >= POWER_CYCLES_FOR_RESET) {
        ESP_LOGW(TAG, "%d quick power cycles: factory reset", POWER_CYCLES_FOR_RESET);
        settings_factory_reset();
        esp_restart();
    }
    if (n > 1) ESP_LOGW(TAG, "Quick power cycle %u of %d (factory reset at %d)", n, POWER_CYCLES_FOR_RESET, POWER_CYCLES_FOR_RESET);
    static esp_timer_handle_t t;
    const esp_timer_create_args_t a = {.callback = clear_power_cycles, .name = "pc_clear"};
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 10 * 1000000);
}

// A new image (upload or git) boots on trial. It's kept once it has run 30 s with the network up
// and the dashboard serving. A crash or watchdog before that makes the bootloader go back; so does
// this task if the trial isn't healthy within 2 minutes (no network = no way to fix it remotely).
#define TRIAL_MIN_S 30
#define TRIAL_MAX_S 120
static void confirm_task(void *arg) {
    (void)arg;
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "New firmware on trial: kept after %d s with the network and dashboard up", TRIAL_MIN_S);
        for (int s = 1;; s++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            // A supported dongle that was identified must also be open: a USB-layer regression on the
            // coordinator rolls back instead of stranding Zigbee2MQTT.
            bool net = net_has_ip(), web = web_running(), dongle = dongle_ready() || !dongle_identified();
            if (s >= TRIAL_MIN_S && net && web && dongle) {
                esp_ota_mark_app_valid_cancel_rollback();
                ESP_LOGI(TAG, "Firmware confirmed");
                stats_event(EV_INFO, "Firmware %s confirmed after its trial boot", esp_app_get_description()->version);
                break;
            }
            if (s >= TRIAL_MAX_S) {
                ESP_LOGE(TAG, "Trial failed after %d s (network %s, dashboard %s, dongle %s): going back to the previous firmware",
                         TRIAL_MAX_S, net ? "up" : "DOWN", web ? "up" : "DOWN", dongle ? "open" : "NOT OPEN");
                vTaskDelay(pdMS_TO_TICKS(200));
                esp_ota_mark_app_invalid_rollback_and_reboot();
            }
        }
    }
    vTaskDelete(NULL);
}

void app_main(void) {
    logbuf_init();
    ESP_LOGI(TAG, "Zigbee Ethernet bridge %s", esp_app_get_description()->version);
#if CONFIG_BRIDGE_TEST_BREAK == 1
    ESP_LOGE(TAG, "TEST BUILD: crashing at boot on purpose");
    abort();
#endif
    stats_begin();
    sysinfo_init();
    settings_init();
    power_cycle_check();
    logstore_start();

    bridge_settings_t cfg;
    settings_get(&cfg);
    stats_event(EV_INFO, "Booted firmware %s", esp_app_get_description()->version);
    watchdog_start();

    net_start(&cfg);
    // The trial-boot check starts before anything else that could hang: a new image that never gets its
    // dashboard up is rolled back within 2 minutes.
    xTaskCreate(confirm_task, "confirm", 3072, NULL, 3, NULL);
    dongle_start(cfg.baud);
    dongle_set_flow(cfg.tx_chunk, cfg.tx_gap_us, cfg.xoff_honor, cfg.xoff_max_ms);
    if (cfg.hw_flow) {
        char err[96];
        dongle_set_hw_flow(true, err, sizeof(err));  // remembered; applied when the dongle opens
    }
    bridge_start(cfg.tcp_port);
    ncpflash_start();
    fwcat_start();
    web_start();
    updater_start();
}

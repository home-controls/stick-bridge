#include "dongle.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "stats.h"
#include "txpace_core.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"
#include "usbser.h"
#include "watchdog.h"

static const char *TAG = "dongle";

#define LINE_POLL_MS 10        // CTS sample period
#define LINE_STATUS_EVERY 10   // UART status every 10th sample (100 ms)
#define RX_STREAM_SIZE (16 * 1024)

static StreamBufferHandle_t s_rx;
static StreamBufferHandle_t s_claim_rx;  // the flasher's, while it has the dongle
static volatile bool s_claimed;
static SemaphoreHandle_t s_tx_lock;
static TaskHandle_t s_task;
static us_port_t s_port;                // s_port.h is the open device (NULL when none)
static volatile bool s_ready;
static uint32_t s_baud;

// What's plugged in (set from the USB enumeration callback, read by everyone else).
static portMUX_TYPE s_id_mux = portMUX_INITIALIZER_UNLOCKED;
static dongle_info_t s_info;
static volatile bool s_have_target;

// USB OUT pacing and XON/XOFF (txpace_core.c). s_tp's XON/XOFF state and stats are guarded by
// s_tp_mux (the USB driver task scans incoming bytes); its line state and config by s_tx_lock.
static tp_state_t s_tp;
static portMUX_TYPE s_tp_mux = portMUX_INITIALIZER_UNLOCKED;
static EventGroupHandle_t s_flow_ev;
#define FLOW_XON BIT0  // set while the NCP has not said XOFF

// The USB chip's own view of the UART (line monitor) and its CTS handshake. s_line is guarded by s_tp_mux;
// s_mon_lock keeps the handle alive while the monitor uses it (close_dongle takes it as well).
static dongle_line_t s_line;
static SemaphoreHandle_t s_mon_lock;
static volatile bool s_hw_flow;   // CTS handshake on right now
static volatile bool s_hw_pref;   // what the settings ask for

// Notification bits for the dongle task
#define NOTIFY_NEW_DEV BIT0
#define NOTIFY_GONE BIT1

StreamBufferHandle_t dongle_rx_stream(void) { return s_rx; }
bool dongle_ready(void) { return s_ready; }
bool dongle_identified(void) { return s_have_target; }

void dongle_info(dongle_info_t *out) {
    portENTER_CRITICAL(&s_id_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_id_mux);
}

void dongle_rematch(void) {
    portENTER_CRITICAL(&s_id_mux);
    bool have = s_info.vid != 0;
    dongle_info_t d = s_info;
    portEXIT_CRITICAL(&s_id_mux);
    if (!have) return;
    const dongle_model_t *m = dongle_match(d.vid, d.pid, d.mfr, d.product, d.has_cdc);
    if (m == d.model) return;
    portENTER_CRITICAL(&s_id_mux);
    s_info.model = m;
    portEXIT_CRITICAL(&s_id_mux);
    stats_event(EV_INFO, "Dongle now identified as %s (updated dongle list)", m->name);
}

void dongle_note_proto(proto_t p) {
    portENTER_CRITICAL(&s_id_mux);
    s_info.proto_detected = p;
    portEXIT_CRITICAL(&s_id_mux);
}

void dongle_note_fw(const char *running, const char *version) {
    portENTER_CRITICAL(&s_id_mux);
    strlcpy(s_info.fw_running, running, sizeof(s_info.fw_running));
    strlcpy(s_info.fw_version, version, sizeof(s_info.fw_version));
    portEXIT_CRITICAL(&s_id_mux);
}

static void flow_line_reset(void) {
    portENTER_CRITICAL(&s_tp_mux);
    tp_reset_line(&s_tp);
    portEXIT_CRITICAL(&s_tp_mux);
    xEventGroupSetBits(s_flow_ev, FLOW_XON);
}

void dongle_flush_rx(void) {
    xStreamBufferReset(s_rx);
    flow_line_reset();  // a new client starts with no stale XOFF
}

static bool on_rx(const uint8_t *data, size_t len, void *arg) {
    (void)arg;
    // FTDI puts 2 status bytes in front of every packet: strip them from a copy (the driver's buffer is
    // read-only). The open forces one 64-byte packet per callback for FTDI.
    static uint8_t ftdi_buf[64];
    if (s_port.chip == US_FTDI) {
        size_t n = len < sizeof(ftdi_buf) ? len : sizeof(ftdi_buf);
        memcpy(ftdi_buf, data, n);
        len = us_rx_fix(&s_port, ftdi_buf, n);
        data = ftdi_buf;
        if (len == 0) return true;
    }
    if (s_claimed) {
        xStreamBufferSend(s_claim_rx, data, len, 0);
        return true;
    }
    // XON/XOFF stay in the stream (Zigbee2MQTT's ASH layer reads them too); they also gate our writes.
    if (memchr(data, TP_XOFF, len) || memchr(data, TP_XON, len)) {
        portENTER_CRITICAL(&s_tp_mux);
        bool changed = tp_rx(&s_tp, data, len, esp_timer_get_time());
        bool xoff = s_tp.xoff;
        portEXIT_CRITICAL(&s_tp_mux);
        if (changed) {
            if (xoff) xEventGroupClearBits(s_flow_ev, FLOW_XON);
            else xEventGroupSetBits(s_flow_ev, FLOW_XON);
        }
    }
    size_t sent = xStreamBufferSend(s_rx, data, len, 0);
    if (sent < len) {
        stats_lock();
        g_stats.dropped_overflow += len - sent;
        stats_unlock();
    }
    return true;
}

static void on_event(const cdc_acm_host_dev_event_data_t *event, void *ctx) {
    (void)ctx;
    switch (event->type) {
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            s_ready = false;
            xTaskNotify(s_task, NOTIFY_GONE, eSetBits);
            break;
        case CDC_ACM_HOST_ERROR:
            ESP_LOGW(TAG, "USB error %d", event->data.error);
            break;
        default:
            break;
    }
}

static void usb_str(const usb_str_desc_t *d, char *out, size_t out_len) {
    out[0] = 0;
    if (d == NULL) return;
    size_t n = (d->bLength - 2) / 2, j = 0;
    for (size_t i = 0; i < n && j + 1 < out_len; i++) {
        uint16_t c = d->wData[i];
        out[j++] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[j] = 0;
}

// Does the active configuration have a CDC communications or data interface?
static bool has_cdc_interface(usb_device_handle_t dev) {
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) return false;
    const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
    while (p + 2 <= end && p[0] >= 2) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE && p + 6 <= end && (p[5] == 0x02 || p[5] == 0x0A)) return true;
        p += p[0];
    }
    return false;
}

// Works out what this USB device is. Runs in the CDC driver task (new device) or the dongle task (scan).
static void identify(usb_device_handle_t usb_dev) {
    const usb_device_desc_t *desc;
    if (usb_host_get_device_descriptor(usb_dev, &desc) != ESP_OK) return;
    if (desc->bDeviceClass == USB_CLASS_HUB) return;
    usb_device_info_t info;
    if (usb_host_device_info(usb_dev, &info) != ESP_OK) return;
    dongle_info_t d = {0};
    usb_str(info.str_desc_manufacturer, d.mfr, sizeof(d.mfr));
    usb_str(info.str_desc_product, d.product, sizeof(d.product));
    usb_str(info.str_desc_serial_num, d.serial, sizeof(d.serial));
    d.vid = desc->idVendor;
    d.pid = desc->idProduct;
    bool cdc = has_cdc_interface(usb_dev);
    d.has_cdc = cdc;
    d.chip = us_chip_for(d.vid, d.pid, cdc);
    d.model = dongle_match(d.vid, d.pid, d.mfr, d.product, cdc);
    d.speed_full = info.speed != USB_SPEED_HIGH;
    ESP_LOGI(TAG, "USB device %04x:%04x '%s' '%s' serial %s (%s speed): %s via %s", d.vid, d.pid, d.mfr, d.product,
             d.serial, info.speed == USB_SPEED_HIGH ? "high" : info.speed == USB_SPEED_FULL ? "full" : "low",
             d.model->name, us_chip_name(d.chip));
    if (d.chip == US_NONE) {
        stats_event(EV_WARN, "Unsupported USB device %04x:%04x '%s' plugged in (not a serial adapter)", d.vid, d.pid,
                    d.product);
        return;
    }
    if (s_port.h != NULL || s_have_target) {
        if (s_info.vid != d.vid || s_info.pid != d.pid || strcmp(s_info.serial, d.serial) != 0)
            stats_event(EV_WARN, "Second USB adapter %04x:%04x '%s' ignored: the bridge drives one dongle", d.vid, d.pid,
                        d.product);
        return;
    }
    portENTER_CRITICAL(&s_id_mux);
    s_info = d;
    portEXIT_CRITICAL(&s_id_mux);
    s_have_target = true;
    stats_lock();
    g_stats.dongle_present = true;
    g_stats.dongle_vid = d.vid;
    g_stats.dongle_pid = d.pid;
    strlcpy(g_stats.dongle_mfr, d.mfr, sizeof(g_stats.dongle_mfr));
    strlcpy(g_stats.dongle_product, d.product, sizeof(g_stats.dongle_product));
    strlcpy(g_stats.dongle_serial, d.serial, sizeof(g_stats.dongle_serial));
    stats_unlock();
    if (s_task) xTaskNotify(s_task, NOTIFY_NEW_DEV, eSetBits);
}

static void on_new_dev(usb_device_handle_t usb_dev) { identify(usb_dev); }

// Fallback: look at devices that were attached before we were listening (own lightweight USB client).
static usb_host_client_handle_t s_scan_client;
static void scan_cb(const usb_host_client_event_msg_t *msg, void *arg) {
    (void)msg;
    (void)arg;
}
static void scan_existing(void) {
    if (s_scan_client == NULL) {
        const usb_host_client_config_t cc = {
            .is_synchronous = false,
            .max_num_event_msg = 5,
            .async = {.client_event_callback = scan_cb, .callback_arg = NULL},
        };
        if (usb_host_client_register(&cc, &s_scan_client) != ESP_OK) return;
    }
    usb_host_client_handle_events(s_scan_client, 0);  // drain (we only scan)
    uint8_t addrs[8];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) != ESP_OK) return;
    for (int i = 0; i < n && !s_have_target; i++) {
        usb_device_handle_t dev;
        if (usb_host_device_open(s_scan_client, addrs[i], &dev) != ESP_OK) continue;
        identify(dev);
        usb_host_device_close(s_scan_client, dev);
    }
}

static void try_open(void) {
    if (s_port.h != NULL || !s_have_target) return;
    dongle_info_t d;
    dongle_info(&d);
    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = 500,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .event_cb = on_event,
        .data_cb = on_rx,
        .user_arg = NULL,
    };
    us_port_t p;
    esp_err_t e = us_open(&p, d.chip, d.vid, d.pid, &cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "Open %04x:%04x as %s failed: %s", d.vid, d.pid, us_chip_name(d.chip), esp_err_to_name(e));
        return;
    }
    // on_rx reads s_port.chip (FTDI stripping) from the first byte on
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    s_port = p;
    xSemaphoreGive(s_tx_lock);
    e = us_setup(&s_port, s_baud);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "%s setup failed: %s", us_chip_name(d.chip), esp_err_to_name(e));
        stats_event(EV_ERROR, "Dongle found but its USB-UART (%s) setup failed (%s)", us_chip_name(d.chip),
                    esp_err_to_name(e));
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        xSemaphoreTake(s_mon_lock, portMAX_DELAY);
        us_close(&s_port);
        xSemaphoreGive(s_mon_lock);
        xSemaphoreGive(s_tx_lock);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    xStreamBufferReset(s_rx);
    flow_line_reset();
    s_ready = true;
    stats_lock();
    g_stats.dongle_present = true;
    g_stats.dongle_open = true;
    g_stats.dongle_attach++;
    g_stats.dongle_since_s = stats_uptime_s();
    stats_unlock();
    ESP_LOGI(TAG, "Dongle open at %lu baud", (unsigned long)s_baud);
    stats_event(EV_INFO, "Dongle connected: %s (%s), serial %s, %lu baud", d.product[0] ? d.product : d.model->name,
                us_chip_name(d.chip), d.serial[0] ? d.serial : "?", (unsigned long)s_baud);
    if (s_hw_pref) {
        char err[96];
        if (dongle_set_hw_flow(true, err, sizeof(err)) != ESP_OK)
            stats_event(EV_WARN, "Hardware flow control left off: %s", err);
    }
}

static void close_dongle(void) {
    s_ready = false;
    s_have_target = false;
    if (s_port.h == NULL) return;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    xSemaphoreTake(s_mon_lock, portMAX_DELAY);
    us_close(&s_port);
    s_hw_flow = false;
    xSemaphoreGive(s_mon_lock);
    xSemaphoreGive(s_tx_lock);
    portENTER_CRITICAL(&s_tp_mux);
    s_line.known = false;
    s_line.hw_flow = false;
    portEXIT_CRITICAL(&s_tp_mux);
    portENTER_CRITICAL(&s_id_mux);
    s_info.proto_detected = PROTO_UNKNOWN;
    s_info.fw_running[0] = s_info.fw_version[0] = 0;
    portEXIT_CRITICAL(&s_id_mux);
    stats_lock();
    g_stats.dongle_present = false;
    g_stats.dongle_open = false;
    g_stats.dongle_detach++;
    stats_unlock();
    ESP_LOGW(TAG, "Dongle disconnected");
    stats_event(EV_ERROR, "Dongle disconnected");
}

static void dongle_task(void *arg) {
    (void)arg;
    watchdog_task_add();
    uint32_t idle = 0;
    for (;;) {
        watchdog_feed();
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(2000));
        if (bits & NOTIFY_GONE) close_dongle();
        if (s_port.h == NULL) {
            // Every 2 s: open the identified device; every ~10 s with nothing identified, scan for one
            // that was attached before we listened.
            if (!s_have_target && ++idle % 5 == 0) scan_existing();
            try_open();
        } else {
            idle = 0;
        }
    }
}

// Samples the USB chip's CTS input (the radio's RTS, if the board wires it) and its UART status: errors on
// what it received from the radio, why its transmitter is held, and how much it has queued each way.
static void line_task(void *arg) {
    (void)arg;
    uint32_t n = 0;
    bool last_cts = true;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(LINE_POLL_MS));
        if (!s_ready || s_claimed) continue;
        if (s_port.chip != US_CP210X && s_port.chip != US_CH34X && s_port.chip != US_FTDI) continue;  // can't tell
        if (xSemaphoreTake(s_mon_lock, pdMS_TO_TICKS(5)) != pdTRUE) continue;
        bool cts = false;
        us_comm_t cs;
        esp_err_t e = s_port.h ? us_get_cts(&s_port, &cts) : ESP_ERR_INVALID_STATE;
        esp_err_t e2 = ESP_ERR_INVALID_STATE;
        if (s_port.h && e == ESP_OK && ++n % LINE_STATUS_EVERY == 0) e2 = us_get_comm(&s_port, &cs);
        xSemaphoreGive(s_mon_lock);

        portENTER_CRITICAL(&s_tp_mux);
        if (e != ESP_OK) {
            s_line.poll_fail++;
        } else {
            s_line.known = true;
            s_line.cts_now = cts;
            s_line.samples++;
            if (!cts) s_line.cts_off++;
            if (last_cts && !cts) s_line.cts_drops++;
            last_cts = cts;
        }
        if (e2 == ESP_OK) {
            s_line.status_polls++;
            if (cs.brk) s_line.err_break++;
            if (cs.framing) s_line.err_framing++;
            if (cs.overrun) s_line.err_hw_overrun++;
            if (cs.queue_overrun) s_line.err_queue_overrun++;
            if (cs.parity) s_line.err_parity++;
            if (cs.hold_cts) s_line.hold_cts++;
            if (cs.hold_xon) s_line.hold_xon++;
            if (cs.in_queue > s_line.in_queue_max) s_line.in_queue_max = cs.in_queue;
            if (cs.out_queue > s_line.out_queue_max) s_line.out_queue_max = cs.out_queue;
        }
        portEXIT_CRITICAL(&s_tp_mux);
    }
}

static void usb_lib_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static esp_err_t write_raw(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    esp_err_t e = s_port.h ? cdc_acm_host_data_tx_blocking(s_port.h, data, len, timeout_ms) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_tx_lock);
    return e;
}

// ---------------------------------------------------------------- paced writes (txpace_core.c)

static int io_write(void *ctx, const uint8_t *buf, size_t len, uint32_t timeout_ms) {
    (void)ctx;
    if (s_port.h == NULL) return -1;
    esp_err_t e = cdc_acm_host_data_tx_blocking(s_port.h, buf, len, timeout_ms);
    return e == ESP_OK ? 0 : e == ESP_ERR_TIMEOUT ? 1 : -1;
}

static int64_t io_now(void *ctx) {
    (void)ctx;
    return esp_timer_get_time();
}

// Waits here are a gap or the tail of one chunk on the line (~1.7 ms at 460800, ~6 ms at 115200):
// sleep whole ticks, spin the last stretch, since a 1 kHz tick can't time it.
static void io_wait_until(void *ctx, int64_t t) {
    (void)ctx;
    int64_t d = t - esp_timer_get_time();
    if (d > 2500) vTaskDelay(pdMS_TO_TICKS((d - 1500) / 1000));
    d = t - esp_timer_get_time();
    if (d > 0) esp_rom_delay_us((uint32_t)d);
}

static void io_wait_xon(void *ctx, int64_t deadline) {
    (void)ctx;
    int64_t d = deadline - esp_timer_get_time();
    if (d <= 0) return;
    TickType_t ticks = pdMS_TO_TICKS((d + 999) / 1000);
    xEventGroupWaitBits(s_flow_ev, FLOW_XON, pdFALSE, pdTRUE, ticks ? ticks : 1);
}

static void io_lock(void *ctx) {
    (void)ctx;
    portENTER_CRITICAL(&s_tp_mux);
}

static void io_unlock(void *ctx) {
    (void)ctx;
    portEXIT_CRITICAL(&s_tp_mux);
}

static const tp_io_t s_tp_io = {
    .write = io_write,
    .now_us = io_now,
    .wait_until = io_wait_until,
    .wait_xon = io_wait_xon,
    .lock = io_lock,
    .unlock = io_unlock,
};

esp_err_t dongle_write(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (s_claimed) return ESP_ERR_INVALID_STATE;  // the flasher has it
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int rc = s_port.h ? tp_write(&s_tp, &s_tp_io, data, len, timeout_ms) : TP_ERR_WRITE;
    bool stalled = rc == TP_ERR_TIMEOUT && s_hw_flow && s_port.h;
    if (stalled) {
        // The radio held CTS off long enough to time a write out: never leave the link stuck on that.
        us_set_cts_flow(&s_port, false);
        s_hw_flow = false;
    }
    xSemaphoreGive(s_tx_lock);
    if (stalled) {
        portENTER_CRITICAL(&s_tp_mux);
        s_line.hw_flow = false;
        s_line.hw_auto_off++;
        portEXIT_CRITICAL(&s_tp_mux);
        stats_event(EV_ERROR, "Hardware flow control turned off: a write to the dongle stalled with CTS held");
    }
    return rc == TP_OK ? ESP_OK : rc == TP_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
}

esp_err_t dongle_set_hw_flow(bool on, char *err, size_t err_len) {
    s_hw_pref = on;
    if (!s_ready || s_port.h == NULL) {
        if (on) snprintf(err, err_len, "No dongle connected; it will be tried when the dongle connects");
        return on ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (on) {
        bool cts = false;
        e = us_get_cts(&s_port, &cts);
        if (e == ESP_ERR_NOT_SUPPORTED) {
            snprintf(err, err_len, "This dongle's USB chip (%s) has no CTS handshake", us_chip_name(s_port.chip));
        } else if (e != ESP_OK) {
            snprintf(err, err_len, "Couldn't read the dongle's CTS line (%s)", esp_err_to_name(e));
        } else if (!cts) {
            snprintf(err, err_len, "The dongle isn't asserting CTS: its RTS isn't wired, or the firmware doesn't drive it");
            e = ESP_ERR_INVALID_STATE;
        }
    }
    if (e == ESP_OK && !s_claimed) {
        e = us_set_cts_flow(&s_port, on);
        if (e == ESP_ERR_NOT_SUPPORTED && !on) e = ESP_OK;
    }
    if (e == ESP_OK) s_hw_flow = on && !s_claimed;
    xSemaphoreGive(s_tx_lock);
    if (e == ESP_OK) {
        portENTER_CRITICAL(&s_tp_mux);
        s_line.hw_flow = s_hw_flow;
        portEXIT_CRITICAL(&s_tp_mux);
        ESP_LOGI(TAG, "CTS handshake %s", s_hw_flow ? "on" : "off");
    }
    return e;
}

void dongle_set_flow(uint16_t chunk, uint16_t gap_us, bool xoff_honor, uint16_t xoff_max_ms) {
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    s_tp.cfg.chunk = chunk > TP_CHUNK_MAX ? TP_CHUNK_MAX : chunk;
    s_tp.cfg.gap_us = gap_us;
    s_tp.cfg.xoff_honor = xoff_honor;
    s_tp.cfg.xoff_max_us = (uint32_t)xoff_max_ms * 1000u;
    xSemaphoreGive(s_tx_lock);
    ESP_LOGI(TAG, "USB OUT: chunk %u (0 = unpaced), gap %u us, XOFF %s (max %u ms)", chunk, gap_us,
             xoff_honor ? "honoured" : "counted only", xoff_max_ms);
}

void dongle_flow_stats(dongle_flow_t *out) {
    // The writer updates its counters under s_tx_lock; don't hold the web server up behind a long hold.
    bool locked = xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(50)) == pdTRUE;
    portENTER_CRITICAL(&s_tp_mux);
    out->st = s_tp.st;
    out->cfg = s_tp.cfg;
    out->xoff_now = s_tp.xoff;
    out->line = s_line;
    portEXIT_CRITICAL(&s_tp_mux);
    if (locked) xSemaphoreGive(s_tx_lock);
}

void dongle_flow_reset_stats(void) {
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_tp_mux);
    memset(&s_tp.st, 0, sizeof(s_tp.st));
    dongle_line_t keep = s_line;
    memset(&s_line, 0, sizeof(s_line));
    s_line.known = keep.known;
    s_line.cts_now = keep.cts_now;
    s_line.hw_flow = keep.hw_flow;
    portEXIT_CRITICAL(&s_tp_mux);
    xSemaphoreGive(s_tx_lock);
}

// ---------------------------------------------------------------- exclusive use (flashing the dongle)

bool dongle_claimed(void) { return s_claimed; }
StreamBufferHandle_t dongle_claim_rx(void) { return s_claim_rx; }

bool dongle_claim(void) {
    if (!s_ready || s_claimed) return false;
    xStreamBufferReset(s_claim_rx);
    s_claimed = true;
    if (s_hw_flow) {
        xSemaphoreTake(s_tx_lock, portMAX_DELAY);
        if (s_port.h) us_set_cts_flow(&s_port, false);
        s_hw_flow = false;
        xSemaphoreGive(s_tx_lock);
    }
    return true;
}

esp_err_t dongle_claimed_write(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (!s_claimed) return ESP_ERR_INVALID_STATE;
    return write_raw(data, len, timeout_ms);
}

esp_err_t dongle_set_baud(uint32_t baud) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    esp_err_t e = s_port.h ? us_set_baud(&s_port, baud) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_tx_lock);
    return e;
}

esp_err_t dongle_set_lines(bool dtr, bool rts) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    esp_err_t e = s_port.h ? us_set_lines(&s_port, dtr, rts) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_tx_lock);
    return e;
}

esp_err_t dongle_wait_reopen(uint32_t timeout_ms) {
    // Flashers call this after a read/write failed because the stick left the bus (ConBee II and nRF52840
    // re-enumerate into and out of their bootloaders). identify() + try_open() bring it back on their own;
    // here we only wait for a fresh attach. If it never actually left (no detach within 1.5 s), it's still ours.
    stats_lock();
    uint32_t att0 = g_stats.dongle_attach, det0 = g_stats.dongle_detach;
    stats_unlock();
    int64_t t0 = esp_timer_get_time(), end = t0 + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < end) {
        stats_lock();
        uint32_t att = g_stats.dongle_attach, det = g_stats.dongle_detach;
        stats_unlock();
        if (att != att0 && s_ready && s_port.h) {
            vTaskDelay(pdMS_TO_TICKS(200));  // let a CDC device settle after DTR goes up
            return ESP_OK;
        }
        if (det == det0 && s_ready && s_port.h && esp_timer_get_time() - t0 > 1500000) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_ERR_TIMEOUT;
}

void dongle_set_app_baud(uint32_t baud) {
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    s_baud = baud;
    s_tp.cfg.baud = baud;  // pacing times chunks against the line
    xSemaphoreGive(s_tx_lock);
    ESP_LOGI(TAG, "Link baud now %lu", (unsigned long)baud);
}

esp_err_t dongle_set_gpio(uint8_t mask, uint8_t state) {
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    esp_err_t e = s_port.h ? us_set_gpio(&s_port, mask, state) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_tx_lock);
    return e;
}

void dongle_release(void) {
    if (!s_claimed) return;
    // Back to how Zigbee2MQTT expects it: its baud, DTR and RTS on.
    dongle_set_baud(s_baud);
    dongle_set_lines(true, true);
    xStreamBufferReset(s_rx);
    s_claimed = false;
    if (s_hw_pref) {
        char err[96];
        if (dongle_set_hw_flow(true, err, sizeof(err)) != ESP_OK)
            stats_event(EV_WARN, "Hardware flow control left off after the dongle update: %s", err);
    }
}

void dongle_start(uint32_t baud) {
    s_baud = baud;
    s_rx = xStreamBufferCreate(RX_STREAM_SIZE, 1);
    s_claim_rx = xStreamBufferCreate(4096, 1);
    s_tx_lock = xSemaphoreCreateMutex();
    s_flow_ev = xEventGroupCreate();
    s_mon_lock = xSemaphoreCreateMutex();
    xEventGroupSetBits(s_flow_ev, FLOW_XON);
    s_info.model = dongle_match(0, 0, NULL, NULL, false);  // "unsupported" until something enumerates
    const tp_cfg_t tp_cfg = {.baud = baud, .chunk = TP_CHUNK_MAX, .gap_us = 300, .xoff_honor = true, .xoff_max_us = 20000};
    tp_init(&s_tp, &tp_cfg);

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(dongle_task, "dongle", 4096, NULL, 6, &s_task);
    xTaskCreate(line_task, "dongle_line", 3072, NULL, 4, NULL);

    // The CDC driver registers before the USB library task runs, so it can't miss the dongle's
    // enumeration at boot (scan_existing covers anything that slips through anyway).
    const cdc_acm_host_driver_config_t drv_cfg = {
        .driver_task_stack_size = 6144,
        .driver_task_priority = 10,
        .xCoreID = 0,
        .new_dev_cb = on_new_dev,
    };
    ESP_ERROR_CHECK(cdc_acm_host_install(&drv_cfg));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    ESP_LOGI(TAG, "USB host ready; waiting for the dongle on J3");
}

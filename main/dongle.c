#include "dongle.h"
#include "tasks.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/usb_dwc_ll.h"
#include "sdkconfig.h"
#include "settings.h"
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
#define FLOW_XON BIT0  // set while the NCP has not said XOFF

struct dongle {
    int idx;
    StreamBufferHandle_t rx;        // what the stick sent, for the radio's client (every radio has one)
    volatile bool claimed;
    SemaphoreHandle_t tx_lock;
    us_port_t port;  // port.h is the open device (NULL when none)
    volatile bool ready;
    volatile bool gone;  // the CDC driver saw it leave; the dongle task closes it
    uint32_t baud;

    // What's plugged in (set from the USB enumeration callback, read by everyone else).
    portMUX_TYPE id_mux;
    dongle_info_t info;
    volatile bool have_target;
    radio_ident_t ident;          // the stick's identity, for the radio binding
    volatile bool bind_pending;   // save ident as this radio's stick (dongle task: NVS)
    volatile bool adopt_pending;  // a new stick took this free radio: set the radio up for it (dongle task)
    volatile bool setup_pending;  // a client connected: save the radio as set up (dongle task: NVS)
    char err[112];                // why the stick can't be used right now ("" = fine), guarded by id_mux
    uint8_t last_port;            // hub port the last stick was on (the flasher's re-enumerating sticks)

    // USB OUT pacing and XON/XOFF (txpace_core.c). tp's XON/XOFF state and stats are guarded by tp_mux
    // (the USB driver task scans incoming bytes); its line state and config by tx_lock.
    tp_state_t tp;
    portMUX_TYPE tp_mux;
    EventGroupHandle_t flow_ev;

    // The USB chip's own view of the UART (line monitor) and its CTS handshake. line is guarded by tp_mux;
    // mon_lock keeps the handle alive while the monitor uses it (close_dongle takes it as well).
    dongle_line_t line;
    SemaphoreHandle_t mon_lock;
    volatile bool hw_flow;  // CTS handshake on right now
    volatile bool hw_pref;  // what the settings ask for
    uint8_t ftdi_buf[64];
};

static dongle_t s_d[RADIO_MAX];
static StreamBufferHandle_t s_claim_rx;  // the flasher's, while it has a stick (one flash at a time)
static TaskHandle_t s_task;
static SemaphoreHandle_t s_assign_lock;  // identify() runs in the CDC driver task and the dongle task
static volatile int s_fl;                // the radio the firmware pages work on

// USB devices (snapshot for /api/usb) and the full-speed-only port.
static portMUX_TYPE s_usb_mux = portMUX_INITIALIZER_UNLOCKED;
static dongle_usb_t s_usb;
static bool s_fs_only;
static int64_t s_last_reforce;
static volatile bool s_reforce_pending;
static volatile int64_t s_bus_change_us;  // last time a stick came or went (free_stale waits for a quiet bus)
// Per USB address: what we already know about it, so the periodic scan doesn't redo (or re-log) the work.
#define SEEN_NONE 0
#define SEEN_HUB 1
#define SEEN_HELD 2
#define SEEN_REFUSED 3
static uint8_t s_seen[128];

// Notification bits for the dongle task
#define NOTIFY_NEW_DEV BIT0
#define NOTIFY_GONE BIT1
#define NOTIFY_REFORCE BIT2
#define NOTIFY_SET_UP BIT3

dongle_t *dongle_at(int radio) { return &s_d[radio < 0 || radio >= RADIO_MAX ? 0 : radio]; }
StreamBufferHandle_t dx_rx_stream(dongle_t *d) { return d->rx; }
bool dx_ready(dongle_t *d) { return d->ready; }
bool dx_identified(dongle_t *d) { return d->have_target; }
bool dx_claimed(dongle_t *d) { return d->claimed; }

void dx_info(dongle_t *d, dongle_info_t *out) {
    portENTER_CRITICAL(&d->id_mux);
    *out = d->info;
    portEXIT_CRITICAL(&d->id_mux);
}

static void rematch_one(dongle_t *d) {
    portENTER_CRITICAL(&d->id_mux);
    bool have = d->info.vid != 0;
    dongle_info_t i = d->info;
    portEXIT_CRITICAL(&d->id_mux);
    if (!have) return;
    const dongle_model_t *m = dongle_match(i.vid, i.pid, i.mfr, i.product, i.has_cdc);
    if (m == i.model) return;
    portENTER_CRITICAL(&d->id_mux);
    d->info.model = m;
    portEXIT_CRITICAL(&d->id_mux);
    stats_event(EV_INFO, "Radio %d: stick now identified as %s (updated stick list)", d->idx + 1, m->name);
}

void dongle_rematch(void) {
    for (int i = 0; i < RADIO_MAX; i++) rematch_one(&s_d[i]);
}

void dx_note_proto(dongle_t *d, proto_t p) {
    portENTER_CRITICAL(&d->id_mux);
    d->info.proto_detected = p;
    portEXIT_CRITICAL(&d->id_mux);
}

void dx_request_set_up(dongle_t *d) {
    d->setup_pending = true;
    if (s_task) xTaskNotify(s_task, NOTIFY_SET_UP, eSetBits);
}

void dx_error(dongle_t *d, char *out, size_t n) {
    portENTER_CRITICAL(&d->id_mux);
    strlcpy(out, d->err, n);
    portEXIT_CRITICAL(&d->id_mux);
}

static void set_err(dongle_t *d, const char *msg) {
    portENTER_CRITICAL(&d->id_mux);
    bool changed = strncmp(d->err, msg, sizeof(d->err)) != 0;
    strlcpy(d->err, msg, sizeof(d->err));
    portEXIT_CRITICAL(&d->id_mux);
    if (changed && msg[0]) stats_event(EV_ERROR, "Radio %d: %s", d->idx + 1, msg);
}

static radio_kind_t kind_of(proto_t p) {
    switch (p) {
        case PROTO_SPINEL: return RADIO_KIND_THREAD;
        case PROTO_ZWAVE: return RADIO_KIND_ZWAVE;
        case PROTO_EZSP:
        case PROTO_ZNP:
        case PROTO_DECONZ:
        case PROTO_ZIGATE:
        case PROTO_ZBOSS: return RADIO_KIND_ZIGBEE;
        default: return RADIO_KIND_UNKNOWN;
    }
}

void dx_note_fw(dongle_t *d, const char *running, const char *version) {
    portENTER_CRITICAL(&d->id_mux);
    strlcpy(d->info.fw_running, running, sizeof(d->info.fw_running));
    strlcpy(d->info.fw_version, version, sizeof(d->info.fw_version));
    portEXIT_CRITICAL(&d->id_mux);
}

static void flow_line_reset(dongle_t *d) {
    portENTER_CRITICAL(&d->tp_mux);
    tp_reset_line(&d->tp);
    portEXIT_CRITICAL(&d->tp_mux);
    xEventGroupSetBits(d->flow_ev, FLOW_XON);
}

void dx_flush_rx(dongle_t *d) {
    if (d->rx) xStreamBufferReset(d->rx);
    flow_line_reset(d);  // a new client starts with no stale XOFF
}

static bool on_rx(const uint8_t *data, size_t len, void *arg) {
    dongle_t *d = arg;
    // FTDI puts 2 status bytes in front of every packet: strip them from a copy (the driver's buffer is
    // read-only). The open forces one 64-byte packet per callback for FTDI.
    if (d->port.chip == US_FTDI) {
        size_t n = len < sizeof(d->ftdi_buf) ? len : sizeof(d->ftdi_buf);
        memcpy(d->ftdi_buf, data, n);
        len = us_rx_fix(&d->port, d->ftdi_buf, n);
        data = d->ftdi_buf;
        if (len == 0) return true;
    }
    if (d->claimed) {
        xStreamBufferSend(s_claim_rx, data, len, 0);
        return true;
    }
    // XON/XOFF stay in the stream (Zigbee2MQTT's ASH layer reads them too); they also gate radio 1's writes. Radios
    // 2 and 3 don't hold on XOFF, and 0x13 is ordinary data in the Z-Wave Serial API: no scan there.
    if (d->idx == 0 && (memchr(data, TP_XOFF, len) || memchr(data, TP_XON, len))) {
        portENTER_CRITICAL(&d->tp_mux);
        bool changed = tp_rx(&d->tp, data, len, esp_timer_get_time());
        bool xoff = d->tp.xoff;
        portEXIT_CRITICAL(&d->tp_mux);
        if (changed) {
            if (xoff) xEventGroupClearBits(d->flow_ev, FLOW_XON);
            else xEventGroupSetBits(d->flow_ev, FLOW_XON);
        }
    }
    size_t sent = xStreamBufferSend(d->rx, data, len, 0);
    if (sent < len) {
        stats_lock();
        if (d->idx == 0) g_stats.dropped_overflow += len - sent;
        g_radio[d->idx].dropped_overflow += len - sent;
        stats_unlock();
    }
    return true;
}

static void on_event(const cdc_acm_host_dev_event_data_t *event, void *ctx) {
    dongle_t *d = ctx;
    switch (event->type) {
        case CDC_ACM_HOST_DEVICE_DISCONNECTED:
            d->ready = false;
            d->gone = true;
            xTaskNotify(s_task, NOTIFY_GONE, eSetBits);
            break;
        case CDC_ACM_HOST_ERROR:
            ESP_LOGW(TAG, "Radio %d: USB error %d", d->idx + 1, event->data.error);
            break;
        default:
            break;
    }
}

static void usb_str(const usb_str_desc_t *s, char *out, size_t out_len) {
    out[0] = 0;
    if (s == NULL) return;
    size_t n = (s->bLength - 2) / 2, j = 0;
    for (size_t i = 0; i < n && j + 1 < out_len; i++) {
        uint16_t c = s->wData[i];
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

// ---------------------------------------------------------------- full-speed-only J3

// HCFG.FSLSSupp on the high-speed core: the port never answers a hub's high-speed chirp, so every hub
// enumerates at full speed, and the full-speed sticks behind it need no transaction translator (which the P4
// doesn't have). The register field is only ever changed field by field by ESP-IDF, so it stays set until a
// core soft reset (hcd_port_recover, after an unplug), which the hub check below catches.
static void force_full_speed(void) {
    usb_dwc_ll_hcfg_set_fsls_supp_only(USB_DWC_LL_GET_HW(0));
    portENTER_CRITICAL(&s_usb_mux);
    s_usb.fs_forced = true;
    portEXIT_CRITICAL(&s_usb_mux);
}

static void note_hub(uint8_t addr, bool high_speed) {
    if (s_seen[addr] == SEEN_HUB) return;
    s_seen[addr] = SEEN_HUB;
    if (radio_hub_reforce(s_fs_only, true, high_speed, esp_timer_get_time(), &s_last_reforce)) {
        stats_event(EV_WARN, "USB hub came up at high speed: forcing J3 back to full speed (sticks behind a high-speed hub can't work)");
        s_reforce_pending = true;
        if (s_task) xTaskNotify(s_task, NOTIFY_REFORCE, eSetBits);
    } else if (high_speed) {
        stats_event(EV_WARN, "USB hub at high speed: full-speed sticks behind it won't enumerate (turn on full-speed-only USB, or use a USB 1.1 hub)");
    } else {
        stats_event(EV_INFO, "USB hub attached (full speed)");
    }
}

// ---------------------------------------------------------------- which radio a stick belongs to

static void port_name(uint8_t port, char *out, size_t n) {
    if (port) snprintf(out, n, "hub port %u", port);
    else strlcpy(out, "J3", n);
}

static void close_dongle(dongle_t *d, bool may_free);

// Sticks refused earlier get another look at the next bus scan (a radio was freed).
static void retry_refused(void) {
    for (int a = 0; a < 128; a++)
        if (s_seen[a] == SEEN_REFUSED) s_seen[a] = SEEN_NONE;
    if (s_task) xTaskNotify(s_task, NOTIFY_NEW_DEV, eSetBits);
}

static void note_refused(uint8_t addr, const dongle_info_t *i, const char *why) {
    if (s_seen[addr] == SEEN_REFUSED) return;
    s_seen[addr] = SEEN_REFUSED;
    portENTER_CRITICAL(&s_usb_mux);
    s_usb.refused++;
    portEXIT_CRITICAL(&s_usb_mux);
    char where[16];
    port_name(i->hub_port, where, sizeof(where));
    stats_event(EV_WARN, "USB stick %04x:%04x '%s' on %s not used: %s", i->vid, i->pid, i->product, where, why);
}

// Works out what this USB device is and which radio it belongs to. Runs in the CDC driver task (new device)
// or the dongle task (scan).
static void identify(usb_device_handle_t usb_dev) {
    const usb_device_desc_t *desc;
    if (usb_host_get_device_descriptor(usb_dev, &desc) != ESP_OK) return;
    usb_device_info_t info;
    if (usb_host_device_info(usb_dev, &info) != ESP_OK) return;
    uint8_t addr = info.dev_addr & 0x7F;
    if (desc->bDeviceClass == USB_CLASS_HUB) {
        note_hub(addr, info.speed == USB_SPEED_HIGH);
        return;
    }
    dongle_info_t di = {0};
    usb_str(info.str_desc_manufacturer, di.mfr, sizeof(di.mfr));
    usb_str(info.str_desc_product, di.product, sizeof(di.product));
    usb_str(info.str_desc_serial_num, di.serial, sizeof(di.serial));
    di.vid = desc->idVendor;
    di.pid = desc->idProduct;
    bool cdc = has_cdc_interface(usb_dev);
    di.has_cdc = cdc;
    di.chip = us_chip_for(di.vid, di.pid, cdc);
    di.model = dongle_match(di.vid, di.pid, di.mfr, di.product, cdc);
    di.speed_full = info.speed != USB_SPEED_HIGH;
    di.usb_addr = addr;
    di.hub_port = info.parent.dev_hdl ? info.parent.port_num : 0;

    if (di.chip == US_NONE) {
        if (s_seen[addr] != SEEN_REFUSED) {
            ESP_LOGI(TAG, "USB device %04x:%04x '%s' isn't a serial adapter", di.vid, di.pid, di.product);
            note_refused(addr, &di, "not a USB serial adapter");
        }
        return;
    }

    xSemaphoreTake(s_assign_lock, portMAX_DELAY);
    for (int i = 0; i < RADIO_MAX; i++) {
        if (s_d[i].have_target && s_d[i].info.usb_addr == addr) {  // already ours (scan saw it again)
            xSemaphoreGive(s_assign_lock);
            return;
        }
    }
    radio_ident_t id = {.vid = di.vid, .pid = di.pid, .port = di.hub_port};
    strlcpy(id.serial, di.serial, sizeof(id.serial));
    int r = RADIO_NONE;
    radio_assign_t how = RA_FULL;
    // A stick being flashed may leave the bus and come back as its bootloader (another USB ID and serial): on
    // the same hub port it is still the flashed radio's, and that radio keeps its saved stick.
    dongle_t *fl = &s_d[s_fl];
    if (fl->claimed && !fl->have_target && fl->last_port == id.port) {
        r = fl->idx;
        how = RA_OK;
    } else {
        radio_slot_t slots[RADIO_MAX];
        settings_radio_slots(slots);
        bool in_use[RADIO_MAX];
        for (int i = 0; i < RADIO_MAX; i++) in_use[i] = s_d[i].have_target;
        r = radio_assign(slots, in_use, &id, &how);
        if (r != RADIO_NONE && how == RA_BOUND && slots[r].bound.port != id.port) how = RA_REPLACED;  // save the new port
    }
    if (r == RADIO_NONE) {
        xSemaphoreGive(s_assign_lock);
        note_refused(addr, &di,
                     "every radio is taken (a set-up radio keeps its stick while it's unplugged: Forget one on "
                     "Settings, Radios to free it)");
        return;
    }
    dongle_t *d = &s_d[r];
    char where[16];
    port_name(di.hub_port, where, sizeof(where));
    ESP_LOGI(TAG, "Radio %d: USB device %04x:%04x '%s' '%s' serial %s (%s speed, %s): %s via %s", r + 1, di.vid,
             di.pid, di.mfr, di.product, di.serial,
             info.speed == USB_SPEED_HIGH ? "high" : info.speed == USB_SPEED_FULL ? "full" : "low", where,
             di.model->name, us_chip_name(di.chip));
    portENTER_CRITICAL(&d->id_mux);
    d->info = di;
    portEXIT_CRITICAL(&d->id_mux);
    d->ident = id;
    d->last_port = id.port;
    d->gone = false;
    d->have_target = true;
    if (how == RA_NEW) d->adopt_pending = true;
    else if (how == RA_REPLACED) d->bind_pending = true;
    s_seen[addr] = SEEN_HELD;
    if (how != RA_NEW) settings_radio_present(r, true);  // a new stick's radio shows once adopt() has set it up
    s_bus_change_us = esp_timer_get_time();
    xSemaphoreGive(s_assign_lock);

    stats_lock();
    if (r == 0) {
        g_stats.dongle_present = true;
        g_stats.dongle_vid = di.vid;
        g_stats.dongle_pid = di.pid;
        strlcpy(g_stats.dongle_mfr, di.mfr, sizeof(g_stats.dongle_mfr));
        strlcpy(g_stats.dongle_product, di.product, sizeof(g_stats.dongle_product));
        strlcpy(g_stats.dongle_serial, di.serial, sizeof(g_stats.dongle_serial));
    }
    g_radio[r].present = true;
    stats_unlock();
    if (s_task) xTaskNotify(s_task, NOTIFY_NEW_DEV, eSetBits);
}

static void on_new_dev(usb_device_handle_t usb_dev) {
    usb_device_info_t info;
    if (usb_host_device_info(usb_dev, &info) == ESP_OK) s_seen[info.dev_addr & 0x7F] = SEEN_NONE;  // fresh device
    identify(usb_dev);
}

// The USB devices on the bus right now (own lightweight USB client): picks up sticks that were attached
// before we were listening, forgets addresses that have gone, and refreshes the snapshot /api/usb shows.
static usb_host_client_handle_t s_scan_client;
static void scan_cb(const usb_host_client_event_msg_t *msg, void *arg) {
    (void)msg;
    (void)arg;
}

static void scan_bus(void) {
    if (s_scan_client == NULL) {
        const usb_host_client_config_t cc = {
            .is_synchronous = false,
            .max_num_event_msg = 5,
            .async = {.client_event_callback = scan_cb, .callback_arg = NULL},
        };
        if (usb_host_client_register(&cc, &s_scan_client) != ESP_OK) return;
    }
    usb_host_client_handle_events(s_scan_client, 0);  // drain (we only scan)
    uint8_t addrs[DONGLE_USB_MAX];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) != ESP_OK) return;
    if (n > DONGLE_USB_MAX) n = DONGLE_USB_MAX;
    bool present[128] = {0};
    dongle_usb_t snap = {0};
    for (int i = 0; i < n; i++) {
        uint8_t a = addrs[i] & 0x7F;
        present[a] = true;
        usb_device_handle_t dev;
        if (usb_host_device_open(s_scan_client, addrs[i], &dev) != ESP_OK) continue;
        const usb_device_desc_t *desc;
        usb_device_info_t info;
        if (usb_host_get_device_descriptor(dev, &desc) == ESP_OK && usb_host_device_info(dev, &info) == ESP_OK) {
            dongle_usb_dev_t *u = &snap.dev[snap.n++];
            u->addr = a;
            u->vid = desc->idVendor;
            u->pid = desc->idProduct;
            u->is_hub = desc->bDeviceClass == USB_CLASS_HUB;
            u->high_speed = info.speed == USB_SPEED_HIGH;
            u->hub_port = info.parent.dev_hdl ? info.parent.port_num : 0;
            usb_str(info.str_desc_product, u->product, sizeof(u->product));
            if (s_seen[a] == SEEN_NONE) identify(dev);
            u->radio = -1;
            for (int r = 0; r < RADIO_MAX; r++)
                if (s_d[r].have_target && s_d[r].info.usb_addr == a) u->radio = (int8_t)r;
        }
        usb_host_device_close(s_scan_client, dev);
    }
    for (int a = 0; a < 128; a++)
        if (!present[a] && s_seen[a] != SEEN_HELD) s_seen[a] = SEEN_NONE;  // gone: a new device may get the address
    // A stick that was given a radio but left before it ever opened sends no disconnect: free its radio here.
    for (int r = 0; r < RADIO_MAX; r++) {
        dongle_t *d = &s_d[r];
        if (d->have_target && d->port.h == NULL && !present[d->info.usb_addr & 0x7F]) {
            ESP_LOGW(TAG, "Radio %d: stick left before it opened", r + 1);
            close_dongle(d, true);
        }
    }
    portENTER_CRITICAL(&s_usb_mux);
    snap.fs_only = s_fs_only;
    snap.fs_forced = s_usb.fs_forced;
    snap.reforces = s_usb.reforces;
    snap.refused = s_usb.refused;
    s_usb = snap;
    portEXIT_CRITICAL(&s_usb_mux);
}

void dongle_usb_snapshot(dongle_usb_t *out) {
    portENTER_CRITICAL(&s_usb_mux);
    *out = s_usb;
    portEXIT_CRITICAL(&s_usb_mux);
}

static esp_err_t set_hw_flow(dongle_t *d, bool on, char *err, size_t err_len);

static void try_open(dongle_t *d) {
    if (d->port.h != NULL || !d->have_target || d->adopt_pending) return;  // a new stick opens once its radio is set up
    dongle_info_t i;
    dx_info(d, &i);
    const cdc_acm_host_device_config_t cfg = {
        .connection_timeout_ms = 500,
        .out_buffer_size = 512,
        .in_buffer_size = 512,
        .event_cb = on_event,
        .data_cb = on_rx,
        .user_arg = d,
    };
    us_port_t p;
    esp_err_t e = us_open(&p, i.chip, i.vid, i.pid, i.usb_addr, &cfg);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "Radio %d: open %04x:%04x (address %u) as %s failed: %s", d->idx + 1, i.vid, i.pid, i.usb_addr,
                 us_chip_name(i.chip), esp_err_to_name(e));
        char msg[112];
        // Each open stick takes host channels (3 for CP210x/CH34x/FTDI, 4 for CDC-ACM; a hub takes 2) and J3 has 16.
        snprintf(msg, sizeof(msg), "can't open the stick (%s)%s", esp_err_to_name(e),
                 e == ESP_ERR_NO_MEM || e == ESP_ERR_NOT_FINISHED || e == ESP_ERR_NOT_SUPPORTED
                     ? ": the USB host may be out of channels for one more stick"
                     : "");
        set_err(d, msg);
        return;
    }
    // on_rx reads port.chip (FTDI stripping) from the first byte on
    xSemaphoreTake(d->tx_lock, portMAX_DELAY);
    d->port = p;
    xSemaphoreGive(d->tx_lock);
    e = us_setup(&d->port, d->baud);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Radio %d: %s setup failed: %s", d->idx + 1, us_chip_name(i.chip), esp_err_to_name(e));
        char msg[112];
        snprintf(msg, sizeof(msg), "stick found, but its USB-UART (%s) setup failed (%s)", us_chip_name(i.chip),
                 esp_err_to_name(e));
        set_err(d, msg);
        xSemaphoreTake(d->tx_lock, portMAX_DELAY);
        xSemaphoreTake(d->mon_lock, portMAX_DELAY);
        us_close(&d->port);
        xSemaphoreGive(d->mon_lock);
        xSemaphoreGive(d->tx_lock);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    dx_flush_rx(d);
    set_err(d, "");
    d->ready = true;
    stats_lock();
    if (d->idx == 0) {
        g_stats.dongle_present = true;
        g_stats.dongle_open = true;
        g_stats.dongle_attach++;
        g_stats.dongle_since_s = stats_uptime_s();
    }
    radio_stats_t *rs = &g_radio[d->idx];
    rs->present = rs->open = true;
    rs->attach++;
    rs->since_s = stats_uptime_s();
    stats_unlock();
    ESP_LOGI(TAG, "Radio %d: stick open at %lu baud", d->idx + 1, (unsigned long)d->baud);
    char where[16];
    port_name(i.hub_port, where, sizeof(where));
    stats_event(EV_INFO, "Radio %d: stick connected: %s (%s), serial %s, %s, %lu baud", d->idx + 1,
                i.product[0] ? i.product : i.model->name, us_chip_name(i.chip), i.serial[0] ? i.serial : "?", where,
                (unsigned long)d->baud);
    if (d->hw_pref) {
        char err[96];
        if (set_hw_flow(d, true, err, sizeof(err)) != ESP_OK)
            stats_event(EV_WARN, "Radio %d: hardware flow control left off: %s", d->idx + 1, err);
    }
}

// The stick left (or the port is being power-cycled: may_free false). A radio nothing was set up on is free
// again (radio_unplug_frees); a set-up one waits for its stick and says so.
static void close_dongle(dongle_t *d, bool may_free) {
    d->ready = false;
    d->gone = false;
    xSemaphoreTake(s_assign_lock, portMAX_DELAY);
    if (d->have_target) s_seen[d->info.usb_addr] = SEEN_NONE;
    bool was = d->have_target;
    d->have_target = false;
    d->bind_pending = d->adopt_pending = false;
    bool set_up = settings_radio_is_set_up(d->idx);
    bool freed = was && may_free && radio_unplug_frees(set_up, d->claimed);
    if (was) s_bus_change_us = esp_timer_get_time();
    if (freed) settings_radio_free(d->idx);
    settings_radio_present(d->idx, false);
    xSemaphoreGive(s_assign_lock);
    set_err(d, "");
    if (was) retry_refused();  // a stick refused for want of a free radio may take this one now
    if (d->port.h == NULL) return;
    xSemaphoreTake(d->tx_lock, portMAX_DELAY);
    xSemaphoreTake(d->mon_lock, portMAX_DELAY);
    us_close(&d->port);
    d->hw_flow = false;
    xSemaphoreGive(d->mon_lock);
    xSemaphoreGive(d->tx_lock);
    portENTER_CRITICAL(&d->tp_mux);
    d->line.known = false;
    d->line.hw_flow = false;
    portEXIT_CRITICAL(&d->tp_mux);
    portENTER_CRITICAL(&d->id_mux);
    d->info.proto_detected = PROTO_UNKNOWN;
    d->info.fw_running[0] = d->info.fw_version[0] = 0;
    portEXIT_CRITICAL(&d->id_mux);
    stats_lock();
    if (d->idx == 0) {
        g_stats.dongle_present = false;
        g_stats.dongle_open = false;
        g_stats.dongle_detach++;
    }
    radio_stats_t *rs = &g_radio[d->idx];
    rs->present = rs->open = false;
    rs->detach++;
    stats_unlock();
    ESP_LOGW(TAG, "Radio %d: stick disconnected", d->idx + 1);
    if (!may_free || d->claimed) stats_event(EV_WARN, "Radio %d: stick disconnected", d->idx + 1);
    else if (freed && d->idx > 0)
        stats_event(EV_INFO, "Radio %d: stick unplugged; nothing was set up on it, so the radio is free again", d->idx + 1);
    else if (set_up)
        stats_event(EV_ERROR, "Radio %d: stick missing: its client can't reach its network until the stick is back",
                    d->idx + 1);
    else stats_event(EV_WARN, "Radio %d: stick unplugged", d->idx + 1);
}

// Once the bus has settled after boot: a radio still bound to a stick that never showed up, and that nothing was
// set up on, would otherwise wait forever (no unplug ever comes for it).
static void free_stale(void) {
    radio_slot_t slots[RADIO_MAX];
    bool bound[RADIO_MAX], su[RADIO_MAX], present[RADIO_MAX];
    xSemaphoreTake(s_assign_lock, portMAX_DELAY);
    settings_radio_slots(slots);
    for (int i = 0; i < RADIO_MAX; i++) {
        bound[i] = !radio_ident_empty(&slots[i].bound);
        su[i] = settings_radio_is_set_up(i);
        present[i] = s_d[i].have_target || s_d[i].claimed;
    }
    uint32_t m = radio_stale_mask(bound, su, present);
    for (int i = 0; i < RADIO_MAX; i++)
        if ((m >> i) & 1u) settings_radio_free(i);
    xSemaphoreGive(s_assign_lock);
    for (int i = 0; i < RADIO_MAX; i++)
        if ((m >> i) & 1u)
            stats_event(EV_INFO, "Radio %d: its stick didn't come back after the restart and nothing was set up on it: radio freed",
                        i + 1);
    if (m) retry_refused();
}

// A new stick took a free radio: set the radio up for it (role from what the stick is, its baud, a free port,
// radio 1's allowed host) before it opens.
static void adopt(dongle_t *d) {
    dongle_info_t i;
    dx_info(d, &i);
    radio_ident_t id = d->ident;
    settings_radio_adopt(d->idx, &id, kind_of(i.model->proto), i.model->baud);
    uint32_t baud = settings_radio_baud(d->idx);
    if (d->port.h) dx_set_link_baud(d, baud);  // can't happen (try_open waits for adopt), but never run at the old baud
    xSemaphoreTake(d->tx_lock, portMAX_DELAY);
    d->baud = baud;
    d->tp.cfg.baud = baud;
    xSemaphoreGive(d->tx_lock);
    settings_radio_present(d->idx, true);  // its port opens now, on the port just picked
    char where[16], name[16];
    port_name(id.port, where, sizeof(where));
    settings_radio_name(d->idx, name, sizeof(name));
    stats_event(EV_INFO, "Radio %d: new stick %s on %s: %s on TCP %u at %lu baud", d->idx + 1,
                i.product[0] ? i.product : i.model->name, where, name[0] ? name : radio_role_name(settings_radio_role(d->idx)),
                settings_radio_port(d->idx), (unsigned long)baud);
}

static void reforce_full_speed(void) {
    // Power-cycle the root port with the bit set: the hub (and everything behind it) enumerates again at full speed.
    ESP_LOGW(TAG, "Re-forcing full speed on J3");
    portENTER_CRITICAL(&s_usb_mux);
    s_usb.fs_forced = false;  // a core reset may have cleared it; set again below
    s_usb.reforces++;
    portEXIT_CRITICAL(&s_usb_mux);
    usb_host_lib_set_root_port_power(false);
    vTaskDelay(pdMS_TO_TICKS(200));
    // Every stick behind the hub just left. This task is the one that closes them, so do it now: the port can only
    // power on again once the USB library has freed every device and recovered the port.
    for (int i = 0; i < RADIO_MAX; i++)
        if (s_d[i].have_target || s_d[i].port.h) close_dongle(&s_d[i], false);  // they come straight back
    for (int a = 0; a < 128; a++)
        if (s_seen[a] == SEEN_HUB) s_seen[a] = SEEN_NONE;  // the hub comes back: check its speed again
    for (int attempt = 0; attempt < 40; attempt++) {  // up to ~10 s
        force_full_speed();  // right before each power-on: a recovery in between resets the core
        esp_err_t e = usb_host_lib_set_root_port_power(true);
        if (e == ESP_OK) return;
        watchdog_feed();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    stats_event(EV_ERROR, "J3 didn't power on again after re-forcing full speed: reboot the bridge");
}

static void dongle_task(void *arg) {
    (void)arg;
    watchdog_task_add();
    uint32_t loops = 0;
    bool stale_done = false;
    for (;;) {
        watchdog_feed();
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, pdMS_TO_TICKS(2000));
        for (int i = 0; i < RADIO_MAX; i++)
            if (s_d[i].gone) close_dongle(&s_d[i], true);
        if ((bits & NOTIFY_REFORCE) && s_reforce_pending) {
            s_reforce_pending = false;
            reforce_full_speed();
        }
        for (int i = 0; i < RADIO_MAX; i++) {
            dongle_t *d = &s_d[i];
            if (d->adopt_pending) {
                d->adopt_pending = false;
                adopt(d);
            }
            if (d->setup_pending) {
                d->setup_pending = false;
                settings_radio_mark_set_up(i);  // skips a radio that was freed meanwhile
            }
            if (d->bind_pending) {
                d->bind_pending = false;
                radio_ident_t id = d->ident;
                settings_radio_bind(i, &id);
                char where[16];
                port_name(id.port, where, sizeof(where));
                stats_event(EV_INFO, "Radio %d: saved stick %04x:%04x serial %s on %s as this radio's", i + 1, id.vid,
                            id.pid, id.serial[0] ? id.serial : "(none)", where);
            }
            if (d->port.h == NULL) try_open(d);
        }
        // Every ~10 s (and on attach/detach): look at the bus.
        if (++loops % 5 == 0 || (bits & (NOTIFY_NEW_DEV | NOTIFY_GONE))) scan_bus();
        // Once the hub and its sticks have enumerated: 20 s up, 10 s without a stick coming or going, no re-force or
        // flash under way.
        int64_t now = esp_timer_get_time();
        bool busy = s_reforce_pending;
        for (int i = 0; i < RADIO_MAX; i++) busy |= s_d[i].claimed || s_d[i].adopt_pending;
        if (!stale_done && now > 20LL * 1000 * 1000 && now - s_bus_change_us > 10LL * 1000 * 1000 && !busy) {
            stale_done = true;
            free_stale();
        }
    }
}

// Samples each USB chip's CTS input (the radio's RTS, if the board wires it) and its UART status: errors on
// what it received from the radio, why its transmitter is held, and how much it has queued each way.
static void line_poll(dongle_t *d, uint32_t n, bool *last_cts) {
    if (!d->ready || d->claimed) return;
    if (d->port.chip != US_CP210X && d->port.chip != US_CH34X && d->port.chip != US_FTDI) return;  // can't tell
    if (xSemaphoreTake(d->mon_lock, pdMS_TO_TICKS(5)) != pdTRUE) return;
    bool cts = false;
    us_comm_t cs;
    esp_err_t e = d->port.h ? us_get_cts(&d->port, &cts) : ESP_ERR_INVALID_STATE;
    esp_err_t e2 = ESP_ERR_INVALID_STATE;
    if (d->port.h && e == ESP_OK && n % LINE_STATUS_EVERY == 0) e2 = us_get_comm(&d->port, &cs);
    xSemaphoreGive(d->mon_lock);

    portENTER_CRITICAL(&d->tp_mux);
    dongle_line_t *l = &d->line;
    if (e != ESP_OK) {
        l->poll_fail++;
    } else {
        l->known = true;
        l->cts_now = cts;
        l->samples++;
        if (!cts) l->cts_off++;
        if (*last_cts && !cts) l->cts_drops++;
        *last_cts = cts;
    }
    if (e2 == ESP_OK) {
        l->status_polls++;
        if (cs.brk) l->err_break++;
        if (cs.framing) l->err_framing++;
        if (cs.overrun) l->err_hw_overrun++;
        if (cs.queue_overrun) l->err_queue_overrun++;
        if (cs.parity) l->err_parity++;
        if (cs.hold_cts) l->hold_cts++;
        if (cs.hold_xon) l->hold_xon++;
        if (cs.in_queue > l->in_queue_max) l->in_queue_max = cs.in_queue;
        if (cs.out_queue > l->out_queue_max) l->out_queue_max = cs.out_queue;
    }
    portEXIT_CRITICAL(&d->tp_mux);
}

// Radio 1 only: its line and flow stats are the ones the dashboard shows. Each poll is a USB control transfer, so
// sticks that can't report (CDC-ACM), or nothing open, get a slow look instead of 100 a second.
static void line_task(void *arg) {
    (void)arg;
    uint32_t n = 0;
    bool last_cts = true;
    dongle_t *d = &s_d[0];
    for (;;) {
        bool can = d->ready && !d->claimed && (d->port.chip == US_CP210X || d->port.chip == US_CH34X || d->port.chip == US_FTDI);
        vTaskDelay(pdMS_TO_TICKS(can ? LINE_POLL_MS : 250));
        if (can) line_poll(d, ++n, &last_cts);
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

static esp_err_t write_raw(dongle_t *d, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (!d->ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->tx_lock, portMAX_DELAY);
    esp_err_t e = d->port.h ? cdc_acm_host_data_tx_blocking(d->port.h, data, len, timeout_ms) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(d->tx_lock);
    return e;
}

// ---------------------------------------------------------------- paced writes (txpace_core.c)

static int io_write(void *ctx, const uint8_t *buf, size_t len, uint32_t timeout_ms) {
    dongle_t *d = ctx;
    if (d->port.h == NULL) return -1;
    esp_err_t e = cdc_acm_host_data_tx_blocking(d->port.h, buf, len, timeout_ms);
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
    int64_t dt = t - esp_timer_get_time();
    if (dt > 2500) vTaskDelay(pdMS_TO_TICKS((dt - 1500) / 1000));
    dt = t - esp_timer_get_time();
    if (dt > 0) esp_rom_delay_us((uint32_t)dt);
}

static void io_wait_xon(void *ctx, int64_t deadline) {
    dongle_t *d = ctx;
    int64_t dt = deadline - esp_timer_get_time();
    if (dt <= 0) return;
    TickType_t ticks = pdMS_TO_TICKS((dt + 999) / 1000);
    xEventGroupWaitBits(d->flow_ev, FLOW_XON, pdFALSE, pdTRUE, ticks ? ticks : 1);
}

static void io_lock(void *ctx) {
    dongle_t *d = ctx;
    portENTER_CRITICAL(&d->tp_mux);
}

static void io_unlock(void *ctx) {
    dongle_t *d = ctx;
    portEXIT_CRITICAL(&d->tp_mux);
}

static const tp_io_t s_tp_io = {
    .write = io_write,
    .now_us = io_now,
    .wait_until = io_wait_until,
    .wait_xon = io_wait_xon,
    .lock = io_lock,
    .unlock = io_unlock,
};

esp_err_t dx_write(dongle_t *d, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    if (d->claimed) return ESP_ERR_INVALID_STATE;  // the flasher has it
    if (!d->ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->tx_lock, portMAX_DELAY);
    tp_io_t io = s_tp_io;
    io.ctx = d;
    int rc = d->port.h ? tp_write(&d->tp, &io, data, len, timeout_ms) : TP_ERR_WRITE;
    bool stalled = rc == TP_ERR_TIMEOUT && d->hw_flow && d->port.h;
    if (stalled) {
        // The radio held CTS off long enough to time a write out: never leave the link stuck on that.
        us_set_cts_flow(&d->port, false);
        d->hw_flow = false;
    }
    xSemaphoreGive(d->tx_lock);
    if (stalled) {
        portENTER_CRITICAL(&d->tp_mux);
        d->line.hw_flow = false;
        d->line.hw_auto_off++;
        portEXIT_CRITICAL(&d->tp_mux);
        stats_event(EV_ERROR, "Radio %d: hardware flow control turned off: a write stalled with CTS held", d->idx + 1);
    }
    return rc == TP_OK ? ESP_OK : rc == TP_ERR_TIMEOUT ? ESP_ERR_TIMEOUT : ESP_FAIL;
}

static esp_err_t set_hw_flow(dongle_t *d, bool on, char *err, size_t err_len) {
    d->hw_pref = on;
    if (!d->ready || d->port.h == NULL) {
        if (on) snprintf(err, err_len, "No stick connected; it will be tried when the stick connects");
        return on ? ESP_ERR_INVALID_STATE : ESP_OK;
    }
    xSemaphoreTake(d->tx_lock, portMAX_DELAY);
    esp_err_t e = ESP_OK;
    if (on) {
        bool cts = false;
        e = us_get_cts(&d->port, &cts);
        if (e == ESP_ERR_NOT_SUPPORTED) {
            snprintf(err, err_len, "This stick's USB chip (%s) has no CTS handshake", us_chip_name(d->port.chip));
        } else if (e != ESP_OK) {
            snprintf(err, err_len, "Couldn't read the stick's CTS line (%s)", esp_err_to_name(e));
        } else if (!cts) {
            snprintf(err, err_len, "The stick isn't asserting CTS: its RTS isn't wired, or the firmware doesn't drive it");
            e = ESP_ERR_INVALID_STATE;
        }
    }
    if (e == ESP_OK && !d->claimed) {
        e = us_set_cts_flow(&d->port, on);
        if (e == ESP_ERR_NOT_SUPPORTED && !on) e = ESP_OK;
    }
    if (e == ESP_OK) d->hw_flow = on && !d->claimed;
    xSemaphoreGive(d->tx_lock);
    if (e == ESP_OK) {
        portENTER_CRITICAL(&d->tp_mux);
        d->line.hw_flow = d->hw_flow;
        portEXIT_CRITICAL(&d->tp_mux);
        ESP_LOGI(TAG, "Radio %d: CTS handshake %s", d->idx + 1, d->hw_flow ? "on" : "off");
    }
    return e;
}

static void set_flow(dongle_t *d, uint16_t chunk, uint16_t gap_us, bool xoff_honor, uint16_t xoff_max_ms) {
    xSemaphoreTake(d->tx_lock, portMAX_DELAY);
    d->tp.cfg.chunk = chunk > TP_CHUNK_MAX ? TP_CHUNK_MAX : chunk;
    d->tp.cfg.gap_us = gap_us;
    d->tp.cfg.xoff_honor = xoff_honor;
    d->tp.cfg.xoff_max_us = (uint32_t)xoff_max_ms * 1000u;
    xSemaphoreGive(d->tx_lock);
    ESP_LOGI(TAG, "Radio %d: USB OUT chunk %u (0 = unpaced), gap %u us, XOFF %s (max %u ms)", d->idx + 1, chunk,
             gap_us, xoff_honor ? "honoured" : "counted only", xoff_max_ms);
}

static void dx_flow_stats(dongle_t *d, dongle_flow_t *out) {
    // The writer updates its counters under tx_lock; don't hold the web server up behind a long hold.
    bool locked = xSemaphoreTake(d->tx_lock, pdMS_TO_TICKS(50)) == pdTRUE;
    portENTER_CRITICAL(&d->tp_mux);
    out->st = d->tp.st;
    out->cfg = d->tp.cfg;
    out->xoff_now = d->tp.xoff;
    out->line = d->line;
    portEXIT_CRITICAL(&d->tp_mux);
    if (locked) xSemaphoreGive(d->tx_lock);
}

static void dx_flow_reset_stats(dongle_t *d) {
    xSemaphoreTake(d->tx_lock, portMAX_DELAY);
    portENTER_CRITICAL(&d->tp_mux);
    memset(&d->tp.st, 0, sizeof(d->tp.st));
    dongle_line_t keep = d->line;
    memset(&d->line, 0, sizeof(d->line));
    d->line.known = keep.known;
    d->line.cts_now = keep.cts_now;
    d->line.hw_flow = keep.hw_flow;
    portEXIT_CRITICAL(&d->tp_mux);
    xSemaphoreGive(d->tx_lock);
}

static void dx_set_app_baud(dongle_t *d, uint32_t baud) {
    xSemaphoreTake(d->tx_lock, portMAX_DELAY);
    d->baud = baud;
    d->tp.cfg.baud = baud;  // pacing times chunks against the line
    xSemaphoreGive(d->tx_lock);
    ESP_LOGI(TAG, "Radio %d: link baud now %lu", d->idx + 1, (unsigned long)baud);
}

esp_err_t dx_forget(dongle_t *d, char *err, size_t err_len) {
    xSemaphoreTake(s_assign_lock, portMAX_DELAY);
    bool busy = d->have_target || d->adopt_pending || d->bind_pending || d->claimed;
    if (!busy) settings_radio_free(d->idx);
    xSemaphoreGive(s_assign_lock);
    if (busy) {
        snprintf(err, err_len, "Radio %d's stick is plugged in: unplug it first", d->idx + 1);
        return ESP_ERR_INVALID_STATE;
    }
    stats_event(EV_INFO, "Radio %d forgotten: the next new stick takes it", d->idx + 1);
    retry_refused();
    return ESP_OK;
}

esp_err_t dx_set_link_baud(dongle_t *d, uint32_t baud) {
    if (d->claimed) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->tx_lock, portMAX_DELAY);
    esp_err_t e = d->port.h ? us_set_baud(&d->port, baud) : ESP_OK;
    if (e == ESP_OK) {
        d->baud = baud;
        d->tp.cfg.baud = baud;  // pacing times chunks against the line
    }
    xSemaphoreGive(d->tx_lock);
    if (e == ESP_OK) ESP_LOGI(TAG, "Radio %d: baud now %lu", d->idx + 1, (unsigned long)baud);
    return e;
}

// ---------------------------------------------------------------- radio 0

void dongle_info(dongle_info_t *out) { dx_info(&s_d[0], out); }
bool dongle_ready(void) { return s_d[0].ready; }
bool dongle_identified(void) { return s_d[0].have_target; }
void dongle_set_flow(uint16_t chunk, uint16_t gap_us, bool xoff_honor, uint16_t xoff_max_ms) {
    set_flow(&s_d[0], chunk, gap_us, xoff_honor, xoff_max_ms);
}
esp_err_t dongle_set_hw_flow(bool on, char *err, size_t err_len) { return set_hw_flow(&s_d[0], on, err, err_len); }
void dongle_flow_stats(dongle_flow_t *out) { dx_flow_stats(&s_d[0], out); }
void dongle_flow_reset_stats(void) {
    for (int i = 0; i < RADIO_MAX; i++) dx_flow_reset_stats(&s_d[i]);
}

// ---------------------------------------------------------------- exclusive use (flashing a stick)

static dongle_t *fl(void) { return &s_d[s_fl]; }

bool dongle_fl_select(int radio) {
    if (radio < 0 || radio >= RADIO_MAX) return false;
    for (int i = 0; i < RADIO_MAX; i++)
        if (s_d[i].claimed) return radio == s_fl;  // a flash is running: only "the same radio" is fine
    s_fl = radio;
    return true;
}
int dongle_fl_index(void) { return s_fl; }
void dongle_fl_info(dongle_info_t *out) { dx_info(fl(), out); }
bool dongle_fl_ready(void) { return fl()->ready; }
void dongle_fl_note_fw(const char *running, const char *version) { dx_note_fw(fl(), running, version); }
esp_err_t dongle_fl_set_hw_flow(bool on, char *err, size_t err_len) { return set_hw_flow(fl(), on, err, err_len); }

StreamBufferHandle_t dongle_claim_rx(void) { return s_claim_rx; }

bool dongle_claim(void) {
    dongle_t *d = fl();
    if (!d->ready || d->claimed) return false;
    xStreamBufferReset(s_claim_rx);
    d->claimed = true;
    if (d->hw_flow) {
        xSemaphoreTake(d->tx_lock, portMAX_DELAY);
        if (d->port.h) us_set_cts_flow(&d->port, false);
        d->hw_flow = false;
        xSemaphoreGive(d->tx_lock);
    }
    return true;
}

esp_err_t dongle_claimed_write(const uint8_t *data, size_t len, uint32_t timeout_ms) {
    dongle_t *d = fl();
    if (!d->claimed) return ESP_ERR_INVALID_STATE;
    return write_raw(d, data, len, timeout_ms);
}

// The flash target's USB chip under its write lock: baud, DTR/RTS or GPIO latch.
typedef enum { FL_BAUD, FL_LINES, FL_GPIO } fl_op_t;
static esp_err_t fl_ctl(fl_op_t op, uint32_t a, uint32_t b) {
    dongle_t *d = fl();
    if (!d->ready) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(d->tx_lock, portMAX_DELAY);
    esp_err_t e = ESP_ERR_INVALID_STATE;
    if (d->port.h) {
        if (op == FL_BAUD) e = us_set_baud(&d->port, a);
        else if (op == FL_LINES) e = us_set_lines(&d->port, a != 0, b != 0);
        else e = us_set_gpio(&d->port, (uint8_t)a, (uint8_t)b);
    }
    xSemaphoreGive(d->tx_lock);
    return e;
}

esp_err_t dongle_set_baud(uint32_t baud) { return fl_ctl(FL_BAUD, baud, 0); }
esp_err_t dongle_set_lines(bool dtr, bool rts) { return fl_ctl(FL_LINES, dtr, rts); }

esp_err_t dongle_wait_reopen(uint32_t timeout_ms) {
    // Flashers call this after a read/write failed because the stick left the bus (ConBee II and nRF52840
    // re-enumerate into and out of their bootloaders). identify() + try_open() bring it back on their own;
    // here we only wait for a fresh attach. If it never actually left (no detach within 1.5 s), it's still ours.
    dongle_t *d = fl();
    stats_lock();
    uint32_t att0 = g_radio[d->idx].attach, det0 = g_radio[d->idx].detach;
    stats_unlock();
    int64_t t0 = esp_timer_get_time(), end = t0 + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < end) {
        stats_lock();
        uint32_t att = g_radio[d->idx].attach, det = g_radio[d->idx].detach;
        stats_unlock();
        if (att != att0 && d->ready && d->port.h) {
            vTaskDelay(pdMS_TO_TICKS(200));  // let a CDC device settle after DTR goes up
            return ESP_OK;
        }
        if (det == det0 && d->ready && d->port.h && esp_timer_get_time() - t0 > 1500000) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return ESP_ERR_TIMEOUT;
}

void dongle_set_app_baud(uint32_t baud) { dx_set_app_baud(fl(), baud); }

esp_err_t dongle_set_gpio(uint8_t mask, uint8_t state) { return fl_ctl(FL_GPIO, mask, state); }

void dongle_release(void) {
    dongle_t *d = fl();
    if (!d->claimed) return;
    // Back to how the radio's client expects it: its baud, DTR and RTS on.
    dongle_set_baud(d->baud);
    dongle_set_lines(true, true);
    if (d->rx) xStreamBufferReset(d->rx);
    d->claimed = false;
    if (d->hw_pref) {
        char err[96];
        if (set_hw_flow(d, true, err, sizeof(err)) != ESP_OK)
            stats_event(EV_WARN, "Hardware flow control left off after the stick update: %s", err);
    }
}

// ---------------------------------------------------------------- start

void dongle_start(bool fs_only) {
    bridge_settings_t cfg;
    settings_get(&cfg);
    s_assign_lock = xSemaphoreCreateMutex();
    s_claim_rx = xStreamBufferCreate(4096, 1);
    s_fs_only = fs_only;
    for (int i = 0; i < RADIO_MAX; i++) {
        dongle_t *d = &s_d[i];
        memset(d, 0, sizeof(*d));
        d->idx = i;
        d->baud = i == 0 ? cfg.baud : cfg.radios[i].baud;
        d->rx = xStreamBufferCreate(RX_STREAM_SIZE, 1);  // PSRAM: above the internal-only threshold
        d->tx_lock = xSemaphoreCreateMutex();
        d->mon_lock = xSemaphoreCreateMutex();
        d->flow_ev = xEventGroupCreate();
        portMUX_INITIALIZE(&d->id_mux);
        portMUX_INITIALIZE(&d->tp_mux);
        xEventGroupSetBits(d->flow_ev, FLOW_XON);
        d->info.model = dongle_match(0, 0, NULL, NULL, false);  // "unsupported" until something enumerates
        // Radios 2 and 3 never hold writes on 0x13: in the Z-Wave Serial API it is FUNC_ID_ZW_SEND_DATA, not XOFF.
        const tp_cfg_t tp_cfg = {.baud = d->baud, .chunk = TP_CHUNK_MAX, .gap_us = 300, .xoff_honor = i == 0, .xoff_max_us = 20000};
        tp_init(&d->tp, &tp_cfg);
    }

    // The root port stays off until the full-speed bit is set: a hub that has chirped once is high speed.
    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .root_port_unpowered = true,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    if (fs_only) force_full_speed();
    portENTER_CRITICAL(&s_usb_mux);
    s_usb.fs_only = fs_only;
    portEXIT_CRITICAL(&s_usb_mux);
    s_task = task_start(dongle_task, "dongle", 6144, NULL, 6, false);  // writes NVS: internal stack
    task_start(line_task, "dongle_line", 3072, NULL, 4, true);       // USB control transfers only

    // The CDC driver registers before the USB library task runs and before the port has power, so it can't
    // miss a stick's enumeration at boot (scan_bus covers anything that slips through anyway).
    const cdc_acm_host_driver_config_t drv_cfg = {
        .driver_task_stack_size = 5120,  // identify() runs here: 2.9 KB measured
        .driver_task_priority = 10,
        .xCoreID = 0,
        .new_dev_cb = on_new_dev,
    };
    ESP_ERROR_CHECK(cdc_acm_host_install(&drv_cfg));
    task_start(usb_lib_task, "usb_lib", 3072, NULL, 10, false);
    ESP_ERROR_CHECK(usb_host_lib_set_root_port_power(true));
    ESP_LOGI(TAG, "USB host ready (%s); waiting for sticks on J3", fs_only ? "full speed only" : "full/high speed");
}

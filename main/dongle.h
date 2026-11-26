#pragma once
// The radio sticks on the P4's USB host port (J3): one plugged straight in, or up to RADIO_MAX behind a hub.
// Each is any USB-serial adapter the bridge can drive (CP210x, CH340/CH341, FTDI, or CDC-ACM class;
// usbser.c), identified against the stick list (dongles.c) and given a radio (radio_core.c): a new stick sets
// a free radio up for itself, and a radio nothing was set up on is freed again when its stick leaves. Bytes a
// stick sends land in its radio's stream buffer, which the bridge drains.
//
// dx_*() take the radio's dongle_t. The older dongle_*() names act on radio 0 (the Zigbee coordinator the
// bridge always had), except the flashing ones (claim .. wait_reopen, dongle_fl_*), which act on the radio
// picked with dongle_fl_select().

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "txpace_core.h"
#include "dongles.h"
#include "radio_core.h"
#include "usbser_core.h"

typedef struct dongle dongle_t;

// What's plugged in.
typedef struct {
    uint16_t vid, pid;
    char mfr[48], product[48], serial[48];
    us_chip_t chip;
    const dongle_model_t *model;  // never NULL (a generic entry when it isn't in the list)
    proto_t proto_detected;       // from the traffic (bridge.c), PROTO_UNKNOWN until it's clear
    char fw_running[80];          // the firmware's own version string when the traffic showed it (Spinel/Z-Stack)
    char fw_version[24];          // just the x.y.z (or Z-Stack build date) part, for comparing
    bool speed_full;
    bool has_cdc;
    uint8_t usb_addr;             // USB address (opens go by address: two sticks may share a VID:PID)
    uint8_t hub_port;             // 0 = straight on J3, n = port n of the hub
} dongle_info_t;

// The CP2102N's own view of the UART, sampled every 10 ms (CTS) / 100 ms (status).
typedef struct {
    bool known, cts_now, hw_flow;   // hw_flow: the CP2102N honours CTS right now
    uint32_t samples, cts_off, cts_drops;  // CTS polls / polls with CTS off / on->off changes
    uint32_t status_polls, hold_cts, hold_xon;  // status polls / with TX held for CTS / waiting for XON
    uint32_t err_framing, err_hw_overrun, err_queue_overrun, err_parity, err_break;  // on bytes from the NCP
    uint32_t in_queue_max, out_queue_max;  // bytes the CP2102N held toward USB / toward the NCP
    uint32_t poll_fail, hw_auto_off;
} dongle_line_t;

typedef struct {
    tp_stats_t st;
    tp_cfg_t cfg;
    bool xoff_now;
    dongle_line_t line;
} dongle_flow_t;

// One USB device as the USB devices card shows it.
typedef struct {
    uint8_t addr, hub_port;
    bool is_hub, high_speed;
    uint16_t vid, pid;
    char product[48];
    int8_t radio;  // radio holding it, -1 none
} dongle_usb_dev_t;
#define DONGLE_USB_MAX 10
typedef struct {
    int n;
    dongle_usb_dev_t dev[DONGLE_USB_MAX];
    bool fs_only;        // J3 refuses high speed (setting usb_fs_only, applied at boot)
    bool fs_forced;      // and the controller bit is set right now
    uint32_t reforces;   // times a hub came up at high speed and the bit was set again
    uint32_t refused;    // sticks that found no free radio
} dongle_usb_t;

// Starts the USB host for every radio. fs_only: make J3 a full-speed-only port (hubs then run at full speed,
// so the full-speed sticks behind them work: the P4 has no transaction translator).
void dongle_start(bool fs_only);
dongle_t *dongle_at(int radio);  // 0 .. RADIO_MAX-1
void dongle_usb_snapshot(dongle_usb_t *out);

void dx_info(dongle_t *d, dongle_info_t *out);
void dx_note_proto(dongle_t *d, proto_t p);
void dx_note_fw(dongle_t *d, const char *running, const char *version);
bool dx_ready(dongle_t *d);
bool dx_identified(dongle_t *d);  // a supported USB serial adapter has been seen for it (open or not)
// Blocking write (up to timeout_ms). ESP_ERR_INVALID_STATE when not connected. Paced: at most 64-byte USB
// transfers with idle line between back-to-back ones, held while the NCP says XOFF (txpace_core.h).
// *sent (may be NULL): the bytes that went to the stick, also when the write timed out partway.
esp_err_t dx_write(dongle_t *d, const uint8_t *data, size_t len, uint32_t timeout_ms, size_t *sent);
StreamBufferHandle_t dx_rx_stream(dongle_t *d);
void dx_flush_rx(dongle_t *d);  // drop anything buffered from before the current client connected
bool dx_claimed(dongle_t *d);
// A client connected to the radio: save it as set up (done by the dongle task, which may write NVS).
void dx_request_set_up(dongle_t *d);
// Why the radio's stick can't be used right now (open or setup failed), "" when fine.
void dx_error(dongle_t *d, char *out, size_t n);
// Free the radio ("Forget" on the dashboard): its saved stick and set-up state go, and the next new stick takes
// it. Refused while its stick is plugged in.
esp_err_t dx_forget(dongle_t *d, char *err, size_t err_len);
// The radio's link baud changed in its settings: switch the open stick now (the client should reconnect).
// ESP_ERR_INVALID_STATE while the stick is being flashed.
esp_err_t dx_set_link_baud(dongle_t *d, uint32_t baud);
// Match every plugged-in stick again (after the downloaded stick list changed).
void dongle_rematch(void);

// ---- radio 0 (the Zigbee coordinator)
void dongle_info(dongle_info_t *out);
bool dongle_ready(void);
bool dongle_identified(void);
// Pacing and software flow control, applied at once (settings tx_chunk, tx_gap_us, xoff_honor, xoff_max_ms).
void dongle_set_flow(uint16_t chunk, uint16_t gap_us, bool xoff_honor, uint16_t xoff_max_ms);
// CTS handshake in the CP2102N (setting hw_flow). Turning it on is refused unless the dongle asserts CTS
// now; it turns itself off if a write stalls, and is off while the flasher has the dongle.
esp_err_t dongle_set_hw_flow(bool on, char *err, size_t err_len);
void dongle_flow_stats(dongle_flow_t *out);
void dongle_flow_reset_stats(void);  // every radio's

// ---- flashing (ncpflash.c, xflash.c, fwcat.c): the radio picked here
// Picks the radio whose stick the firmware pages work on; refused (false) while a flash runs.
bool dongle_fl_select(int radio);
int dongle_fl_index(void);
void dongle_fl_info(dongle_info_t *out);
bool dongle_fl_ready(void);
void dongle_fl_note_fw(const char *running, const char *version);
esp_err_t dongle_fl_set_hw_flow(bool on, char *err, size_t err_len);
// Exclusive use while the stick's own firmware is flashed: the bridge stops talking to it, received bytes
// go to dongle_claim_rx() instead, and the flasher may change the baud and DTR/RTS. dongle_release() puts
// the baud and lines back the way the radio's client expects them.
bool dongle_claim(void);
void dongle_release(void);
// The baud the stick's firmware talks at from now on (after flashing one at another baud): every reopen and
// dongle_release use it.
void dongle_set_app_baud(uint32_t baud);
// For flashers whose stick re-enumerates (ConBee II, nRF52840): waits until it has come back and been
// reopened (it may come back as another USB ID, e.g. a bootloader). ESP_OK, or ESP_ERR_TIMEOUT.
esp_err_t dongle_wait_reopen(uint32_t timeout_ms);
StreamBufferHandle_t dongle_claim_rx(void);
esp_err_t dongle_claimed_write(const uint8_t *data, size_t len, uint32_t timeout_ms);
esp_err_t dongle_set_baud(uint32_t baud);
esp_err_t dongle_set_lines(bool dtr, bool rts);
// CP2102N GPIO latch (SLZB-07 style reset lines); ESP_ERR_NOT_SUPPORTED on other chips.
esp_err_t dongle_set_gpio(uint8_t mask, uint8_t state);

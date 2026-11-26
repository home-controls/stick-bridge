#pragma once
// The Zigbee dongle on the P4's high-speed USB host port (J3): any USB-serial adapter the bridge can
// drive (CP210x, CH340/CH341, FTDI, or CDC-ACM class; usbser.c), identified against the dongle list
// (dongles.c). Bytes it sends land in a stream buffer the bridge drains.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "txpace_core.h"
#include "dongles.h"
#include "usbser_core.h"

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
} dongle_info_t;
void dongle_info(dongle_info_t *out);
void dongle_note_proto(proto_t p);
// Match the plugged-in dongle again (after the downloaded dongle list changed).
void dongle_rematch(void);
void dongle_note_fw(const char *running, const char *version);

void dongle_start(uint32_t baud);
bool dongle_ready(void);
// A supported USB serial adapter has been seen (open or not).
bool dongle_identified(void);
// Blocking write to the dongle (up to timeout_ms). ESP_ERR_INVALID_STATE when not connected.
// Paced: at most 64-byte USB transfers with idle line between back-to-back ones, held while the NCP
// says XOFF (txpace_core.h).
esp_err_t dongle_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

// Pacing and software flow control, applied at once (settings tx_chunk, tx_gap_us, xoff_honor, xoff_max_ms).
void dongle_set_flow(uint16_t chunk, uint16_t gap_us, bool xoff_honor, uint16_t xoff_max_ms);
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

// CTS handshake in the CP2102N (setting hw_flow). Turning it on is refused unless the dongle asserts CTS
// now; it turns itself off if a write stalls, and is off while the flasher has the dongle.
esp_err_t dongle_set_hw_flow(bool on, char *err, size_t err_len);
void dongle_flow_stats(dongle_flow_t *out);
void dongle_flow_reset_stats(void);
// Bytes from the dongle, in order. Drained by the bridge.
StreamBufferHandle_t dongle_rx_stream(void);
// Drop anything buffered from before the current client connected.
void dongle_flush_rx(void);

// Exclusive use while the dongle's own firmware is flashed (ncpflash.c): the bridge stops talking to it,
// received bytes go to dongle_claim_rx() instead, and the flasher may change the baud and DTR/RTS.
// dongle_release() puts the baud and lines back the way Zigbee2MQTT expects them.
bool dongle_claim(void);
bool dongle_claimed(void);
void dongle_release(void);
// The baud the dongle's firmware talks at from now on (after flashing one at another baud): dongle_release
// and every reopen use it.
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
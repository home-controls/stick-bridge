#pragma once
// The dongle's USB-to-serial chip: open it and drive its UART the way a host serial driver would (baud,
// 8N1, DTR/RTS, CTS handshake, modem status). CP210x, CH340/CH341 and FTDI take vendor control requests
// (the same ones Linux sends); everything else that is CDC-ACM class (CH9102F, ESP32-S3 bridges, ConBee II,
// nRF52840) uses the standard class requests. The arithmetic is in usbser_core.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "usb/cdc_acm_host.h"
#include "usbser_core.h"

typedef struct {
    us_chip_t chip;
    cdc_acm_dev_hdl_t h;
    uint8_t ch34x_version;
    volatile uint8_t ftdi_modem;  // latest FTDI modem-status byte (from the IN packets)
    volatile uint8_t ftdi_errs;   // FTDI line errors seen since the last us_get_comm
} us_port_t;

typedef struct {
    bool framing, overrun, queue_overrun, parity, brk;
    bool hold_cts, hold_xon;
    uint32_t in_queue, out_queue;
} us_comm_t;

// Opens the device as the right kind of interface; cfg's callbacks are the caller's. FTDI forces one
// max-packet per IN callback so the status bytes can be stripped (us_rx_fix).
// dev_addr: the USB address to open (CDC_HOST_ANY_DEV_ADDR = first match by VID:PID).
esp_err_t us_open(us_port_t *p, us_chip_t chip, uint16_t vid, uint16_t pid, uint8_t dev_addr,
                  const cdc_acm_host_device_config_t *cfg);
void us_close(us_port_t *p);

// Chip init, then baud, 8N1, no hardware flow control, DTR and RTS asserted: what Linux does when
// Zigbee2MQTT opens the port with rtscts off.
esp_err_t us_setup(us_port_t *p, uint32_t baud);
esp_err_t us_set_baud(us_port_t *p, uint32_t baud);
esp_err_t us_set_lines(us_port_t *p, bool dtr, bool rts);
// CTS handshake (stop sending while the radio deasserts its RTS). ESP_ERR_NOT_SUPPORTED on CDC.
esp_err_t us_set_cts_flow(us_port_t *p, bool on);
// CTS input right now. ESP_ERR_NOT_SUPPORTED where the chip can't tell (CDC).
esp_err_t us_get_cts(us_port_t *p, bool *cts);
// UART errors/queues where the chip reports them (CP210x fully, FTDI errors only).
esp_err_t us_get_comm(us_port_t *p, us_comm_t *c);
// CP2102N GPIO latch (SMLIGHT SLZB-07 reset lines). ESP_ERR_NOT_SUPPORTED elsewhere.
esp_err_t us_set_gpio(us_port_t *p, uint8_t mask, uint8_t state);
// Received data as the chip delivered it -> the serial bytes (strips FTDI status). Returns the new length.
size_t us_rx_fix(us_port_t *p, uint8_t *data, size_t len);

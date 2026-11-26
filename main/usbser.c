#include "usbser.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "usbser";

static esp_err_t req(us_port_t *p, uint8_t type, uint8_t r, uint16_t v, uint16_t idx, uint16_t len, uint8_t *data) {
    if (!p->h) return ESP_ERR_INVALID_STATE;
    return cdc_acm_host_send_custom_request(p->h, type, r, v, idx, len, data);
}

// ---------------------------------------------------------------- CP210x (AN571; requests to interface 0)
#define CP_OUT 0x41
#define CP_IN 0xC1
#define CP_IFC_ENABLE 0x00
#define CP_SET_LINE_CTL 0x03
#define CP_SET_MHS 0x07
#define CP_GET_MDMSTS 0x08
#define CP_GET_COMM_STATUS 0x10
#define CP_SET_FLOW 0x13
#define CP_SET_BAUDRATE 0x1E
#define CP_VENDOR 0xFF
#define CP_WRITE_LATCH 0x37E1

static esp_err_t cp_flow(us_port_t *p, bool cts) {
    // DTR active, CTS handshake on request; RTS statically active; no in-chip XON/XOFF.
    uint8_t f[16] = {0};
    uint32_t ctl = 0x01 | (cts ? 0x08 : 0), repl = 0x40, xon = 128, xoff = 128;
    memcpy(f, &ctl, 4);
    memcpy(f + 4, &repl, 4);
    memcpy(f + 8, &xon, 4);
    memcpy(f + 12, &xoff, 4);
    return req(p, CP_OUT, CP_SET_FLOW, 0, 0, sizeof(f), f);
}

static esp_err_t cp_baud(us_port_t *p, uint32_t baud) {
    uint8_t b[4];
    memcpy(b, &baud, 4);
    return req(p, CP_OUT, CP_SET_BAUDRATE, 0, 0, 4, b);
}

// ---------------------------------------------------------------- CH340/CH341 (Linux ch341.c; device requests)
#define CH_OUT 0x40
#define CH_IN 0xC0
#define CH_READ_VERSION 0x5F
#define CH_READ_REG 0x95
#define CH_WRITE_REG 0x9A
#define CH_SERIAL_INIT 0xA1
#define CH_MODEM_CTRL 0xA4
#define CH_DTR 0x20
#define CH_RTS 0x40

static esp_err_t ch_baud(us_port_t *p, uint32_t baud) {
    int w = us_ch34x_divisor(baud, p->ch34x_version);
    if (w < 0) return ESP_ERR_INVALID_ARG;
    esp_err_t e = req(p, CH_OUT, CH_WRITE_REG, 0x1312, (uint16_t)w, 0, NULL);
    if (e != ESP_OK || p->ch34x_version < 0x30) return e;
    return req(p, CH_OUT, CH_WRITE_REG, 0x2518, 0xC3, 0, NULL);  // LCR: RX+TX on, 8 bits, no parity, 1 stop
}

// ---------------------------------------------------------------- FTDI (Linux ftdi_sio.c; device requests, port 0)
#define FT_OUT 0x40
#define FT_IN 0xC0
#define FT_RESET 0x00
#define FT_MODEM_CTRL 0x01
#define FT_SET_FLOW 0x02
#define FT_SET_BAUD 0x03
#define FT_SET_DATA 0x04
#define FT_LATENCY 0x09

static esp_err_t ft_baud(us_port_t *p, uint32_t baud) {
    uint32_t d = us_ftdi_divisor(baud);
    return req(p, FT_OUT, FT_SET_BAUD, (uint16_t)d, (uint16_t)(d >> 16), 0, NULL);
}

// ---------------------------------------------------------------- common

esp_err_t us_open(us_port_t *p, us_chip_t chip, uint16_t vid, uint16_t pid, const cdc_acm_host_device_config_t *cfg) {
    memset(p, 0, sizeof(*p));
    p->chip = chip;
    cdc_acm_host_device_config_t c = *cfg;
    esp_err_t e;
    switch (chip) {
        case US_CP210X:
        case US_CH34X:
            e = cdc_acm_host_open_vendor_specific(vid, pid, 0, &c, &p->h);
            break;
        case US_FTDI:
            c.in_buffer_size = 64;  // one packet per callback: each starts with 2 status bytes
            e = cdc_acm_host_open_vendor_specific(vid, pid, 0, &c, &p->h);
            break;
        case US_CDC:
            e = cdc_acm_host_open(vid, pid, 0, &c, &p->h);
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
    if (e != ESP_OK) p->h = NULL;
    return e;
}

void us_close(us_port_t *p) {
    if (p->h) cdc_acm_host_close(p->h);
    p->h = NULL;
}

esp_err_t us_set_baud(us_port_t *p, uint32_t baud) {
    switch (p->chip) {
        case US_CP210X: return cp_baud(p, baud);
        case US_CH34X: return ch_baud(p, baud);
        case US_FTDI: return ft_baud(p, baud);
        case US_CDC: {
            cdc_acm_line_coding_t lc = {.dwDTERate = baud, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8};
            return cdc_acm_host_line_coding_set(p->h, &lc);
        }
        default: return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t us_set_lines(us_port_t *p, bool dtr, bool rts) {
    switch (p->chip) {
        case US_CP210X: return req(p, CP_OUT, CP_SET_MHS, (uint16_t)(0x0300 | (dtr ? 1 : 0) | (rts ? 2 : 0)), 0, 0, NULL);
        case US_CH34X: {
            uint8_t bits = (dtr ? CH_DTR : 0) | (rts ? CH_RTS : 0);
            return req(p, CH_OUT, CH_MODEM_CTRL, (uint16_t)~bits, 0, 0, NULL);  // active low
        }
        case US_FTDI: return req(p, FT_OUT, FT_MODEM_CTRL, (uint16_t)(0x0300 | (dtr ? 1 : 0) | (rts ? 2 : 0)), 0, 0, NULL);
        case US_CDC: return cdc_acm_host_set_control_line_state(p->h, dtr, rts);
        default: return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t us_set_cts_flow(us_port_t *p, bool on) {
    switch (p->chip) {
        case US_CP210X: return cp_flow(p, on);
        case US_CH34X: return req(p, CH_OUT, CH_WRITE_REG, 0x2727, on ? 0x0101 : 0, 0, NULL);
        case US_FTDI: return req(p, FT_OUT, FT_SET_FLOW, 0, on ? 0x0100 : 0, 0, NULL);
        default: return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t us_setup(us_port_t *p, uint32_t baud) {
    esp_err_t e;
    switch (p->chip) {
        case US_CP210X:
            if ((e = req(p, CP_OUT, CP_IFC_ENABLE, 0x0001, 0, 0, NULL)) != ESP_OK) return e;
            if ((e = req(p, CP_OUT, CP_SET_LINE_CTL, 0x0800, 0, 0, NULL)) != ESP_OK) return e;  // 8N1
            if ((e = cp_flow(p, false)) != ESP_OK) return e;
            break;
        case US_CH34X: {
            uint8_t v[2] = {0};
            if (req(p, CH_IN, CH_READ_VERSION, 0, 0, 2, v) == ESP_OK) p->ch34x_version = v[0];
            if ((e = req(p, CH_OUT, CH_SERIAL_INIT, 0, 0, 0, NULL)) != ESP_OK) return e;
            ESP_LOGI(TAG, "CH34x chip version 0x%02x", p->ch34x_version);
            break;
        }
        case US_FTDI:
            if ((e = req(p, FT_OUT, FT_RESET, 0, 0, 0, NULL)) != ESP_OK) return e;
            if ((e = req(p, FT_OUT, FT_SET_DATA, 0x0008, 0, 0, NULL)) != ESP_OK) return e;  // 8N1
            if ((e = req(p, FT_OUT, FT_SET_FLOW, 0, 0, 0, NULL)) != ESP_OK) return e;
            // 1 ms: the default 16 ms would hold every reply from the radio for up to 16 ms
            if ((e = req(p, FT_OUT, FT_LATENCY, 1, 0, 0, NULL)) != ESP_OK) return e;
            break;
        case US_CDC:
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
    if ((e = us_set_baud(p, baud)) != ESP_OK) return e;
    e = us_set_lines(p, true, true);
    // Some CDC devices (e.g. firmware that ignores line state) don't implement it: not fatal there.
    if (e == ESP_ERR_NOT_SUPPORTED && p->chip == US_CDC) e = ESP_OK;
    return e;
}

esp_err_t us_get_cts(us_port_t *p, bool *cts) {
    switch (p->chip) {
        case US_CP210X: {
            uint8_t m = 0;
            esp_err_t e = req(p, CP_IN, CP_GET_MDMSTS, 0, 0, 1, &m);
            if (e == ESP_OK) *cts = (m & 0x10) != 0;
            return e;
        }
        case US_CH34X: {
            uint8_t b[2] = {0};
            esp_err_t e = req(p, CH_IN, CH_READ_REG, 0x0706, 0, 2, b);
            if (e == ESP_OK) *cts = ((uint8_t)~b[0] & 0x01) != 0;
            return e;
        }
        case US_FTDI:
            *cts = (p->ftdi_modem & US_FTDI_CTS) != 0;
            return ESP_OK;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t us_get_comm(us_port_t *p, us_comm_t *c) {
    memset(c, 0, sizeof(*c));
    if (p->chip == US_CP210X) {
        uint8_t cs[19] = {0};
        esp_err_t e = req(p, CP_IN, CP_GET_COMM_STATUS, 0, 0, sizeof(cs), cs);
        if (e != ESP_OK) return e;
        uint32_t errs, hold;
        memcpy(&errs, cs, 4);
        memcpy(&hold, cs + 4, 4);
        memcpy(&c->in_queue, cs + 8, 4);
        memcpy(&c->out_queue, cs + 12, 4);
        c->brk = errs & 0x01;
        c->framing = errs & 0x02;
        c->overrun = errs & 0x04;
        c->queue_overrun = errs & 0x08;
        c->parity = errs & 0x10;
        c->hold_cts = hold & 0x01;
        c->hold_xon = hold & 0x08;
        return ESP_OK;
    }
    if (p->chip == US_FTDI) {
        uint8_t e = p->ftdi_errs;
        p->ftdi_errs = 0;
        c->overrun = e & US_FTDI_LINE_OVERRUN;
        c->parity = e & US_FTDI_LINE_PARITY;
        c->framing = e & US_FTDI_LINE_FRAMING;
        c->brk = e & US_FTDI_LINE_BREAK;
        return ESP_OK;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t us_set_gpio(us_port_t *p, uint8_t mask, uint8_t state) {
    if (p->chip != US_CP210X) return ESP_ERR_NOT_SUPPORTED;
    // Linux cp210x_gpio_set for CP2102N: vendor request to the device, wIndex = state << 8 | mask
    return req(p, 0x40, CP_VENDOR, CP_WRITE_LATCH, (uint16_t)(state << 8 | mask), 0, NULL);
}

size_t us_rx_fix(us_port_t *p, uint8_t *data, size_t len) {
    if (p->chip != US_FTDI) return len;
    uint8_t modem = p->ftdi_modem, errs = 0;
    size_t n = us_ftdi_strip(data, len, 64, &modem, &errs);
    p->ftdi_modem = modem;
    p->ftdi_errs |= errs;
    return n;
}

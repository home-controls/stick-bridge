#pragma once
// Baud rate to divisor values for CH340/CH341 and FTDI USB serial chips, written from docs/usb-serial-baud.md.
// Portable: no ESP-IDF here, so test/host/test_serial_baud.py checks this very code on the host.

#include <stdint.h>

// CH340/CH341: the 16-bit word for register pair 0x13/0x12 (high byte 256 - N, low byte PS | FACT << 2,
// bit 7 when chip_version > 0x27), or -1 for baud 0. baud is clamped to 46 ... 3,000,000.
int sb_ch34x_word(uint32_t baud, uint8_t chip_version);

// The rate a CH34x word really gives: 48,000,000 / (P * N), rounded down. Only the low 16 bits count;
// 0 for a negative word or a high byte of 0.
uint32_t sb_ch34x_rate(int word);

// FTDI "BM" type: the 17-bit SET_BAUD_RATE value (bits 0-15 to wValue, bit 16 to wIndex bit 0).
uint32_t sb_ftdi_value(uint32_t baud);

// The rate an FTDI value really gives, rounded to the nearest integer.
uint32_t sb_ftdi_rate(uint32_t value);

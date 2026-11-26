#pragma once
// Writing Zigbee2MQTT's bytes to the dongle without overrunning the NCP's UART.
// Portable: no ESP-IDF here, so test/host/test_txpace.py drives this very code on a virtual clock.
//
// The CP2102N takes a whole USB OUT transfer into its own FIFO at once and shifts it out back to back,
// so pacing only shows up on the UART when it is measured against the UART itself: a chunk may start
// once the previous one has had time to leave the wire (bytes * 10 bits / baud, from when the USB
// transfer completed), plus gap_us of idle line. A lone frame on a quiet line goes out at once.
//
// Software flow control: ASH byte-stuffs 0x11 and 0x13 inside frames, so a raw XON/XOFF byte from the
// NCP is always flow control. With at most one chunk queued in the CP2102N, holding the next chunk on
// XOFF actually stops the line within one chunk. A hold ends on XON, or after xoff_max_us without one
// (each new XOFF re-arms it), or when the write's own timeout runs out.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TP_ASH_FLAG 0x7E
#define TP_XON 0x11
#define TP_XOFF 0x13
#define TP_CHUNK_MAX 64  // USB full-speed bulk packet

typedef struct {
    uint32_t baud;         // UART bit rate; 8N1 = 10 bits per byte
    uint16_t chunk;        // bytes per USB OUT transfer, 8..64; 0 = whole write at once, no pacing
    uint32_t gap_us;       // idle UART time between back-to-back chunks
    bool xoff_honor;       // hold writes on XOFF (XON/XOFF are counted either way)
    uint32_t xoff_max_us;  // longest hold on one XOFF
} tp_cfg_t;

typedef struct {
    uint32_t xon_rx, xoff_rx;     // raw XON / XOFF bytes from the NCP
    uint32_t pauses;              // chunks held back by XOFF
    uint32_t pause_timeouts;      // holds that ended without an XON
    uint64_t pause_us_sum;
    uint32_t pause_max_us;
    uint32_t writes, chunks;      // tp_write calls / USB OUT transfers
    uint32_t paced;               // chunks that waited for the line
    uint64_t pace_us_sum;
    uint32_t pace_max_us;
    uint32_t write_max_us;        // longest tp_write call, pacing and holds included
    uint32_t write_errors;
    uint32_t queue_hwm;           // most bytes handed over in one write: what Z2M had queued when read
} tp_stats_t;

typedef struct {
    tp_cfg_t cfg;
    int64_t wire_free_at;  // when the UART should have shifted out everything written so far
    bool xoff;             // the NCP said XOFF and no XON since
    int64_t xoff_at;       // time of the latest XOFF
    tp_stats_t st;
} tp_state_t;

typedef struct {
    void *ctx;
    // One USB OUT transfer; returns when the CP2102N has taken it. 0 ok, <0 error, 1 timeout.
    int (*write)(void *ctx, const uint8_t *buf, size_t len, uint32_t timeout_ms);
    int64_t (*now_us)(void *ctx);
    void (*wait_until)(void *ctx, int64_t t_us);
    // Block until an XON may have arrived or until deadline_us (returning early is fine).
    void (*wait_xon)(void *ctx, int64_t deadline_us);
    // Guard the XON/XOFF state against tp_rx running in another task (may be NULL).
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} tp_io_t;

#define TP_OK 0
#define TP_ERR_WRITE -1
#define TP_ERR_TIMEOUT -2

void tp_init(tp_state_t *s, const tp_cfg_t *cfg);
// Line time of n bytes at the configured baud, rounded up.
uint32_t tp_wire_us(const tp_cfg_t *cfg, size_t n);
// Length of the next chunk: up to cfg->chunk bytes, cut just after the last ASH flag inside when the
// data runs longer, so gaps fall between frames.
size_t tp_next_len(const tp_cfg_t *cfg, const uint8_t *data, size_t len);
// Bytes from the NCP (call with the lock held). Returns true when the XON/XOFF state changed.
bool tp_rx(tp_state_t *s, const uint8_t *data, size_t len, int64_t now_us);
// Writes len bytes in paced chunks, holding on XOFF. TP_OK, TP_ERR_WRITE or TP_ERR_TIMEOUT.
int tp_write(tp_state_t *s, const tp_io_t *io, const uint8_t *data, size_t len, uint32_t timeout_ms);
// Forget line state (dongle reopened, new client): no stale XOFF, no pacing debt.
void tp_reset_line(tp_state_t *s);

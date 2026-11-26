// Runs the firmware's own txpace_core.c on a virtual clock (test_txpace.py builds it with ASan/UBSan).
// The simulated USB transfer takes usb_us; XON/XOFF bytes from the "NCP" are scheduled at set times and
// delivered through tp_rx whenever the clock passes them, as the USB task would.
//
//   txpace_harness selftest

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "txpace_core.h"

#define MAX_EV 32
#define MAX_TX 256

typedef struct {
    int64_t t;
    uint8_t b;
} ev_t;

typedef struct {
    int64_t now;
    int64_t usb_us;  // how long one USB OUT transfer takes to complete
    ev_t ev[MAX_EV];
    int nev, next_ev;
    tp_state_t *s;
    // what reached the CP2102N
    int ntx;
    int64_t tx_start[MAX_TX], tx_done[MAX_TX];
    size_t tx_len[MAX_TX];
    uint8_t tx_bytes[4096];
    size_t tx_total;
    int fail_at;  // transfer index that fails (-1 none)
} sim_t;

static int g_fail;
#define CHECK(c, ...)                                         \
    do {                                                      \
        if (!(c)) {                                           \
            printf("FAIL %s:%d: ", __func__, __LINE__);       \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
            g_fail++;                                         \
        }                                                     \
    } while (0)

static void deliver(sim_t *m) {
    while (m->next_ev < m->nev && m->ev[m->next_ev].t <= m->now) {
        tp_rx(m->s, &m->ev[m->next_ev].b, 1, m->ev[m->next_ev].t);
        m->next_ev++;
    }
}

static void advance(sim_t *m, int64_t t) {
    if (t > m->now) m->now = t;
    deliver(m);
}

static int io_write(void *ctx, const uint8_t *buf, size_t len, uint32_t timeout_ms) {
    (void)timeout_ms;
    sim_t *m = ctx;
    if (m->ntx == m->fail_at) return -1;
    if (m->ntx < MAX_TX) {
        m->tx_start[m->ntx] = m->now;
        m->tx_len[m->ntx] = len;
    }
    memcpy(m->tx_bytes + m->tx_total, buf, len);
    m->tx_total += len;
    advance(m, m->now + m->usb_us);
    if (m->ntx < MAX_TX) m->tx_done[m->ntx] = m->now;
    m->ntx++;
    return 0;
}
static int64_t io_now(void *ctx) { return ((sim_t *)ctx)->now; }
static void io_wait_until(void *ctx, int64_t t) { advance(ctx, t); }
static void io_wait_xon(void *ctx, int64_t deadline) {
    sim_t *m = ctx;
    // Wake at the next scheduled byte if it comes before the deadline (like an event-group bit).
    int64_t t = deadline;
    if (m->next_ev < m->nev && m->ev[m->next_ev].t < t) t = m->ev[m->next_ev].t;
    advance(m, t);
}

static tp_io_t io_for(sim_t *m) {
    tp_io_t io = {.ctx = m, .write = io_write, .now_us = io_now, .wait_until = io_wait_until, .wait_xon = io_wait_xon};
    return io;
}

static void sim_init(sim_t *m, tp_state_t *s, int64_t usb_us) {
    memset(m, 0, sizeof(*m));
    m->now = 1000000;  // not 0: 0 is "no line state"
    m->usb_us = usb_us;
    m->s = s;
    m->fail_at = -1;
}

static void schedule(sim_t *m, int64_t t, uint8_t b) {
    m->ev[m->nev].t = t;
    m->ev[m->nev].b = b;
    m->nev++;
}

static const tp_cfg_t CFG460 = {.baud = 460800, .chunk = 64, .gap_us = 300, .xoff_honor = true, .xoff_max_us = 20000};

// Frames of the given lengths (flag included), back to back, no flag bytes inside.
static size_t frames(uint8_t *out, const int *lens, int n) {
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < lens[i] - 1; k++) out[o++] = (uint8_t)(0x20 + (k % 0x50));
        out[o++] = TP_ASH_FLAG;
    }
    return o;
}

static void t_wire_math(void) {
    CHECK(tp_wire_us(&CFG460, 64) == 1389, "64 B at 460800 = %u us", tp_wire_us(&CFG460, 64));
    tp_cfg_t c = CFG460;
    c.baud = 115200;
    CHECK(tp_wire_us(&c, 64) == 5556, "64 B at 115200 = %u us", tp_wire_us(&c, 64));
    CHECK(tp_wire_us(&c, 0) == 0, "0 bytes");
}

static void t_next_len(void) {
    uint8_t d[300];
    int lens[] = {5, 40, 100, 9};
    size_t n = frames(d, lens, 4);
    CHECK(tp_next_len(&CFG460, d, 10) == 10, "short write stays whole");
    CHECK(tp_next_len(&CFG460, d, n) == 45, "cut after the 2nd frame's flag, got %zu", tp_next_len(&CFG460, d, n));
    CHECK(tp_next_len(&CFG460, d + 45, n - 45) == 64, "no flag inside: full chunk");
    uint8_t f[100];
    memset(f, 0x41, sizeof(f));
    f[1] = TP_ASH_FLAG;  // too early to cut on
    CHECK(tp_next_len(&CFG460, f, 100) == 64, "flag at index 1 ignored");
    tp_cfg_t off = CFG460;
    off.chunk = 0;
    CHECK(tp_next_len(&off, d, n) == n, "chunk 0 = whole write");
}

static void t_lone_frame_no_delay(void) {
    tp_state_t s;
    sim_t m;
    tp_init(&s, &CFG460);
    sim_init(&m, &s, 200);
    tp_io_t io = io_for(&m);
    uint8_t d[64];
    int lens[] = {12};
    size_t n = frames(d, lens, 1);
    int64_t t0 = m.now;
    CHECK(tp_write(&s, &io, d, n, 1000) == TP_OK, "write ok");
    CHECK(m.ntx == 1 && m.tx_start[0] == t0, "went out at once");
    CHECK(s.st.paced == 0, "no pacing on an idle line");
    // Next frame 50 ms later: line long idle, again no wait.
    advance(&m, m.now + 50000);
    t0 = m.now;
    CHECK(tp_write(&s, &io, d, n, 1000) == TP_OK, "write ok");
    CHECK(m.tx_start[1] == t0 && s.st.paced == 0, "second lone frame not delayed");
}

static void t_burst_paced(void) {
    tp_state_t s;
    sim_t m;
    tp_init(&s, &CFG460);
    sim_init(&m, &s, 200);
    tp_io_t io = io_for(&m);
    uint8_t d[400];
    int lens[] = {5, 40, 100, 9, 120};
    size_t n = frames(d, lens, 5);
    int64_t t0 = m.now;
    CHECK(tp_write(&s, &io, d, n, 1000) == TP_OK, "write ok");
    CHECK(m.tx_total == n && memcmp(m.tx_bytes, d, n) == 0, "every byte, in order");
    CHECK(s.st.queue_hwm == n, "queue high-water = burst size (%u)", s.st.queue_hwm);
    for (int i = 0; i < m.ntx; i++) CHECK(m.tx_len[i] <= 64, "chunk %d is %zu bytes", i, m.tx_len[i]);
    for (int i = 1; i < m.ntx; i++) {
        int64_t prev_end = m.tx_done[i - 1] + tp_wire_us(&CFG460, m.tx_len[i - 1]);
        CHECK(m.tx_start[i] >= prev_end + 300, "chunk %d starts %lld us after the line is free (want >= 300)", i,
              (long long)(m.tx_start[i] - prev_end));
    }
    CHECK(s.st.paced == (uint32_t)m.ntx - 1, "every chunk after the first paced (%u of %d)", s.st.paced, m.ntx);
    int64_t took = m.now - t0;
    int64_t wire = tp_wire_us(&CFG460, n);
    // Line time + gaps + USB time per chunk: 274 B at 460800 is ~6 ms; must stay far under 20 ms.
    CHECK(took < wire + m.ntx * (300 + 200) + 1000 && took < 20000, "burst took %lld us (wire %lld)",
          (long long)took, (long long)wire);
}

static void t_back_to_back_writes(void) {
    // Two writes, the second arriving while the first is still on the wire: it waits for the line.
    tp_state_t s;
    sim_t m;
    tp_init(&s, &CFG460);
    sim_init(&m, &s, 100);
    tp_io_t io = io_for(&m);
    uint8_t d[64];
    int lens[] = {60};
    size_t n = frames(d, lens, 1);
    tp_write(&s, &io, d, n, 1000);
    advance(&m, m.now + 100);
    tp_write(&s, &io, d, n, 1000);
    int64_t free_at = m.tx_done[0] + tp_wire_us(&CFG460, 60);
    CHECK(m.tx_start[1] >= free_at + 300, "second write waited for the line (%lld us early)",
          (long long)(free_at + 300 - m.tx_start[1]));
}

static void t_xoff_then_xon(void) {
    tp_state_t s;
    sim_t m;
    tp_init(&s, &CFG460);
    sim_init(&m, &s, 200);
    tp_io_t io = io_for(&m);
    uint8_t d[400];
    int lens[] = {60, 60, 60, 60};
    size_t n = frames(d, lens, 4);
    int64_t t0 = m.now;
    schedule(&m, t0 + 300, TP_XOFF);   // lands while the first chunk is on the wire
    schedule(&m, t0 + 5300, TP_XON);
    CHECK(tp_write(&s, &io, d, n, 1000) == TP_OK, "write ok");
    CHECK(m.ntx == 4, "4 chunks (%d)", m.ntx);
    CHECK(m.tx_start[1] >= t0 + 5300, "2nd chunk held until XON (%lld)", (long long)(m.tx_start[1] - t0));
    CHECK(s.st.pauses == 1 && s.st.pause_timeouts == 0, "one hold, ended by XON (%u, %u)", s.st.pauses,
          s.st.pause_timeouts);
    CHECK(s.st.xoff_rx == 1 && s.st.xon_rx == 1, "counted XOFF/XON");
    CHECK(s.st.pause_max_us > 0 && s.st.pause_max_us <= 5300, "hold time %u", s.st.pause_max_us);
    CHECK(m.tx_total == n && memcmp(m.tx_bytes, d, n) == 0, "nothing lost or reordered");
}

static void t_xoff_lost_xon(void) {
    tp_state_t s;
    sim_t m;
    tp_init(&s, &CFG460);
    sim_init(&m, &s, 200);
    tp_io_t io = io_for(&m);
    uint8_t d[200];
    int lens[] = {60, 60};
    size_t n = frames(d, lens, 2);
    int64_t t0 = m.now;
    schedule(&m, t0 + 100, TP_XOFF);  // and never an XON
    CHECK(tp_write(&s, &io, d, n, 1000) == TP_OK, "write ok after the hold gives up");
    CHECK(s.st.pause_timeouts == 1, "hold timed out");
    CHECK(m.tx_start[1] >= t0 + 100 + 20000 && m.tx_start[1] < t0 + 100 + 21000, "resumed at xoff_max (%lld)",
          (long long)(m.tx_start[1] - t0));
    CHECK(!s.xoff, "state cleared");
}

static void t_xoff_repeated_until_deadline(void) {
    tp_state_t s;
    sim_t m;
    tp_cfg_t c = CFG460;
    c.xoff_max_us = 5000;
    tp_init(&s, &c);
    sim_init(&m, &s, 200);
    tp_io_t io = io_for(&m);
    uint8_t d[200];
    int lens[] = {60, 60};
    size_t n = frames(d, lens, 2);
    int64_t t0 = m.now;
    for (int i = 0; i < 30; i++) schedule(&m, t0 + 100 + i * 4000, TP_XOFF);  // keeps re-arming
    CHECK(tp_write(&s, &io, d, n, 50) == TP_ERR_TIMEOUT, "gives up at the write timeout");
    CHECK(s.last_sent < n, "says how much went before it gave up (%u of %u)", (unsigned)s.last_sent, (unsigned)n);
    CHECK(m.now - t0 <= 51000, "within the 50 ms timeout (%lld)", (long long)(m.now - t0));
    CHECK(s.st.write_errors == 1, "counted");
}

static void t_xoff_not_honored(void) {
    tp_state_t s;
    sim_t m;
    tp_cfg_t c = CFG460;
    c.xoff_honor = false;
    tp_init(&s, &c);
    sim_init(&m, &s, 200);
    tp_io_t io = io_for(&m);
    uint8_t d[200];
    int lens[] = {60, 60};
    size_t n = frames(d, lens, 2);
    schedule(&m, m.now + 100, TP_XOFF);
    tp_write(&s, &io, d, n, 1000);
    CHECK(s.st.pauses == 0 && s.st.xoff_rx == 1, "counted but not held");
}

static void t_unpaced_mode(void) {
    tp_state_t s;
    sim_t m;
    tp_cfg_t c = CFG460;
    c.chunk = 0;
    tp_init(&s, &c);
    sim_init(&m, &s, 200);
    tp_io_t io = io_for(&m);
    uint8_t d[400];
    int lens[] = {100, 100, 100};
    size_t n = frames(d, lens, 3);
    tp_write(&s, &io, d, n, 1000);
    CHECK(m.ntx == 1 && m.tx_len[0] == n, "one transfer, as before (%d)", m.ntx);
}

static void t_write_error(void) {
    tp_state_t s;
    sim_t m;
    tp_init(&s, &CFG460);
    sim_init(&m, &s, 200);
    m.fail_at = 1;
    tp_io_t io = io_for(&m);
    uint8_t d[200];
    int lens[] = {60, 60};
    size_t n = frames(d, lens, 2);
    CHECK(tp_write(&s, &io, d, n, 1000) == TP_ERR_WRITE, "error passed up");
    CHECK(s.st.write_errors == 1 && s.st.chunks == 1, "counted");
}

static void t_rx_scan(void) {
    tp_state_t s;
    tp_init(&s, &CFG460);
    uint8_t a[] = {0x7E, 0x13, 0x41, 0x11, 0x42, 0x13};
    CHECK(tp_rx(&s, a, sizeof(a), 5) == true && s.xoff && s.xoff_at == 5, "last byte wins: XOFF");
    CHECK(s.st.xoff_rx == 2 && s.st.xon_rx == 1, "counts");
    uint8_t b[] = {0x7D, 0x31, 0x11};  // escaped 0x11 is 0x7D 0x31: only the raw one counts
    CHECK(tp_rx(&s, b, sizeof(b), 6) == true && !s.xoff && s.st.xon_rx == 2, "XON");
    tp_reset_line(&s);
    CHECK(!s.xoff && s.wire_free_at == 0, "reset");
}

int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "selftest") != 0) {
        fprintf(stderr, "usage: txpace_harness selftest\n");
        return 2;
    }
    t_wire_math();
    t_next_len();
    t_lone_frame_no_delay();
    t_burst_paced();
    t_back_to_back_writes();
    t_xoff_then_xon();
    t_xoff_lost_xon();
    t_xoff_repeated_until_deadline();
    t_xoff_not_honored();
    t_unpaced_mode();
    t_write_error();
    t_rx_scan();
    printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "all passed", g_fail);
    return g_fail ? 1 : 0;
}

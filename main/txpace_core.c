#include "txpace_core.h"

#include <string.h>

void tp_init(tp_state_t *s, const tp_cfg_t *cfg) {
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
}

void tp_reset_line(tp_state_t *s) {
    s->wire_free_at = 0;
    s->xoff = false;
    s->xoff_at = 0;
}

uint32_t tp_wire_us(const tp_cfg_t *cfg, size_t n) {
    if (cfg->baud == 0) return 0;
    uint64_t bits = (uint64_t)n * 10u * 1000000u;
    return (uint32_t)((bits + cfg->baud - 1) / cfg->baud);
}

size_t tp_next_len(const tp_cfg_t *cfg, const uint8_t *data, size_t len) {
    size_t chunk = cfg->chunk;
    if (chunk == 0 || len <= chunk) return len;
    // Smallest ASH frame is control + CRC + flag = 4 bytes: a flag earlier than that ends a frame
    // that started in an earlier chunk, so cutting there would only send a sliver.
    for (size_t i = chunk; i-- > 3;) {
        if (data[i] == TP_ASH_FLAG) return i + 1;
    }
    return chunk;
}

bool tp_rx(tp_state_t *s, const uint8_t *data, size_t len, int64_t now_us) {
    bool was = s->xoff;
    for (size_t i = 0; i < len; i++) {
        if (data[i] == TP_XOFF) {
            s->st.xoff_rx++;
            s->xoff = true;
            s->xoff_at = now_us;
        } else if (data[i] == TP_XON) {
            s->st.xon_rx++;
            s->xoff = false;
        }
    }
    return s->xoff != was;
}

static void lock(const tp_io_t *io) {
    if (io->lock) io->lock(io->ctx);
}
static void unlock(const tp_io_t *io) {
    if (io->unlock) io->unlock(io->ctx);
}

// Holds while the NCP has said XOFF. false when the write's deadline ran out first.
static bool hold_for_xon(tp_state_t *s, const tp_io_t *io, int64_t deadline) {
    lock(io);
    bool held = s->xoff;
    int64_t since = s->xoff_at;
    unlock(io);
    if (!held) return true;

    int64_t t0 = io->now_us(io->ctx);
    bool ok = true;
    s->st.pauses++;
    for (;;) {
        int64_t limit = since + s->cfg.xoff_max_us;
        int64_t now = io->now_us(io->ctx);
        if (now >= limit) {
            // No XON in time: carry on rather than stall the link on a lost byte.
            s->st.pause_timeouts++;
            lock(io);
            if (s->xoff_at == since) s->xoff = false;
            unlock(io);
            break;
        }
        if (now >= deadline) {
            ok = false;
            break;
        }
        io->wait_xon(io->ctx, limit < deadline ? limit : deadline);
        lock(io);
        held = s->xoff;
        since = s->xoff_at;
        unlock(io);
        if (!held) break;
    }
    uint32_t dt = (uint32_t)(io->now_us(io->ctx) - t0);
    s->st.pause_us_sum += dt;
    if (dt > s->st.pause_max_us) s->st.pause_max_us = dt;
    return ok;
}

int tp_write(tp_state_t *s, const tp_io_t *io, const uint8_t *data, size_t len, uint32_t timeout_ms) {
    int64_t start = io->now_us(io->ctx);
    int64_t deadline = start + (int64_t)timeout_ms * 1000;
    int rc = TP_OK;
    size_t off = 0;
    s->st.writes++;
    if (len > s->st.queue_hwm) s->st.queue_hwm = (uint32_t)len;

    while (off < len) {
        size_t n = tp_next_len(&s->cfg, data + off, len - off);
        if (s->cfg.chunk != 0) {
            // Wait for the line: the previous chunk off the wire, then gap_us of idle.
            int64_t ready = s->wire_free_at + s->cfg.gap_us;
            int64_t now = io->now_us(io->ctx);
            if (s->wire_free_at != 0 && now < ready) {
                if (ready > deadline) {
                    rc = TP_ERR_TIMEOUT;
                    break;
                }
                io->wait_until(io->ctx, ready);
                uint32_t w = (uint32_t)(io->now_us(io->ctx) - now);
                s->st.paced++;
                s->st.pace_us_sum += w;
                if (w > s->st.pace_max_us) s->st.pace_max_us = w;
            }
        }
        // Checked after the line wait, so an XOFF that came in meanwhile still holds this chunk.
        if (s->cfg.xoff_honor && !hold_for_xon(s, io, deadline)) {
            rc = TP_ERR_TIMEOUT;
            break;
        }
        int64_t now = io->now_us(io->ctx);
        if (now >= deadline) {
            rc = TP_ERR_TIMEOUT;
            break;
        }
        int r = io->write(io->ctx, data + off, n, (uint32_t)((deadline - now + 999) / 1000));
        if (r != 0) {
            rc = r > 0 ? TP_ERR_TIMEOUT : TP_ERR_WRITE;
            break;
        }
        s->st.chunks++;
        // The CP2102N starts shifting once the transfer completes; it can't be faster than the line.
        int64_t done = io->now_us(io->ctx);
        int64_t from = done > s->wire_free_at ? done : s->wire_free_at;
        s->wire_free_at = from + tp_wire_us(&s->cfg, n);
        off += n;
    }

    s->last_sent = (uint32_t)off;
    if (rc != TP_OK) s->st.write_errors++;
    uint32_t took = (uint32_t)(io->now_us(io->ctx) - start);
    if (took > s->st.write_max_us) s->st.write_max_us = took;
    return rc;
}

#pragma once
// The stored log's on-flash format, free of ESP-IDF so the host tests can run it (test/host/test_logstore.py).
//
// The log area is a ring of 4 KB sectors. Each sector starts with a 16-byte header (magic, sequence number,
// its complement, format) followed by log text up to the first 0xFF byte (erased flash; log text never holds
// 0xFF). Sector i always holds a sequence number s with s % sectors == i, so the newest sector is the valid
// header with the highest number and the oldest is found by walking back from it. Appending fills the newest
// sector; the next one is erased only when it's needed, so a power cut loses at most the bytes being written.
// "Clear" doesn't erase anything: it raises a floor (kept by the caller) below which sectors are ignored.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LS_SECTOR 4096u
#define LS_HDR 16u
#define LS_TEXT (LS_SECTOR - LS_HDR)
#define LS_MAGIC 0x474F4C5Au  // "ZLOG"

typedef struct {
    // Offsets are relative to the start of the log area. Return 0 on success.
    int (*read)(void *ctx, uint32_t off, void *buf, size_t n);
    int (*write)(void *ctx, uint32_t off, const void *buf, size_t n);
    int (*erase)(void *ctx, uint32_t off, size_t n);  // whole sectors
    void *ctx;
} ls_flash_t;

typedef struct {
    ls_flash_t fl;
    uint32_t sectors;
    uint32_t head_seq;    // newest sector's number (0 = nothing written yet)
    uint32_t head_fill;   // text bytes in the newest sector
    uint32_t oldest_seq;  // oldest readable sector's number
    uint32_t floor_seq;   // sectors below this number were cleared
} ls_ring_t;

typedef struct {
    uint32_t seq;  // sector number
    uint32_t off;  // text offset in it
} ls_cursor_t;

// Scans the area (size bytes, whole sectors) and finds where to continue. Returns 0, or -1 on a read error.
int ls_mount(ls_ring_t *r, const ls_flash_t *fl, uint32_t size, uint32_t floor_seq);
// Appends text (0xFF bytes are dropped). Returns 0, or -1 on a flash error (the ring stays usable).
int ls_append(ls_ring_t *r, const char *text, size_t n);
// Ignores everything written so far; returns the new floor for the caller to keep.
uint32_t ls_clear(ls_ring_t *r);
// Approximate bytes of text stored.
uint32_t ls_used(const ls_ring_t *r);

void ls_cursor_oldest(const ls_ring_t *r, ls_cursor_t *c);
// A cursor at the start of the sector holding the last `bytes` bytes (or the oldest).
void ls_cursor_tail(const ls_ring_t *r, uint32_t bytes, ls_cursor_t *c);
// Reads text from the cursor onward, oldest to newest, across sectors. Returns the bytes read, 0 at the end.
// A sector overwritten while being read is skipped (its text would be newer than what came before).
size_t ls_read(ls_ring_t *r, ls_cursor_t *c, char *buf, size_t n);

// ---------------------------------------------------------------- line stamps
// ESP-IDF log lines look like "I (1234) tag: message", the number being ms since boot. Stored lines get a
// UTC time in front: "2026-11-24T04:25:36.123Z I (1234) tag: message". boot_epoch_ms = Unix time of the
// boot in ms, or 0 when the clock wasn't set: then the stamp is "boot+1.234s". Lines without an ms number
// (continuations) get the last one seen (*last_ms).

// Writes the stamp for a moment (ms after boot) into out (at least 32 bytes), with a trailing space.
size_t ls_stamp(int64_t boot_epoch_ms, uint32_t ms, char *out);
// Stamps every complete line of in[0..n) into out. Returns the bytes written to out; *used = input bytes
// consumed (up to and including the last '\n' that fit). out needs n + 32 bytes per line to take it all.
size_t ls_stamp_lines(const char *in, size_t n, int64_t boot_epoch_ms, uint32_t *last_ms, char *out, size_t cap,
                      size_t *used);
// A note line: "<stamp># text\n". Returns its length (cut to fit cap).
size_t ls_note(int64_t boot_epoch_ms, uint32_t ms, const char *text, char *out, size_t cap);
// The Unix time in ms of a stored line's stamp, or 0 if it has none / a boot+ stamp.
int64_t ls_parse_stamp(const char *line, size_t n);

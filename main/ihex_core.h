#pragma once
// Streaming Intel HEX parser for the TI dongle flasher (Koenkk's Z-Stack builds ship as .hex).
// Portable: no ESP-IDF here, so the host test (test/host/test_tiflash.py) drives the very same code.
//
// Feed it the file in pieces of any size (a byte at a time works); it hands each data record to a
// callback as (absolute address, bytes) so the caller can write them into a flat binary image that it
// pre-filled with 0xFF. RAM: one decoded record (260 bytes) plus a small table of the address ranges seen.
//
// Records: 00 data, 01 end of file, 02 extended segment address (base = value << 4), 04 extended linear
// address (base = value << 16), 03/05 start address (length checked, otherwise ignored). Every record's
// checksum is verified. Lines may end in LF, CRLF or CR; blank lines and spaces/tabs around a record are
// fine; anything else outside a record is an error, as is anything but whitespace after the EOF record.
//
// Ordering: records may come in any order (TI's tools write them ascending, but nothing requires it).
// What is refused is what's ambiguous:
//  - two data records that write the same byte (overlap), even with equal values - we keep no copy of
//    the data to compare against, and two values for one flash byte means the file is broken;
//  - a data record whose 16-bit offset runs past 0xFFFF: tools disagree on whether that wraps inside the
//    segment or carries into the next one.
// Overlap is caught with a table of the merged address ranges written so far (IHEX_MAX_RANGES); a file
// fragmented into more separate pieces than that is refused rather than checked partially.
//
// Errors carry the line number: "line 12: checksum mismatch (...)". Once an error is reported the parser
// stays failed and keeps returning it.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IHEX_MAX_RANGES 32

// Called once per data record. Return 0 to go on, anything else stops the parse (err "rejected by caller",
// or the caller's own message if it set one with ihex_fail()).
typedef int (*ihex_data_cb)(void *ctx, uint32_t addr, const uint8_t *data, size_t len);

typedef struct {
    // what the caller reads back
    uint32_t min_addr;     // lowest address written (valid when total_bytes > 0)
    uint32_t end_addr;     // one past the highest address written: the flat image size when starting at 0
    uint32_t total_bytes;  // data bytes delivered (no overlaps, so also the bytes covered)
    uint32_t records;      // records parsed, all types
    uint32_t line;         // current line, 1-based
    bool eof_seen;         // the 01 record has been parsed

    // internal state
    ihex_data_cb cb;
    void *ctx;
    uint32_t base;        // from the last 02/04 record
    uint8_t rec[5 + 255];  // decoded record bytes
    uint16_t rn;           // bytes decoded so far in rec
    uint8_t hi;            // pending high nibble
    bool half;             // a nibble is pending
    bool in_rec;           // between ':' and the record's last byte
    bool rec_done;         // record parsed, waiting for the line end
    bool last_cr;          // previous char was CR (so a following LF doesn't count a second line)
    struct {
        uint32_t lo, hi;  // [lo, hi)
    } ranges[IHEX_MAX_RANGES];
    uint8_t nranges;
    char err[96];  // "" while fine
} ihex_t;

void ihex_init(ihex_t *h, ihex_data_cb cb, void *ctx);

// Parses len more bytes of the file. NULL while fine, else the error (also kept in h->err).
const char *ihex_feed(ihex_t *h, const char *buf, size_t len);

// End of input: NULL when the file was complete (EOF record seen, no half record), else why not.
const char *ihex_finish(ihex_t *h);

// For the callback: fail the parse with this message (prefixed with the line number).
void ihex_fail(ihex_t *h, const char *msg);

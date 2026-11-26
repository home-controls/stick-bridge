#pragma once
// The last 32 KB of ESP_LOG output, for the dashboard's live log and for the stored log's writer (logstore.c).
// Lines are still printed on the Type-C console.
//
// The buffer sits in memory that a software restart, a crash or a watchdog reset doesn't clear. Lines the
// writer hadn't saved yet when the board went down are handed over at the next boot (logbuf_take_prev), so the
// stored log keeps the last moments before a crash. A power cut does lose them.

#include <stddef.h>
#include <stdint.h>

#define LOGBUF_SIZE (32 * 1024)

void logbuf_init(void);
// Copies log text written after byte position `since` (an ever-growing counter) into out.
// Returns the bytes copied; *next is the position to ask for next time.
size_t logbuf_read(uint32_t since, char *out, size_t out_len, uint32_t *next);
uint32_t logbuf_total(void);

// For the writer: where it has saved up to, and this boot's number + clock (kept for the next boot).
uint32_t logbuf_flushed(void);
void logbuf_set_flushed(uint32_t pos);
void logbuf_set_boot(uint32_t boot_no, int64_t boot_epoch_ms);

// The previous boot's unsaved lines, if the reset kept memory. The caller frees *text. Returns 0 if none.
size_t logbuf_take_prev(char **text, int64_t *boot_epoch_ms, uint32_t *boot_no, uint32_t *lost);

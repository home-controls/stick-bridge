#pragma once
// A copy of the stored log on a microSD card, one file per UTC day: /zblog/2026-11-24.log (FAT32).
//
// Optional: with no card the bridge just keeps its flash log. A card is looked for at boot and every
// 30 s, and is used as soon as one is in (no card-detect pin on this board, so a pulled card shows up
// as a failed write or status check, and the bridge goes back to looking). The card is never formatted
// by the bridge. When it's nearly full the oldest day files are deleted.
//
// Board wiring (Waveshare ESP32-P4-ETH schematic): SDMMC 4-bit, CLK 43, CMD 44, D0-D3 39-42, card power
// through a P-MOSFET on GPIO45 (low = on), and the pins' I/O bank (VDDPST_5) fed by the P4's LDO channel 4.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_server.h"

// Housekeeping (look for a card, check it's still there, make room). Called from the log writer's task.
void sdlog_poll(void);
// Appends stored-log text to the day file for epoch_ms (0 = clock not set: boot-<n>.log).
void sdlog_append(const char *text, size_t n, int64_t epoch_ms, uint32_t boot_no);
// "sd": state, card, space, files.
void sdlog_add_json(cJSON *root);
// Streams one day file (name as listed) as text/plain.
esp_err_t sdlog_send_file(httpd_req_t *req, const char *name, bool download);

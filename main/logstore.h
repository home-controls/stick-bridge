#pragma once
// The stored log: every ESP_LOG line, stamped with UTC time, kept in flash across reboots and power cuts
// (and copied to a microSD card when one is in, sdlog.c).
//
// Where: if the flash chip is bigger than the 16 MB the firmware is built for (this board has a 32 MB part)
// and the chip can address it, the whole upper part is the log (about 16 MB, many months). Otherwise a 2 MB
// area after the dongle image. Format and its tests: logstore_core.h, test/host/test_logstore.py.

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_http_server.h"

void logstore_start(void);
// "flash": where, how big, how much is used, the oldest/newest stamps; plus "sd" (sdlog.c).
void logstore_add_json(cJSON *root);
// Streams the stored log as text/plain, oldest first; tail > 0 = only about the last `tail` bytes.
esp_err_t logstore_send(httpd_req_t *req, uint32_t tail, bool download);
// Hides everything stored so far (the card's files stay). Returns false if the log isn't running.
bool logstore_clear(void);

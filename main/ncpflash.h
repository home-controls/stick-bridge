#pragma once
// Flashing the Zigbee dongle's own firmware (EmberZNet .gbl) from the bridge.
//
// The image is uploaded first and kept in its own flash area ("dongle_fw", 2 MB after the app slots), so
// it's checked whole before the dongle is touched and is still there to try again if flashing fails.
// Flashing takes the dongle away from Zigbee2MQTT for a minute or two; the protocol is in ncpflash_core.c.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

void ncpflash_start(void);

// Upload into the image area: begin (size, file name), write in order, end. end checks the image (GBL
// structure + CRC-32, MD5 read back from flash) and keeps it only when it's fine. NULL or the reason.
const char *ncpflash_upload_begin(uint32_t size, const char *name);
const char *ncpflash_upload_write(const uint8_t *data, size_t len);
const char *ncpflash_upload_end(bool complete);
const char *ncpflash_delete(void);

// Starts flashing the stored image. md5 must be the stored image's (so it's the one the user saw).
// false with why when it can't start.
bool ncpflash_flash(const char *md5, char *why, size_t why_len);

// True while an upload or a flash is under way (no reboots, no bridge firmware updates meanwhile).
bool ncpflash_busy(void);

// State for GET /api/dongle.
void ncpflash_add_json(cJSON *root);

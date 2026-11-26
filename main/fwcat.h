#pragma once
// Firmware for the dongle from GitHub releases (default Nerivec/silabs-firmware-builder; any repo with the
// same file naming works, e.g. a fork with your own builds). A check reads the newest releases through the
// GitHub API as a stream (fwcat_core.c), keeps the coordinator (zigbee_ncp) builds for this dongle's board
// and ranks them for its wiring and the bridge's baud. A download streams the chosen .gbl straight into the
// dongle image area (ncpflash.c: GBL check + MD5 read-back) and checks GitHub's SHA-256; flashing it is
// still the separate, explicit step on the System page.

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

void fwcat_start(void);
void fwcat_check_now(void);
// Download the asset of that name from the last check. false (with why) when it can't start.
bool fwcat_download(const char *name, char *why, size_t why_len);
// Adds "catalog" to root.
void fwcat_add_json(cJSON *root);

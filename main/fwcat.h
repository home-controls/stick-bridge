#pragma once
// Firmware for each radio's stick from GitHub releases (Silicon Labs: default Nerivec/silabs-firmware-builder,
// any repo with the same file naming works; TI: Koenkk/Z-Stack-firmware). A check reads the newest releases
// through the GitHub API as a stream (fwcat_core.c), keeps the builds for the stick's board and the radio's role
// (zigbee_ncp, openthread_rcp, Z-Stack coordinator) and ranks them for its wiring and the radio's baud. Every
// radio keeps its own result, for the stick it had then. A download streams the chosen file straight into the
// image area (ncpflash.c: checks + MD5 read-back) and checks GitHub's SHA-256; flashing it is still the
// separate, explicit step on the System page.

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

void fwcat_start(void);
void fwcat_check_now(int radio);
// The radio's role or stick changed: its last check no longer applies.
void fwcat_forget(int radio);
// Download the asset of that name from the radio's last check. false (with why) when it can't start.
bool fwcat_download(int radio, const char *name, char *why, size_t why_len);
// Adds "catalog" (the radio's last check) to root.
void fwcat_add_json(cJSON *root, int radio);

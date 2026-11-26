#pragma once
// Updates from git: the bridge reads a release manifest (JSON on the git server, published by
// tools/publish_release.py) and, when asked, downloads the image and installs it itself:
// checked like an upload (chip, project), plus its size and SHA-256 from the manifest, then the
// usual trial boot with rollback.
//
// manifest.json: {"version": "1.0.10", "file": "zigbee_eth_bridge-1.0.10.bin", "size": 765000,
//                 "sha256": "<64 hex>", "date": "2026-11-21", "notes": "..."}
// "file" is relative to the manifest (or a full URL).

#include <stdbool.h>
#include "cJSON.h"

void updater_start(void);
// Ask for a check now (runs in the updater's task).
void updater_check_now(void);
// Start installing the release found by the last check. false (with why) if there is none or one is running.
bool updater_install(char *why, int why_len);
// Adds the updater's state as "update" to root.
void updater_add_json(cJSON *root);
// The version on offer, or "" (for the dashboard's header).
void updater_available(char *out, int out_len);

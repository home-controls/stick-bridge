#pragma once
// Checks the start of a firmware image before any of it is written: an ESP32-P4 app image of this
// project (an image for another chip or another project is refused). Used by the upload on the
// System page and by the updater that downloads releases from git.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_partition.h"

// Bytes needed before fw_check_header() can decide.
size_t fw_header_len(void);
// NULL when the image looks right (version copied to ver_out, >= 33 bytes), else why not.
const char *fw_check_header(const uint8_t *buf, size_t len, char *ver_out);
// a.b.c compare (anything after '+' or '-' ignored): <0, 0, >0.
int fw_version_cmp(const char *a, const char *b);
// One firmware write at a time (upload and git install share the spare slot). Non-blocking.
bool fw_write_lock(void);
void fw_write_unlock(void);
// Reads back the first len bytes of part and compares their MD5 with md5_hex (32 lowercase hex).
// Catches a flash write that didn't stick, before the slot is made bootable. NULL when they match.
const char *fw_readback_md5(const esp_partition_t *part, size_t len, const char *md5_hex);
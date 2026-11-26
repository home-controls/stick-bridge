#pragma once
// Flashing dongles that aren't Silicon Labs: TI CC26xx/CC13xx (ROM bootloader, tiflash_core.c, from
// Koenkk's .zip or a .hex), dresden elektronik ConBee II/III (.GCF, gcfflash_core.c) and the Nordic
// nRF52840 dongle (open USB DFU bootloader, nrfdfu_core.c, from a .hex). The image store, the dongle claim
// and the progress/state reporting stay in ncpflash.c; this file adapts the cores to them.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dongles.h"

typedef enum { IMG_GBL = 0, IMG_TI, IMG_GCF, IMG_NRF, IMG_UNKNOWN } img_kind_t;

// Which kind a file is, from its name and the dongle's family (a .hex is TI or Nordic by the dongle).
img_kind_t xf_kind_for(const char *name, family_t family);
const char *xf_kind_name(img_kind_t k);  // "Silicon Labs .gbl", "TI Z-Stack", "ConBee .GCF", "nRF52840 .hex"
// The family a kind is for.
family_t xf_kind_family(img_kind_t k);

typedef int (*xf_read_fn)(uint32_t off, uint8_t *buf, size_t len);  // the stored file; 0 ok

// Checks a stored file of that kind (structure, checksums, that it's for this dongle and safe to write).
// NULL when it's fine; desc gets a one-line summary (version, chip, size).
const char *xf_check(img_kind_t k, xf_read_fn rd, uint32_t size, const char *name, char *desc, size_t desc_len);

typedef struct {
    char before[64], after[64];
    const char *entered;     // how the bootloader was reached
    bool left_running;       // failed, the old firmware still answers
    bool in_bootloader;      // failed, the stick sits in its bootloader (flash again)
    uint32_t link_baud;      // the baud the new firmware talks at (the bridge switches to it), 0 = unchanged
} xf_result_t;

typedef void (*xf_progress_fn)(const char *step, uint32_t done, uint32_t total);

// The whole job on a claimed dongle. True when the new firmware is on (and answering where the protocol
// lets us ask); else err says what went wrong.
bool xf_flash(img_kind_t k, xf_read_fn rd, uint32_t size, const char *name, xf_progress_fn progress, xf_result_t *r,
              char *err, size_t errlen);

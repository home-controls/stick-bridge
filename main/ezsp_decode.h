#pragma once
// Passive decoder for the bytes the bridge forwards: ASH framing (UG101) in both directions and
// the EZSP frames inside (EZSP v14+ layouts, as zigbee-herdsman's ember adapter reads them).
// It only watches; the forwarded bytes are never changed or delayed by it.

#include <stddef.h>
#include <stdint.h>

typedef enum { DIR_HOST = 0, DIR_NCP = 1 } ash_dir_t;  // HOST = Z2M -> dongle

void ezsp_decode_reset(void);
// Feed bytes as they pass. Takes the stats lock itself.
void ezsp_decode_feed(ash_dir_t dir, const uint8_t *data, size_t len, int64_t now_us);

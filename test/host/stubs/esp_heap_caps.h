#pragma once
// Host stand-in: the bridge keeps its 24 h history in PSRAM; on the host it is plain heap.
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_8BIT (1u << 2)
static inline void *heap_caps_calloc(size_t n, size_t size, unsigned caps) { (void)caps; return calloc(n, size); }

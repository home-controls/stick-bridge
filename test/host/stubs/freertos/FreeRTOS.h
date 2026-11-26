// Host stand-in for the bits of FreeRTOS the decoder and stats use (single thread).
#pragma once
#include <stdint.h>
typedef void *SemaphoreHandle_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE 1

#pragma once
#include "FreeRTOS.h"
// A recursion check stands in for the mutex: taking it twice is the deadlock the firmware would hit.
extern int g_lock_depth;
extern int g_lock_violations;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) {
    (void)s; (void)t;
    if (g_lock_depth++ > 0) g_lock_violations++;
    return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    (void)s;
    g_lock_depth--;
    return pdTRUE;
}

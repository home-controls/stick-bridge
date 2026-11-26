#pragma once
#include <stdint.h>
typedef void (*esp_timer_cb_t)(void *arg);
typedef struct { esp_timer_cb_t callback; void *arg; int dispatch_method; const char *name; int skip_unhandled_events; } esp_timer_create_args_t;
typedef void *esp_timer_handle_t;
typedef int esp_err_t;
extern int64_t g_now_us;           // the harness's clock
extern esp_timer_cb_t g_tick_cb;   // the stats' 1 s tick, called by the harness
static inline int64_t esp_timer_get_time(void) { return g_now_us; }
static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *h) { g_tick_cb = a->callback; *h = (void *)1; return 0; }
static inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t h, uint64_t us) { (void)h; (void)us; return 0; }

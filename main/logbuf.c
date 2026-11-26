#include "logbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"

#define MAGIC 0x4C4F4742u  // "LOGB"

typedef struct {
    uint32_t magic;
    uint32_t total;    // bytes ever written this boot
    uint32_t flushed;  // bytes the writer has saved
    uint32_t boot_no;
    int64_t boot_epoch_ms;
    uint32_t check;
    char buf[LOGBUF_SIZE];
} keep_t;

// Not cleared at boot: survives software restarts, panics and watchdog resets (garbage after power-on).
static __NOINIT_ATTR keep_t s_k;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig;

static char *s_prev;
static size_t s_prev_len;
static int64_t s_prev_epoch;
static uint32_t s_prev_boot, s_prev_lost;

static uint32_t check_of(const keep_t *k) {
    return k->magic ^ k->total ^ (k->flushed * 2654435761u) ^ (k->boot_no * 40503u) ^ (uint32_t)k->boot_epoch_ms ^
           (uint32_t)((uint64_t)k->boot_epoch_ms >> 32) ^ 0xA5A5A5A5u;
}

static void append(const char *p, size_t n) {
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\033' || (uint8_t)c == 0xFF) continue;  // no colour codes (colours are off); 0xFF ends text on flash
        s_k.buf[s_k.total % LOGBUF_SIZE] = c;
        s_k.total++;
    }
    s_k.check = check_of(&s_k);
    portEXIT_CRITICAL(&s_mux);
}

static int hook(const char *fmt, va_list ap) {
    char line[256];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) append(line, n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);
    return s_orig ? s_orig(fmt, ap) : n;
}

void logbuf_init(void) {
    esp_reset_reason_t why = esp_reset_reason();
    bool kept = why != ESP_RST_POWERON && why != ESP_RST_EXT && why != ESP_RST_UNKNOWN && s_k.magic == MAGIC &&
                s_k.check == check_of(&s_k) && s_k.flushed <= s_k.total;
    if (kept && s_k.total > s_k.flushed) {
        uint32_t from = s_k.flushed;
        if (s_k.total - from > LOGBUF_SIZE) {
            s_prev_lost = s_k.total - from - LOGBUF_SIZE;
            from = s_k.total - LOGBUF_SIZE;
        }
        s_prev_len = s_k.total - from;
        s_prev = malloc(s_prev_len);
        if (s_prev) {
            for (size_t i = 0; i < s_prev_len; i++) s_prev[i] = s_k.buf[(from + i) % LOGBUF_SIZE];
            s_prev_epoch = s_k.boot_epoch_ms;
            s_prev_boot = s_k.boot_no;
        } else {
            s_prev_len = 0;
        }
    }
    s_k.magic = MAGIC;
    s_k.total = 0;
    s_k.flushed = 0;
    s_k.boot_no = 0;
    s_k.boot_epoch_ms = 0;
    s_k.check = check_of(&s_k);
    s_orig = esp_log_set_vprintf(hook);
}

size_t logbuf_read(uint32_t since, char *out, size_t out_len, uint32_t *next) {
    portENTER_CRITICAL(&s_mux);
    uint32_t total = s_k.total;
    uint32_t oldest = total > LOGBUF_SIZE ? total - LOGBUF_SIZE : 0;
    if (since < oldest || since > total) since = oldest;
    size_t avail = total - since;
    if (avail > out_len) {
        // The dashboard wants the newest; the writer asks for at most what it can take and reads on.
        avail = out_len;
    }
    for (size_t i = 0; i < avail; i++) out[i] = s_k.buf[(since + i) % LOGBUF_SIZE];
    portEXIT_CRITICAL(&s_mux);
    *next = since + avail;
    return avail;
}

uint32_t logbuf_total(void) {
    portENTER_CRITICAL(&s_mux);
    uint32_t t = s_k.total;
    portEXIT_CRITICAL(&s_mux);
    return t;
}

uint32_t logbuf_flushed(void) {
    portENTER_CRITICAL(&s_mux);
    uint32_t f = s_k.flushed;
    portEXIT_CRITICAL(&s_mux);
    return f;
}

void logbuf_set_flushed(uint32_t pos) {
    portENTER_CRITICAL(&s_mux);
    if (pos <= s_k.total) s_k.flushed = pos;
    s_k.check = check_of(&s_k);
    portEXIT_CRITICAL(&s_mux);
}

void logbuf_set_boot(uint32_t boot_no, int64_t boot_epoch_ms) {
    portENTER_CRITICAL(&s_mux);
    s_k.boot_no = boot_no;
    s_k.boot_epoch_ms = boot_epoch_ms;
    s_k.check = check_of(&s_k);
    portEXIT_CRITICAL(&s_mux);
}

size_t logbuf_take_prev(char **text, int64_t *boot_epoch_ms, uint32_t *boot_no, uint32_t *lost) {
    *text = s_prev;
    *boot_epoch_ms = s_prev_epoch;
    *boot_no = s_prev_boot;
    *lost = s_prev_lost;
    size_t n = s_prev_len;
    s_prev = NULL;
    s_prev_len = 0;
    return n;
}

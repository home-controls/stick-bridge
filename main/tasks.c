#include "tasks.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include "sdkconfig.h"

static const char *TAG = "tasks";

#define TASKS_MAX 24

typedef struct {
    const char *name;
    TaskHandle_t h;
    uint32_t stack;
    bool psram;
} task_rec_t;

static task_rec_t s_t[TASKS_MAX];
static int s_n;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

TaskHandle_t task_start(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, bool psram) {
    TaskHandle_t h = NULL;
    BaseType_t ok = pdFAIL;
    if (psram) {
        ok = xTaskCreateWithCaps(fn, name, stack, arg, prio, &h, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) ESP_LOGW(TAG, "%s: no PSRAM for its stack, using internal RAM", name);
    }
    if (ok != pdPASS) {
        psram = false;
        ok = xTaskCreate(fn, name, stack, arg, prio, &h);
    }
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "%s: couldn't start (%lu bytes of stack)", name, (unsigned long)stack);
        return NULL;
    }
    portENTER_CRITICAL(&s_mux);
    if (s_n < TASKS_MAX) s_t[s_n++] = (task_rec_t){.name = name, .h = h, .stack = stack, .psram = psram};
    portEXIT_CRITICAL(&s_mux);
    return h;
}

// System tasks: created by ESP-IDF and its components, found by name, sizes from their configuration.
static const struct {
    const char *name;
    uint32_t stack;
} SYS[] = {
    {"httpd", 10240},  // web.c: httpd_config_t.stack_size
    {"tiT", CONFIG_LWIP_TCPIP_TASK_STACK_SIZE},
    {"emac_rx", 4096},  // ETH_MAC_DEFAULT_CONFIG
    {"USB-CDC", 5120},  // dongle.c: driver_task_stack_size
    {"esp_timer", CONFIG_ESP_TIMER_TASK_STACK_SIZE},
    {"Tmr Svc", CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH},
    {"main", CONFIG_ESP_MAIN_TASK_STACK_SIZE},
    {"IDLE0", CONFIG_FREERTOS_IDLE_TASK_STACKSIZE},
    {"IDLE1", CONFIG_FREERTOS_IDLE_TASK_STACKSIZE},
    {"ipc0", CONFIG_ESP_IPC_TASK_STACK_SIZE},
    {"ipc1", CONFIG_ESP_IPC_TASK_STACK_SIZE},
};

static void add_task(cJSON *a, const char *name, TaskHandle_t h, uint32_t stack, bool psram, bool system) {
    if (!h || xTaskGetHandle(name) != h) return;  // never read a task that has ended
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", name);
    cJSON_AddNumberToObject(o, "stack", stack);
    cJSON_AddNumberToObject(o, "min_free", uxTaskGetStackHighWaterMark(h));  // bytes on ESP-IDF
    cJSON_AddStringToObject(o, "where", psram ? "psram" : "internal");
    if (system) cJSON_AddBoolToObject(o, "system", true);
    cJSON_AddItemToArray(a, o);
}

static void add_heap(cJSON *m, const char *key, uint32_t caps) {
    cJSON *o = cJSON_AddObjectToObject(m, key);
    cJSON_AddNumberToObject(o, "free", heap_caps_get_free_size(caps));
    cJSON_AddNumberToObject(o, "min", heap_caps_get_minimum_free_size(caps));
    cJSON_AddNumberToObject(o, "largest", heap_caps_get_largest_free_block(caps));
    cJSON_AddNumberToObject(o, "total", heap_caps_get_total_size(caps));
}

void tasks_add_json(cJSON *root) {
    task_rec_t t[TASKS_MAX];
    portENTER_CRITICAL(&s_mux);
    int n = s_n;
    memcpy(t, s_t, sizeof(task_rec_t) * n);
    portEXIT_CRITICAL(&s_mux);
    cJSON *a = cJSON_AddArrayToObject(root, "tasks");
    for (int i = 0; i < n; i++) add_task(a, t[i].name, t[i].h, t[i].stack, t[i].psram, false);
    for (size_t i = 0; i < sizeof(SYS) / sizeof(SYS[0]); i++) add_task(a, SYS[i].name, xTaskGetHandle(SYS[i].name), SYS[i].stack, false, true);
    cJSON *m = cJSON_AddObjectToObject(root, "memory");
    add_heap(m, "internal", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    add_heap(m, "dma", MALLOC_CAP_DMA);
    add_heap(m, "psram", MALLOC_CAP_SPIRAM);
}

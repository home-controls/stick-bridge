#include "sysinfo.h"

#include <stdio.h>

#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "watchdog.h"

static temperature_sensor_handle_t s_tsens;
static esp_reset_reason_t s_reset;

void sysinfo_init(void) {
    s_reset = esp_reset_reason();
    temperature_sensor_config_t c = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&c, &s_tsens) == ESP_OK) temperature_sensor_enable(s_tsens);
    else s_tsens = NULL;
}

float sysinfo_temperature(void) {
    float t = 0;
    if (s_tsens) temperature_sensor_get_celsius(s_tsens, &t);
    return t;
}

static const char *reset_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "Power on";
        case ESP_RST_EXT: return "Reset pin";
        case ESP_RST_SW: return "Software restart";
        case ESP_RST_PANIC: return "Crash (panic)";
        case ESP_RST_INT_WDT: return "Interrupt watchdog";
        case ESP_RST_TASK_WDT: return "Task watchdog";
        case ESP_RST_WDT: return "Watchdog";
        case ESP_RST_DEEPSLEEP: return "Deep sleep wake";
        case ESP_RST_BROWNOUT: return "Brownout (power dip)";
        default: return "Unknown";
    }
}

void sysinfo_add_json(cJSON *root) {
    const esp_app_desc_t *d = esp_app_get_description();
    cJSON *fw = cJSON_AddObjectToObject(root, "fw");
    cJSON_AddStringToObject(fw, "version", d->version);
    cJSON_AddStringToObject(fw, "project", d->project_name);
    cJSON_AddStringToObject(fw, "idf", esp_get_idf_version());
    char built[40];
    snprintf(built, sizeof(built), "%s %s", d->date, d->time);
    cJSON_AddStringToObject(fw, "built", built);
    const esp_partition_t *run = esp_ota_get_running_partition();
    cJSON_AddStringToObject(fw, "partition", run ? run->label : "?");
    esp_ota_img_states_t st;
    bool pending = run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    cJSON_AddBoolToObject(fw, "pending_verify", pending);

    cJSON *sys = cJSON_AddObjectToObject(root, "sys");
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    char chip[32];
    snprintf(chip, sizeof(chip), "ESP32-P4 rev %d.%d, %d cores", ci.revision / 100, ci.revision % 100, ci.cores);
    cJSON_AddStringToObject(sys, "chip", chip);
    cJSON_AddStringToObject(sys, "reset_reason", watchdog_last_reason()[0] ? "Watchdog" : reset_name(s_reset));
    cJSON_AddStringToObject(sys, "watchdog", watchdog_last_reason());
    cJSON_AddNumberToObject(sys, "temp_c", sysinfo_temperature());
    // heap_* = internal RAM (what can run out); psram_* = the 32 MB PSRAM (0 when it isn't enabled/found)
    cJSON_AddNumberToObject(sys, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(sys, "heap_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(sys, "heap_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(sys, "psram_total", heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(sys, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(sys, "psram_min", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

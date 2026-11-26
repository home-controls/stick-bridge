#pragma once
// Firmware/system facts for the dashboard: version, build, reset reason, chip temperature, memory,
// which app slot is running.

#include "cJSON.h"

void sysinfo_init(void);
float sysinfo_temperature(void);
// Adds "fw" and "sys" objects to root.
void sysinfo_add_json(cJSON *root);

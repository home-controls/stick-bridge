#pragma once
// The bridge's tasks in one place: where each stack lives and how close it has come to full.
//
// task_start() creates a task whose stack is in internal RAM or, with psram, in PSRAM (xTaskCreateWithCaps).
// A task with its stack in PSRAM must never write flash itself (NVS, OTA, the log or image partitions):
// flash writes turn the cache off, and a stack in PSRAM goes with it. The radio tasks qualify; anything that
// saves settings or writes images stays internal.
//
// GET /api/tasks lists every task started here plus the system tasks known by name (web server, network,
// Ethernet, USB, timers, idle), each with its stack size and the least free stack it has had (FreeRTOS
// high-water mark), and the heap: internal, DMA-capable and PSRAM free / lowest / largest block.

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

TaskHandle_t task_start(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, bool psram);
void tasks_add_json(cJSON *root);

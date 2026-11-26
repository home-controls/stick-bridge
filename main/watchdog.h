#pragma once
#include "sdkconfig.h"
// Watchdogs: keep the bridge from sitting there broken while Zigbee2MQTT waits for it.
//   - tasks: the bridge and dongle tasks check in with the task watchdog (10 s, then a reboot)
//   - memory: free heap critically low for 30 s -> restart
//   - network: no Zigbee2MQTT connected and neither the Z2M host nor the gateway answering a ping
//     for 10 minutes -> restart (never while Zigbee2MQTT is connected: that proves the path works)
// A watchdog restart keeps its reason across the reboot (RTC memory) for the dashboard.

void watchdog_start(void);
// Subscribe the calling task to the task watchdog; call watchdog_feed() at least every few seconds.
void watchdog_task_add(void);
void watchdog_feed(void);
// The reason the last restart was a watchdog's, or "" (valid after watchdog_start()).
const char *watchdog_last_reason(void);
// Restart now, keeping why.
void watchdog_restart(const char *why) __attribute__((noreturn));

#ifdef CONFIG_BRIDGE_TEST_HOOKS
// Test builds only: "task" stalls the bridge's receive task, "heap" eats memory, "net" checks an
// unreachable address on short timers. Returns false for an unknown kind.
#include <stdbool.h>
extern volatile bool g_wd_test_stall;
bool watchdog_test(const char *kind);
#endif

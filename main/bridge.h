#pragma once
// TCP server for Zigbee2MQTT: one client at a time (the Z2M host), raw bytes both ways between it
// and the dongle, every byte also fed to the passive decoder.

#include <stdint.h>

void bridge_start(uint16_t port);
// Close the current client, if any (e.g. after the Z2M host setting changed).
void bridge_kick_client(const char *why);

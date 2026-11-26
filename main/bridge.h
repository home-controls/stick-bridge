#pragma once
// TCP servers, one per radio: one client at a time per radio (radio 0 = the Zigbee2MQTT host), raw bytes
// both ways between it and that radio's stick. Radio 0's bytes are also fed to the passive EZSP decoder; the
// other radios get protocol detection and firmware version sniffing only.

#include <stdint.h>

// Starts every enabled radio's listener (ports and on/off from the settings this boot runs with).
void bridge_start(void);
// Close radio 0's current client, if any (e.g. after the Z2M host setting changed).
void bridge_kick_client(const char *why);
void bridge_kick_radio(int radio, const char *why);

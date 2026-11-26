#pragma once
// Ethernet (internal EMAC + IP101 PHY on the ESP32-P4-POE-ETH), static IP or DHCP from settings,
// mDNS (<hostname>.local) and SNTP for event timestamps.

#include <stdbool.h>
#include "settings.h"

void net_start(const bridge_settings_t *cfg);
bool net_has_ip(void);
// The radios' ports changed (a radio came or went, or a port was edited): refresh the mDNS TXT record.
void net_set_ports(void);

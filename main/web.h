#pragma once
// Dashboard + JSON API on port 80.
//   First boot: no password exists; the page asks the user to create one (POST /api/setup).
//   After that: login sets an HttpOnly session cookie. Scripts (Prometheus /metrics, curl) may use
//   HTTP Basic auth as user "admin". Changing requests need the header "X-Bridge: 1".

#include <stdbool.h>

void web_start(void);
// The dashboard is serving (for the trial-boot check in main.c).
bool web_running(void);

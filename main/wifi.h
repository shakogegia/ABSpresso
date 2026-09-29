#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Starts station mode with the credentials from secrets.h and keeps reconnecting.
esp_err_t wifi_start(void);
bool wifi_wait_connected(int timeout_ms);
bool wifi_is_connected(void);

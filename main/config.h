#pragma once

// Device configuration, stored in NVS and set up from a phone through the setup portal
// (portal.c). secrets.h, if present, only supplies defaults for anything not yet saved.

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char server[128];     // base URL, e.g. https://abs.example.com or http://192.168.1.5:13378
    char username[64];    // who signed in (display only; "" when using an API key)
    char access[1024];    // Bearer credential: an API key, or an access token from signing in
    char refresh[1024];   // refresh token for renewing the access token ("" for API keys)
    int skip_back_s;      // Now Playing skip buttons
    int skip_fwd_s;
} app_config_t;

void config_init(void);
// Read-only view; valid for the life of the program (contents change only via the setters).
const app_config_t *config_get(void);
// Replaces and saves the whole configuration.
void config_save(const app_config_t *cfg);
// Saves new tokens after a refresh (thread-safe).
void config_set_tokens(const char *access, const char *refresh);
// Wi-Fi, a server and a credential are all present.
bool config_complete(void);

#pragma once

// Battery saving. There are no buttons, so touch is the only way in:
//
//  - No touch for a while: dim, then turn the screen off (panel asleep, LVGL paused, CPU allowed
//    down to 80 MHz). Audio carries on. The next touch wakes the screen and is swallowed so it
//    can't press anything.
//  - Screen off, nothing playing or downloading, and no USB host (so flashing and logs keep
//    working): after a longer while, deep sleep. Touching the screen or pressing BOOT wakes it,
//    which restarts the firmware (the library comes back from the SD cache in ~2 s).
//
// Timeouts and brightness are user settings, kept in NVS.

#include <stdbool.h>

typedef struct {
    int brightness;   // 10-100 %
    int screen_off_s; // 0 = never
    int sleep_min;    // after the screen goes off; 0 = never
} power_config_t;

void power_init(void);
void power_get_config(power_config_t *out);
void power_set_config(const power_config_t *cfg);

// Called by the touch reader on every poll. Returns whether LVGL should see the press (false
// while swallowing the touch that woke the screen).
bool power_filter_touch(bool pressed);
// Holds the screen on and the device awake (UI capture builds; setup does this itself).
void power_keep_awake(bool on);

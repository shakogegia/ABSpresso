#pragma once

// Battery level from the LiPo voltage (GPIO8 via a 1:3 divider).
//
// The charger's status pin only drives the charge LED and USB power isn't wired to a GPIO, so
// charging is inferred from the voltage: plugging in makes it jump up and unplugging makes it drop
// (with USB present the battery is switched off the load and only charges), and between those the
// trend over a few minutes decides. A USB host on the S3's own USB port also means power.
// Full is inferred too: the charger holds ~4.2 V while charging, then stops, and the cell rests
// slightly lower and flat; so it takes ~3 minutes on power before "full" can be recognised.

#include <stdbool.h>

typedef struct {
    bool present;     // false if the reading is implausible (no battery / sense line)
    float volts;
    int percent;      // 0-100, from a typical LiPo curve (meaningless while charging)
    bool charging;    // on external power, still charging
    bool charged;     // on external power, charging finished
} battery_status_t;

void battery_init(void);
void battery_get(battery_status_t *out);

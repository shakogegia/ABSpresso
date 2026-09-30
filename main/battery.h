#pragma once

// Battery level from the LiPo voltage (GPIO8 via a 1:3 divider).
//
// The charger's status pin only drives the charge LED and USB power isn't wired to a GPIO, so
// charging is inferred from the voltage: plugging in makes it jump up and unplugging makes it drop
// (with USB present the battery is switched off the load and only charges), and between those the
// trend over a few minutes decides. "Charged" is charging with the voltage at the top and flat.

#include <stdbool.h>

typedef struct {
    bool present;     // false if the reading is implausible (no battery / sense line)
    float volts;
    int percent;      // 0-100, from a typical LiPo curve (reads high while charging)
    bool charging;
    bool charged;
} battery_status_t;

void battery_init(void);
void battery_get(battery_status_t *out);

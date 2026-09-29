#pragma once

#include "lvgl.h"

// Background loader for cover art: download a resized JPEG from the server, decode it into a
// dimmed RGB565 image in PSRAM, and hand it to the UI.

void cover_init(void);

// Non-blocking. Only the most recent request is honoured.
void cover_request(const char *item_id);

// Returns a newly finished cover (the caller owns it), or NULL. Call from the LVGL task.
lv_image_dsc_t *cover_take(void);

// Frees a cover returned by cover_take(). Detach it from any widget first.
void cover_free(lv_image_dsc_t *dsc);

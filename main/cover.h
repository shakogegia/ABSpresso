#pragma once

#include "lvgl.h"

// Cover art cache. Covers are downloaded resized by the server, decoded into RGB565 LVGL images in
// PSRAM by a background task, and kept in a small LRU cache.
//
// Everything except cover_init() must be called from the LVGL task.

typedef enum {
    COVER_THUMB,     // carousel card, full brightness
    COVER_BACKDROP,  // full-screen player background, dimmed
} cover_kind_t;

#define COVER_THUMB_SIZE 160

void cover_init(void);

// Returns the cover if cached; otherwise queues a download (most recent request first) and
// returns NULL. Call it every refresh for each cover on screen: that keeps visible covers
// fresh in the LRU so they are never evicted while displayed.
const lv_image_dsc_t *cover_get(const char *item_id, cover_kind_t kind);

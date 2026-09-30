#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "abs_api.h"

// All ui_* calls must hold the LVGL port lock (they are LVGL calls).
void ui_init(void);
void ui_show_message(const char *msg);
// The UI keeps using `books` until the next ui_set_books(); the caller frees the old array after.
void ui_set_books(const abs_book_t *books, int count);

// Whether the books last passed to ui_set_books() came from the SD cache (vs. the server).
void ui_set_source(bool from_cache);
// When the library was last loaded (lv_tick), and whether it came from the cache.
void ui_get_source(uint32_t *loaded_tick, bool *from_cache);

// Ask the main loop to reload the library.
void ui_request_refresh(void);
// True once after a reload was requested. Lock not needed.
bool ui_take_refresh_request(void);

#pragma once

// The bottom "<  Name  >" pill with a dot per item, used to switch between Home's shelves and the
// Library's views. Tap an arrow or swipe left/right on the pill. The pill keeps its swipes (they
// don't bubble to the page), so they never also move a carousel.

#include <stdbool.h>
#include "lvgl.h"

#define SWITCHER_MAX 6

typedef struct switcher switcher_t;

// Called when the user moves to another item; `dir` is -1 or +1. The owner shows the item and
// then calls switcher_select().
typedef void (*switcher_cb_t)(int index, int dir);

switcher_t *switcher_create(lv_obj_t *parent, int y, const char *const *names, int count, switcher_cb_t cb);
// Disabled items are skipped by the arrows and have no dot (e.g. an empty shelf).
void switcher_set_enabled(switcher_t *sw, int index, bool enabled);
bool switcher_is_enabled(const switcher_t *sw, int index);
// Next enabled item in `dir` from `from`, or -1.
int switcher_next(const switcher_t *sw, int from, int dir);
void switcher_select(switcher_t *sw, int index);

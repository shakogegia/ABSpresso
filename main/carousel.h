#pragma once

// A cover carousel over a list of books. It is virtual: three card objects (previous, current,
// next) are rebound after each move, so it costs the same for 3 books as for 300.
// Swipes are not handled here; the owning page routes its gestures to carousel_step().

#include <stdbool.h>
#include "lvgl.h"

typedef struct carousel carousel_t;

// Cards are placed at `y` (relative to parent), with title/subtitle/position labels below.
carousel_t *carousel_create(lv_obj_t *parent, int y);

// `idx` indexes g_books and must stay valid until the next call.
void carousel_set_items(carousel_t *c, const int *idx, int count, int pos);
void carousel_step(carousel_t *c, int delta);  // animated
void carousel_jump(carousel_t *c, int pos);    // immediate
int carousel_pos(const carousel_t *c);
int carousel_count(const carousel_t *c);

// Pulls covers from the cache; call on every UI refresh while visible.
void carousel_refresh(carousel_t *c);
void carousel_set_hidden(carousel_t *c, bool hidden);
// Shows/hides the "3 / 42" position label.
void carousel_show_position(carousel_t *c, bool show);

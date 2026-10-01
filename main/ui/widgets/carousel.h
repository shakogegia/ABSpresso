#pragma once

// A cover carousel over a list of books. It is virtual: three card objects (previous, current,
// next) are rebound after each move, so it costs the same for 3 books as for 300.
// Swipes are not handled here; the owning page routes its gestures to carousel_step().

#include <stdbool.h>
#include "lvgl.h"

typedef struct carousel carousel_t;

typedef struct {
    int y;        // top of the cards, relative to the parent
    int x;        // horizontal offset of the carousel's centre from the parent's centre
    int width;    // visible width; neighbouring cards are clipped to it
    int title_w;  // widths of the title and author lines (they scroll when longer)
    int sub_w;
} carousel_cfg_t;

// Cards with a title and author line below them.
carousel_t *carousel_create(lv_obj_t *parent, const carousel_cfg_t *cfg);

// `idx` indexes g_books and must stay valid until the next call.
void carousel_set_items(carousel_t *c, const int *idx, int count, int pos);
void carousel_step(carousel_t *c, int delta);  // animated
void carousel_jump(carousel_t *c, int pos);    // immediate
int carousel_pos(const carousel_t *c);
int carousel_count(const carousel_t *c);

// Pulls covers from the cache; call on every UI refresh while visible.
void carousel_refresh(carousel_t *c);
void carousel_set_hidden(carousel_t *c, bool hidden);

#include "carousel.h"

#include <stdio.h>
#include <stdlib.h>
#include "cover.h"
#include "ui_priv.h"

#define CARD_SIZE    COVER_THUMB_SIZE
#define CARD_SPACING (CARD_SIZE + 30)

typedef struct {
    lv_obj_t *root, *img, *placeholder;
    const lv_image_dsc_t *src;
    int pos;  // position in the item list, or -1
} card_t;

struct carousel {
    lv_obj_t *root, *track, *title, *sub;
    card_t cards[3];  // previous, current, next
    const int *idx;
    int n;
    int pos;
    bool animating;
};

static const abs_book_t *book_at(const carousel_t *c, int pos)
{
    return (pos >= 0 && pos < c->n) ? &g_books[c->idx[pos]] : NULL;
}

static void card_bind(carousel_t *c, card_t *card, int pos)
{
    const abs_book_t *b = book_at(c, pos);
    card->pos = b ? pos : -1;
    card->src = NULL;
    lv_obj_add_flag(card->img, LV_OBJ_FLAG_HIDDEN);
    if (!b) {
        lv_obj_add_flag(card->root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(card->root, LV_OBJ_FLAG_HIDDEN);
    // The title stands in until (or unless) the cover arrives.
    lv_label_set_text(card->placeholder, b->title);
}

static void bind(carousel_t *c)
{
    for (int k = 0; k < 3; k++) {
        card_bind(c, &c->cards[k], c->pos + k - 1);
    }
    const abs_book_t *b = book_at(c, c->pos);
    if (!b) {
        lv_label_set_text(c->title, "");
        lv_label_set_text(c->sub, "");
        return;
    }
    char buf[160];
    lv_label_set_text(c->title, b->title);
    ui_book_subtitle(b, buf, sizeof(buf));
    lv_label_set_text(c->sub, buf);
    lv_obj_set_style_text_color(c->sub, ui_book_in_progress(b) ? COLOR_ACCENT : COLOR_MUTED, 0);
}

void carousel_refresh(carousel_t *c)
{
    if (c->animating || lv_obj_has_flag(c->root, LV_OBJ_FLAG_HIDDEN)) return;
    // Request order sets download priority (most recent first): look-ahead, neighbours, centre.
    static const int ahead[] = {2, -2};
    for (int i = 0; i < 2; i++) {
        const abs_book_t *b = book_at(c, c->pos + ahead[i]);
        if (b) cover_get(b->id, COVER_THUMB);
    }
    static const int order[] = {0, 2, 1};
    for (int i = 0; i < 3; i++) {
        card_t *card = &c->cards[order[i]];
        const abs_book_t *b = book_at(c, card->pos);
        if (!b) continue;
        const lv_image_dsc_t *d = cover_get(b->id, COVER_THUMB);
        if (d && d != card->src) {
            lv_image_set_src(card->img, d);
            lv_obj_remove_flag(card->img, LV_OBJ_FLAG_HIDDEN);
            card->src = d;
        }
    }
}

static void track_set_x(void *obj, int32_t x)
{
    lv_obj_set_x(obj, x);
}

static void anim_done(lv_anim_t *a)
{
    carousel_t *c = lv_anim_get_user_data(a);
    c->pos += lv_obj_get_x(c->track) < 0 ? 1 : -1;
    lv_obj_set_x(c->track, 0);
    c->animating = false;
    bind(c);
    carousel_refresh(c);
}

void carousel_step(carousel_t *c, int delta)
{
    delta = delta > 0 ? 1 : -1;
    if (c->animating || !book_at(c, c->pos + delta)) return;
    c->animating = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, c->track);
    lv_anim_set_exec_cb(&a, track_set_x);
    lv_anim_set_values(&a, 0, -delta * CARD_SPACING);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, c);
    lv_anim_set_completed_cb(&a, anim_done);
    lv_anim_start(&a);
}

void carousel_jump(carousel_t *c, int pos)
{
    if (pos < 0) pos = 0;
    if (pos >= c->n) pos = c->n - 1;
    if (pos < 0 || (pos == c->pos && !c->animating)) return;
    lv_anim_delete(c->track, track_set_x);
    lv_obj_set_x(c->track, 0);
    c->animating = false;
    c->pos = pos;
    bind(c);
}

static void on_card_long(lv_event_t *e)
{
    carousel_t *c = lv_event_get_user_data(e);
    for (int k = 0; k < 3; k++) {
        if (lv_event_get_current_target(e) == c->cards[k].root && c->cards[k].pos >= 0) {
            ui_sheet_show(c->idx[c->cards[k].pos]);
        }
    }
}

static void on_card_clicked(lv_event_t *e)
{
    carousel_t *c = lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_current_target(e);
    if (c->animating) return;
    if (target == c->cards[1].root) {
        ui_open_book(c->idx[c->pos]);
    } else {
        carousel_step(c, target == c->cards[0].root ? -1 : 1);
    }
}

carousel_t *carousel_create(lv_obj_t *parent, const carousel_cfg_t *cfg)
{
    carousel_t *c = calloc(1, sizeof(*c));

    // The root clips the peeking neighbours to cfg->width.
    c->root = lv_obj_create(parent);
    lv_obj_remove_style_all(c->root);
    lv_obj_set_size(c->root, cfg->width, CARD_SIZE);
    lv_obj_align(c->root, LV_ALIGN_TOP_MID, cfg->x, cfg->y);
    lv_obj_remove_flag(c->root, LV_OBJ_FLAG_SCROLLABLE);

    c->track = lv_obj_create(c->root);
    lv_obj_remove_style_all(c->track);
    lv_obj_set_size(c->track, cfg->width, CARD_SIZE);
    lv_obj_remove_flag(c->track, LV_OBJ_FLAG_SCROLLABLE);

    for (int k = 0; k < 3; k++) {
        card_t *card = &c->cards[k];
        card->root = lv_obj_create(c->track);
        lv_obj_remove_style_all(card->root);
        lv_obj_set_size(card->root, CARD_SIZE, CARD_SIZE);
        lv_obj_set_pos(card->root, cfg->width / 2 - CARD_SIZE / 2 + (k - 1) * CARD_SPACING, 0);
        lv_obj_set_style_bg_color(card->root, COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(card->root, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(card->root, 10, 0);
        lv_obj_add_flag(card->root, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(card->root, LV_OBJ_FLAG_SCROLLABLE);
        // Short tap plays (or steps to a side card); long-press opens the details sheet.
        lv_obj_add_event_cb(card->root, on_card_clicked, LV_EVENT_SHORT_CLICKED, c);
        lv_obj_add_event_cb(card->root, on_card_long, LV_EVENT_LONG_PRESSED, c);

        card->placeholder = ui_label(card->root, &lv_font_montserrat_16, COLOR_MUTED, CARD_SIZE - 20);
        lv_label_set_long_mode(card->placeholder, LV_LABEL_LONG_WRAP);
        lv_obj_center(card->placeholder);

        card->img = lv_image_create(card->root);
        lv_obj_center(card->img);
        lv_obj_add_flag(card->img, LV_OBJ_FLAG_HIDDEN);

        if (k != 1) {
            // Dim the neighbours without an opacity layer (cheap per-part opacities only).
            lv_obj_set_style_image_opa(card->img, LV_OPA_40, 0);
            lv_obj_set_style_bg_opa(card->root, LV_OPA_40, 0);
            lv_obj_set_style_text_opa(card->placeholder, LV_OPA_40, 0);
        }
    }

    // Fixed widths so long text can't spill toward the screen edge; it scrolls instead.
    c->title = ui_label(parent, &lv_font_montserrat_16, COLOR_TEXT, cfg->title_w);
    lv_label_set_long_mode(c->title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(c->title, LV_ALIGN_TOP_MID, cfg->x, cfg->y + CARD_SIZE + 8);
    c->sub = ui_label(parent, &lv_font_montserrat_14, COLOR_MUTED, cfg->sub_w);
    lv_label_set_long_mode(c->sub, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(c->sub, LV_ALIGN_TOP_MID, cfg->x, cfg->y + CARD_SIZE + 30);

    bind(c);
    return c;
}

void carousel_set_items(carousel_t *c, const int *idx, int count, int pos)
{
    lv_anim_delete(c->track, track_set_x);
    lv_obj_set_x(c->track, 0);
    c->animating = false;
    c->idx = idx;
    c->n = count;
    c->pos = (pos >= 0 && pos < count) ? pos : 0;
    bind(c);
}

int carousel_pos(const carousel_t *c)
{
    return c->pos;
}

int carousel_count(const carousel_t *c)
{
    return c->n;
}

void carousel_set_hidden(carousel_t *c, bool hidden)
{
    lv_obj_t *objs[] = {c->root, c->title, c->sub};
    for (int i = 0; i < 3; i++) {
        if (hidden) lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

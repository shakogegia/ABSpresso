#include "switcher.h"

#include <stdlib.h>
#include "ui_priv.h"

#define W 230
#define H 36

struct switcher {
    lv_obj_t *pill, *name, *prev, *next;
    lv_obj_t *dots[SWITCHER_MAX];
    const char *const *names;
    bool enabled[SWITCHER_MAX];
    int count;
    int current;
    switcher_cb_t cb;
};

int switcher_next(const switcher_t *sw, int from, int dir)
{
    for (int i = from + dir; i >= 0 && i < sw->count; i += dir) {
        if (sw->enabled[i]) return i;
    }
    return -1;
}

static void go(switcher_t *sw, int dir)
{
    int i = switcher_next(sw, sw->current, dir);
    if (i >= 0) sw->cb(i, dir);
}

static void on_prev(lv_event_t *e) { go(lv_event_get_user_data(e), -1); }
static void on_next(lv_event_t *e) { go(lv_event_get_user_data(e), 1); }

static void on_gesture(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) go(lv_event_get_user_data(e), 1);
    else if (dir == LV_DIR_RIGHT) go(lv_event_get_user_data(e), -1);
}

static void set_arrow(lv_obj_t *btn, bool on)
{
    lv_obj_set_style_opa(lv_obj_get_child(btn, 0), on ? LV_OPA_COVER : LV_OPA_20, 0);
    if (on) lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
}

void switcher_select(switcher_t *sw, int index)
{
    if (index < 0 || index >= sw->count) return;
    sw->current = index;
    lv_label_set_text(sw->name, sw->names[index]);
    set_arrow(sw->prev, switcher_next(sw, index, -1) >= 0);
    set_arrow(sw->next, switcher_next(sw, index, 1) >= 0);
    for (int i = 0; i < sw->count; i++) {
        if (sw->enabled[i]) lv_obj_remove_flag(sw->dots[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(sw->dots[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(sw->dots[i], i == index ? COLOR_ACCENT : COLOR_CARD, 0);
    }
}

void switcher_set_enabled(switcher_t *sw, int index, bool enabled)
{
    if (index >= 0 && index < sw->count) sw->enabled[index] = enabled;
}

bool switcher_is_enabled(const switcher_t *sw, int index)
{
    return index >= 0 && index < sw->count && sw->enabled[index];
}

static lv_obj_t *arrow(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, lv_align_t align, switcher_t *sw)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, H, H);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, align, 0, 0);
    lv_obj_t *l = ui_label(b, &ui_font_16, COLOR_TEXT, 0);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, sw);
    return b;
}

switcher_t *switcher_create(lv_obj_t *parent, int y, const char *const *names, int count, switcher_cb_t cb)
{
    switcher_t *sw = calloc(1, sizeof(*sw));
    sw->names = names;
    sw->count = count < SWITCHER_MAX ? count : SWITCHER_MAX;
    sw->cb = cb;
    for (int i = 0; i < sw->count; i++) sw->enabled[i] = true;

    // Clickable so it receives swipes itself; gestures don't bubble past it.
    sw->pill = lv_obj_create(parent);
    lv_obj_remove_style_all(sw->pill);
    lv_obj_set_size(sw->pill, W, H);
    lv_obj_align(sw->pill, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_radius(sw->pill, H / 2, 0);
    lv_obj_set_style_bg_color(sw->pill, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(sw->pill, LV_OPA_COVER, 0);
    lv_obj_add_flag(sw->pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sw->pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(sw->pill, on_gesture, LV_EVENT_GESTURE, sw);

    sw->prev = arrow(sw->pill, LV_SYMBOL_LEFT, on_prev, LV_ALIGN_LEFT_MID, sw);
    sw->next = arrow(sw->pill, LV_SYMBOL_RIGHT, on_next, LV_ALIGN_RIGHT_MID, sw);
    sw->name = ui_label(sw->pill, &ui_font_14, COLOR_ACCENT, W - 2 * H - 8);
    lv_label_set_long_mode(sw->name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_center(sw->name);

    lv_obj_t *dots = lv_obj_create(parent);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, 8);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, 8, 0);
    lv_obj_align(dots, LV_ALIGN_TOP_MID, 0, y + H + 8);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < sw->count; i++) {
        sw->dots[i] = lv_obj_create(dots);
        lv_obj_remove_style_all(sw->dots[i]);
        lv_obj_set_size(sw->dots[i], 8, 8);
        lv_obj_set_style_radius(sw->dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(sw->dots[i], LV_OPA_COVER, 0);
    }
    switcher_select(sw, 0);
    return sw;
}

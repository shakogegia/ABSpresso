// Home: shelves of books, one at a time. Swipe left/right on the covers to browse a shelf; change
// shelf with the switcher at the bottom ("<  Recently Added  >", with a dot per shelf): tap its
// arrows or swipe left/right on it. Swipes on the switcher stay there (they don't bubble to the
// page), so they never also move the carousel.

#include "carousel.h"
#include "cover.h"
#include "ui_priv.h"

#define CAROUSEL_Y   (PAGE_TOP + 2)
#define SWITCHER_Y   (CAROUSEL_Y + COVER_THUMB_SIZE + 54)
#define SWITCHER_W   230
#define SWITCHER_H   36

typedef struct {
    const char *name;
    const book_list_t *books;
    int pos;  // remembered position within the shelf
} row_t;

static row_t s_rows[] = {
    {"Continue Listening", &g_continue, 0},
    {"Recently Added", &g_recent, 0},
    {"Downloaded", &g_downloaded, 0},
};
#define ROW_COUNT (int)(sizeof(s_rows) / sizeof(s_rows[0]))

static lv_obj_t *s_group, *s_name, *s_prev, *s_next, *s_empty;
static lv_obj_t *s_dots[ROW_COUNT];
static carousel_t *s_carousel;
static int s_row = -1;

static bool row_has_books(int r)
{
    return r >= 0 && r < ROW_COUNT && s_rows[r].books->count > 0;
}

static int next_row(int from, int dir)
{
    for (int r = from + dir; r >= 0 && r < ROW_COUNT; r += dir) {
        if (row_has_books(r)) return r;
    }
    return -1;
}

static void group_set_x(void *obj, int32_t x)
{
    lv_obj_set_x(obj, x);
}

static void set_enabled(lv_obj_t *btn, bool on)
{
    lv_obj_set_style_opa(lv_obj_get_child(btn, 0), on ? LV_OPA_COVER : LV_OPA_20, 0);
    if (on) lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    else lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
}

static void show_row(int r, int dir)
{
    if (s_row >= 0) {
        s_rows[s_row].pos = carousel_pos(s_carousel);
    }
    s_row = r;
    if (r < 0) {
        lv_obj_add_flag(s_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_name, s_rows[r].name);
    carousel_set_items(s_carousel, s_rows[r].books->idx, s_rows[r].books->count, s_rows[r].pos);
    set_enabled(s_prev, next_row(r, -1) >= 0);
    set_enabled(s_next, next_row(r, 1) >= 0);
    // One dot per non-empty shelf; the current one highlighted.
    for (int i = 0; i < ROW_COUNT; i++) {
        if (row_has_books(i)) lv_obj_remove_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_dots[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(s_dots[i], i == r ? COLOR_ACCENT : COLOR_CARD, 0);
    }

    if (dir) {
        // Slide the new shelf in from the side it came from.
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_group);
        lv_anim_set_exec_cb(&a, group_set_x);
        lv_anim_set_values(&a, dir * 40, 0);
        lv_anim_set_duration(&a, 200);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

static void change_shelf(int dir)
{
    int r = next_row(s_row, dir);
    if (r >= 0) show_row(r, dir);
}

static void on_prev(lv_event_t *e) { change_shelf(-1); }
static void on_next(lv_event_t *e) { change_shelf(1); }

static void on_switcher_gesture(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) change_shelf(1);
    else if (dir == LV_DIR_RIGHT) change_shelf(-1);
}

// Swipes anywhere else on the page browse the current shelf.
static void on_page_gesture(lv_event_t *e)
{
    if (s_row < 0) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) carousel_step(s_carousel, 1);
    else if (dir == LV_DIR_RIGHT) carousel_step(s_carousel, -1);
}

static lv_obj_t *arrow(lv_obj_t *parent, const char *sym, lv_event_cb_t cb, lv_align_t align)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, SWITCHER_H, SWITCHER_H);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(b, COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_align(b, align, 0, 0);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_16, COLOR_TEXT, 0);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

void home_build(lv_obj_t *page)
{
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);  // so swipes on empty space register
    lv_obj_add_event_cb(page, on_page_gesture, LV_EVENT_GESTURE, NULL);

    s_group = lv_obj_create(page);
    lv_obj_remove_style_all(s_group);
    lv_obj_set_size(s_group, 360, 360);
    lv_obj_remove_flag(s_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_group, LV_OBJ_FLAG_CLICKABLE);

    // No ring on Home, so the carousel can use the full width.
    const carousel_cfg_t cfg = {.y = CAROUSEL_Y, .x = 0, .width = 360, .title_w = 240, .sub_w = 230};
    s_carousel = carousel_create(s_group, &cfg);

    // Shelf switcher. Clickable so it receives swipes itself; gestures don't bubble past it.
    lv_obj_t *sw = lv_obj_create(s_group);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, SWITCHER_W, SWITCHER_H);
    lv_obj_align(sw, LV_ALIGN_TOP_MID, 0, SWITCHER_Y);
    lv_obj_set_style_radius(sw, SWITCHER_H / 2, 0);
    lv_obj_set_style_bg_color(sw, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(sw, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(sw, on_switcher_gesture, LV_EVENT_GESTURE, NULL);

    s_prev = arrow(sw, LV_SYMBOL_LEFT, on_prev, LV_ALIGN_LEFT_MID);
    s_next = arrow(sw, LV_SYMBOL_RIGHT, on_next, LV_ALIGN_RIGHT_MID);
    s_name = ui_label(sw, &lv_font_montserrat_14, COLOR_ACCENT, SWITCHER_W - 2 * SWITCHER_H - 8);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_center(s_name);

    lv_obj_t *dots = lv_obj_create(s_group);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, 8);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, 8, 0);
    lv_obj_align(dots, LV_ALIGN_TOP_MID, 0, SWITCHER_Y + SWITCHER_H + 8);
    lv_obj_remove_flag(dots, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < ROW_COUNT; i++) {
        s_dots[i] = lv_obj_create(dots);
        lv_obj_remove_style_all(s_dots[i]);
        lv_obj_set_size(s_dots[i], 8, 8);
        lv_obj_set_style_radius(s_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s_dots[i], LV_OPA_COVER, 0);
    }

    s_empty = ui_label(page, &lv_font_montserrat_16, COLOR_MUTED, 240);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_empty, "Nothing here yet.\nBrowse the " LV_SYMBOL_LIST " library to start a book.");
    lv_obj_center(s_empty);
    lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_group, LV_OBJ_FLAG_HIDDEN);
}

void home_set_books(void)
{
    // New data: start each shelf from its first book, on the first non-empty shelf.
    for (int i = 0; i < ROW_COUNT; i++) s_rows[i].pos = 0;
    s_row = -1;
    show_row(next_row(-1, 1), 0);
}

void home_lists_changed(void)
{
    // Stay on the current shelf if it still has books (keeping the position); else find another.
    int r = row_has_books(s_row) ? s_row : next_row(-1, 1);
    if (s_row >= 0) s_rows[s_row].pos = carousel_pos(s_carousel);
    s_row = -1;
    show_row(r, 0);
}

void home_refresh(void)
{
    if (s_row >= 0) carousel_refresh(s_carousel);
}

#ifdef UI_CAPTURE
void home_debug_next_row(void)
{
    change_shelf(1);
}
#endif

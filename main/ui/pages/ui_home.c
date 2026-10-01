// Home: shelves of books, one at a time. Swipe left/right on the covers to browse a shelf; change
// shelf with the switcher pill at the bottom (switcher.c).

#include "carousel.h"
#include "switcher.h"
#include "ui_priv.h"

typedef struct {
    const book_list_t *books;
    int pos;  // remembered position within the shelf
} row_t;

static const char *const s_names[] = {"Continue Listening", "Recently Added", "Downloaded"};
static row_t s_rows[] = {{&g_continue, 0}, {&g_recent, 0}, {&g_downloaded, 0}};
#define ROW_COUNT (int)(sizeof(s_rows) / sizeof(s_rows[0]))

static lv_obj_t *s_group, *s_empty;
static carousel_t *s_carousel;
static switcher_t *s_switcher;
static int s_row = -1;

static bool row_has_books(int r)
{
    return r >= 0 && r < ROW_COUNT && s_rows[r].books->count > 0;
}

static int first_row(void)
{
    for (int r = 0; r < ROW_COUNT; r++) {
        if (row_has_books(r)) return r;
    }
    return -1;
}

static void group_set_x(void *obj, int32_t x)
{
    lv_obj_set_x(obj, x);
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

    carousel_set_items(s_carousel, s_rows[r].books->idx, s_rows[r].books->count, s_rows[r].pos);
    for (int i = 0; i < ROW_COUNT; i++) switcher_set_enabled(s_switcher, i, row_has_books(i));
    switcher_select(s_switcher, r);

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

static void on_switch(int index, int dir)
{
    show_row(index, dir);
}

// Swipes anywhere else on the page browse the current shelf.
static void on_page_gesture(lv_event_t *e)
{
    if (s_row < 0) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) carousel_step(s_carousel, 1);
    else if (dir == LV_DIR_RIGHT) carousel_step(s_carousel, -1);
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
    const carousel_cfg_t cfg = {.y = PAGE_CAROUSEL_Y, .x = 0, .width = 360, .title_w = 240, .sub_w = 230};
    s_carousel = carousel_create(s_group, &cfg);
    s_switcher = switcher_create(s_group, PAGE_SWITCHER_Y, s_names, ROW_COUNT, on_switch);

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
    show_row(first_row(), 0);
}

void home_lists_changed(void)
{
    // Stay on the current shelf if it still has books (keeping the position); else find another.
    int r = row_has_books(s_row) ? s_row : first_row();
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
    int r = switcher_next(s_switcher, s_row, 1);
    if (r >= 0) show_row(r, 1);
}
#endif

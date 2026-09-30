// Home: shelves of books, one row at a time. Swipe left/right within a row, up/down between rows.

#include "carousel.h"
#include "cover.h"
#include "ui_priv.h"

#define ROW_TITLE_Y  PAGE_TOP
#define CAROUSEL_Y   (PAGE_TOP + 22)

typedef struct {
    const char *name;
    const book_list_t *books;
    int pos;  // remembered position within the row
} row_t;

static row_t s_rows[] = {
    {"Continue Listening", &g_continue, 0},
    {"Recently Added", &g_recent, 0},
    {"Downloaded", &g_downloaded, 0},
};
#define ROW_COUNT (int)(sizeof(s_rows) / sizeof(s_rows[0]))

static lv_obj_t *s_page, *s_group, *s_title, *s_hint, *s_empty;
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

static void group_set_y(void *obj, int32_t y)
{
    lv_obj_set_y(obj, y);
}

static void show_row(int r, int dir)
{
    if (s_row >= 0) {
        s_rows[s_row].pos = carousel_pos(s_carousel);
    }
    s_row = r;
    const bool any = r >= 0;
    if (any) {
        lv_obj_remove_flag(s_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_group, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(s_title, s_rows[r].name);
    carousel_set_items(s_carousel, s_rows[r].books->idx, s_rows[r].books->count, s_rows[r].pos);

    // Point at the neighbouring shelf so the vertical swipe is discoverable.
    int below = next_row(r, 1), above = next_row(r, -1);
    static char hint[48];
    if (below >= 0) {
        lv_snprintf(hint, sizeof(hint), LV_SYMBOL_DOWN "  %s", s_rows[below].name);
    } else if (above >= 0) {
        lv_snprintf(hint, sizeof(hint), LV_SYMBOL_UP "  %s", s_rows[above].name);
    } else {
        hint[0] = 0;
    }
    lv_label_set_text(s_hint, hint);
    lv_obj_t *pill = lv_obj_get_parent(s_hint);
    if (hint[0]) lv_obj_remove_flag(pill, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(pill, LV_OBJ_FLAG_HIDDEN);

    if (dir) {
        // Slide the new shelf in from the direction of travel.
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_group);
        lv_anim_set_exec_cb(&a, group_set_y);
        lv_anim_set_values(&a, dir * 50, 0);
        lv_anim_set_duration(&a, 200);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

static void on_gesture(lv_event_t *e)
{
    if (s_row < 0) return;
    switch (lv_indev_get_gesture_dir(lv_indev_active())) {
    case LV_DIR_LEFT:  carousel_step(s_carousel, 1); break;
    case LV_DIR_RIGHT: carousel_step(s_carousel, -1); break;
    case LV_DIR_TOP: {
        int r = next_row(s_row, 1);
        if (r >= 0) show_row(r, 1);
        break;
    }
    case LV_DIR_BOTTOM: {
        int r = next_row(s_row, -1);
        if (r >= 0) show_row(r, -1);
        break;
    }
    default: break;
    }
}

static void on_hint_clicked(lv_event_t *e)
{
    int r = next_row(s_row, 1);
    if (r >= 0) show_row(r, 1);
    else if ((r = next_row(s_row, -1)) >= 0) show_row(r, -1);
}

void home_build(lv_obj_t *page)
{
    s_page = page;
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);  // so swipes on empty space register
    lv_obj_add_event_cb(page, on_gesture, LV_EVENT_GESTURE, NULL);

    s_group = lv_obj_create(page);
    lv_obj_remove_style_all(s_group);
    lv_obj_set_size(s_group, 360, 360);
    lv_obj_remove_flag(s_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_group, LV_OBJ_FLAG_CLICKABLE);

    s_title = ui_label(s_group, &lv_font_montserrat_16, COLOR_ACCENT, 260);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, ROW_TITLE_Y);

    // No ring on Home, so the carousel can use the full width.
    const carousel_cfg_t cfg = {.y = CAROUSEL_Y, .x = 0, .width = 360, .title_w = 240, .sub_w = 230};
    s_carousel = carousel_create(s_group, &cfg);

    // Names the neighbouring shelf; tapping it moves there too (swiping isn't discoverable).
    lv_obj_t *pill = lv_button_create(s_group);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, 28);
    lv_obj_set_style_radius(pill, 14, 0);
    lv_obj_set_style_bg_color(pill, COLOR_CARD, 0);
    lv_obj_set_style_bg_color(pill, COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(pill, 0, 0);
    lv_obj_set_style_pad_hor(pill, 14, 0);
    lv_obj_set_style_pad_ver(pill, 0, 0);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, CAROUSEL_Y + COVER_THUMB_SIZE + 54);
    lv_obj_add_event_cb(pill, on_hint_clicked, LV_EVENT_CLICKED, NULL);
    s_hint = ui_label(pill, &lv_font_montserrat_14, COLOR_MUTED, 0);
    lv_obj_center(s_hint);


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
    on_hint_clicked(NULL);
}
#endif

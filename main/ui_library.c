// Library: every book, as an A-Z list, a cover carousel, or grouped by author. A ring segment on
// the right edge scrubs through whichever view is showing (like a scrollbar), with the current
// letter shown large while dragging.

#include <stdio.h>
#include "carousel.h"
#include "nvs.h"
#include "ui.h"
#include "ui_priv.h"

#define VIEW_Y (PAGE_TOP + 44)

typedef enum {
    VIEW_LIST,
    VIEW_COVERS,
    VIEW_AUTHORS,
    VIEW_AUTHOR_BOOKS,  // drilled into one author (reached from VIEW_AUTHORS)
} view_t;

static lv_obj_t *s_tabs, *s_list, *s_authors, *s_author_books, *s_ring, *s_bubble, *s_bubble_label;
static carousel_t *s_carousel;
static view_t s_view;
static int s_author = -1;
static bool s_scrubbing;

/* ---------- helpers ---------- */

static void set_hidden(lv_obj_t *o, bool hidden)
{
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *make_list(lv_obj_t *page)
{
    // Sized and nudged left to stay clear of the A-Z ring on the right (see ui_priv.h).
    lv_obj_t *l = lv_list_create(page);
    lv_obj_set_size(l, 236, 172);
    lv_obj_align(l, LV_ALIGN_TOP_MID, -8, VIEW_Y);
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_pad_all(l, 0, 0);
    lv_obj_set_style_pad_row(l, 6, 0);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
    return l;
}

// Big letter for the scrub bubble: the first character, upper-cased, or '#' for non-letters.
static void set_bubble_letter(const char *s)
{
    char c = s ? s[0] : 0;
    if (c >= 'a' && c <= 'z') c -= 32;
    char txt[2] = {(c >= 'A' && c <= 'Z') ? c : '#', 0};
    lv_label_set_text(s_bubble_label, txt);
}

// Rows in the author-books list that come before the first book (back button and heading).
#define AUTHOR_BOOKS_HEADER 2

static void scroll_list_to(lv_obj_t *list, int child)
{
    lv_obj_t *row = lv_obj_get_child(list, child);
    if (row) lv_obj_scroll_to_y(list, lv_obj_get_y(row), LV_ANIM_OFF);
}

/* ---------- views ---------- */

static void sync_ring(void)
{
    lv_obj_t *l = s_view == VIEW_LIST      ? s_list
                  : s_view == VIEW_AUTHORS ? s_authors
                  : s_view == VIEW_AUTHOR_BOOKS ? s_author_books
                                                : NULL;
    int value = 0;
    if (l) {
        int32_t top = lv_obj_get_scroll_y(l), rest = lv_obj_get_scroll_bottom(l);
        if (top + rest > 0) value = (int)(1000LL * top / (top + rest));
    } else if (carousel_count(s_carousel) > 1) {
        value = 1000 * carousel_pos(s_carousel) / (carousel_count(s_carousel) - 1);
    }
    lv_arc_set_value(s_ring, value);
}

static void apply_view(void)
{
    set_hidden(s_list, s_view != VIEW_LIST);
    set_hidden(s_authors, s_view != VIEW_AUTHORS);
    set_hidden(s_author_books, s_view != VIEW_AUTHOR_BOOKS);
    carousel_set_hidden(s_carousel, s_view != VIEW_COVERS);
    uint32_t tab = s_view == VIEW_AUTHOR_BOOKS ? VIEW_AUTHORS : s_view;
    lv_buttonmatrix_set_button_ctrl(s_tabs, tab, LV_BUTTONMATRIX_CTRL_CHECKED);
    lv_obj_update_layout(s_tabs);
    sync_ring();
}

static void save_view(void)
{
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "libview", s_view == VIEW_AUTHOR_BOOKS ? VIEW_AUTHORS : s_view);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void on_tab(lv_event_t *e)
{
    uint32_t id = lv_buttonmatrix_get_selected_button(s_tabs);
    if (id > VIEW_AUTHORS) return;
    // Tapping Authors while inside an author goes back to the author list.
    s_view = (view_t)id;
    apply_view();
    save_view();
}

static void on_back_to_authors(lv_event_t *e)
{
    s_view = VIEW_AUTHORS;
    apply_view();
}

static void show_author(int a)
{
    s_author = a;
    lv_obj_clean(s_author_books);

    lv_obj_t *back = lv_list_add_button(s_author_books, LV_SYMBOL_LEFT, "All authors");
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(back, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_add_event_cb(back, on_back_to_authors, LV_EVENT_CLICKED, NULL);

    lv_obj_t *head = lv_list_add_text(s_author_books, g_authors[a].name);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(head, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(head, &lv_font_montserrat_20, 0);

    for (int i = 0; i < g_authors[a].books.count; i++) {
        ui_add_book_row(s_author_books, g_authors[a].books.idx[i]);
    }
    lv_obj_scroll_to_y(s_author_books, 0, LV_ANIM_OFF);
    s_view = VIEW_AUTHOR_BOOKS;
    apply_view();
}

static void on_author_clicked(lv_event_t *e)
{
    show_author((int)(intptr_t)lv_event_get_user_data(e));
}

static void on_refresh(lv_event_t *e)
{
    ui_request_refresh();
    ui_show_message("Refreshing...");
}

/* ---------- gestures ---------- */

static void on_gesture(lv_event_t *e)
{
    if (s_view != VIEW_COVERS) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) carousel_step(s_carousel, 1);
    else if (dir == LV_DIR_RIGHT) carousel_step(s_carousel, -1);
}

/* ---------- A-Z ring ---------- */

static void scrub_to(int value)
{
    const float frac = value / 1000.0f;
    switch (s_view) {
    case VIEW_LIST:
    case VIEW_COVERS: {
        if (!g_alpha.count) return;
        int i = (int)(frac * (g_alpha.count - 1) + 0.5f);
        set_bubble_letter(g_books[g_alpha.idx[i]].sort_title);
        if (s_view == VIEW_LIST) scroll_list_to(s_list, i);
        else carousel_jump(s_carousel, i);
        break;
    }
    case VIEW_AUTHORS: {
        if (!g_author_count) return;
        int i = (int)(frac * (g_author_count - 1) + 0.5f);
        set_bubble_letter(g_authors[i].sort_key);
        scroll_list_to(s_authors, i);
        break;
    }
    case VIEW_AUTHOR_BOOKS: {
        const book_list_t *bl = &g_authors[s_author].books;
        if (!bl->count) return;
        int i = (int)(frac * (bl->count - 1) + 0.5f);
        set_bubble_letter(g_books[bl->idx[i]].sort_title);
        scroll_list_to(s_author_books, i + AUTHOR_BOOKS_HEADER);
        break;
    }
    }
}

static void on_ring(lv_event_t *e)
{
    switch (lv_event_get_code(e)) {
    case LV_EVENT_PRESSED:
        s_scrubbing = true;
        lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        scrub_to(lv_arc_get_value(s_ring));
        break;
    case LV_EVENT_VALUE_CHANGED:
        if (s_scrubbing) scrub_to(lv_arc_get_value(s_ring));
        break;
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST:
        s_scrubbing = false;
        lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        break;
    default:
        break;
    }
}

// Keep the ring's knob in step with ordinary scrolling.
static void on_list_scroll(lv_event_t *e)
{
    if (s_scrubbing) return;
    lv_obj_t *l = lv_event_get_target(e);
    int32_t top = lv_obj_get_scroll_y(l), rest = lv_obj_get_scroll_bottom(l);
    if (top + rest > 0) lv_arc_set_value(s_ring, (int)(1000LL * top / (top + rest)));
}

/* ---------- build ---------- */

void library_build(lv_obj_t *page)
{
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(page, on_gesture, LV_EVENT_GESTURE, NULL);

    static const char *tabs[] = {"List", "Covers", "Authors", ""};
    s_tabs = lv_buttonmatrix_create(page);
    lv_buttonmatrix_set_map(s_tabs, tabs);
    lv_buttonmatrix_set_button_ctrl_all(s_tabs, LV_BUTTONMATRIX_CTRL_CHECKABLE);
    lv_buttonmatrix_set_one_checked(s_tabs, true);
    lv_obj_set_size(s_tabs, 212, 36);
    lv_obj_align(s_tabs, LV_ALIGN_TOP_MID, 16, PAGE_TOP);
    lv_obj_set_style_bg_opa(s_tabs, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_tabs, 0, 0);
    lv_obj_set_style_pad_all(s_tabs, 0, 0);
    lv_obj_set_style_pad_column(s_tabs, 4, 0);
    lv_obj_set_style_bg_color(s_tabs, COLOR_CARD, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_tabs, COLOR_ACCENT, LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(s_tabs, COLOR_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_tabs, lv_color_black(), LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_font(s_tabs, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_radius(s_tabs, 18, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_tabs, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(s_tabs, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(s_tabs, on_tab, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *refresh = ui_round_button(page, 36, LV_SYMBOL_REFRESH, &lv_font_montserrat_14, on_refresh, NULL);
    // Left of the tabs: the A-Z ring runs down the right edge.
    lv_obj_align(refresh, LV_ALIGN_TOP_MID, -114, PAGE_TOP);

    s_list = make_list(page);
    s_authors = make_list(page);
    s_author_books = make_list(page);
    lv_obj_add_event_cb(s_list, on_list_scroll, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_authors, on_list_scroll, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_author_books, on_list_scroll, LV_EVENT_SCROLL, NULL);

    const carousel_cfg_t cfg = {.y = VIEW_Y + 2, .x = -12, .width = 280, .title_w = 230, .sub_w = 200};
    s_carousel = carousel_create(page, &cfg);

    // Right-edge ring segment from 1 o'clock to 5 o'clock: top = A, bottom = Z.
    s_ring = lv_arc_create(page);
    lv_obj_set_size(s_ring, 348, 348);
    lv_obj_center(s_ring);
    lv_arc_set_bg_angles(s_ring, 300, 60);
    lv_arc_set_range(s_ring, 0, 1000);
    lv_arc_set_value(s_ring, 0);
    lv_obj_set_style_arc_width(s_ring, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_ring, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_ring, COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_ring, 4, LV_PART_KNOB);
    // Only the ring itself should take touches, not the square it sits in.
    lv_obj_add_flag(s_ring, LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_add_event_cb(s_ring, on_ring, LV_EVENT_ALL, NULL);

    s_bubble = lv_obj_create(page);
    lv_obj_set_size(s_bubble, 84, 84);
    lv_obj_center(s_bubble);
    lv_obj_set_style_radius(s_bubble, 20, 0);
    lv_obj_set_style_bg_color(s_bubble, COLOR_CARD, 0);
    lv_obj_set_style_border_color(s_bubble, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(s_bubble, 2, 0);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_bubble_label = ui_label(s_bubble, &lv_font_montserrat_40, COLOR_ACCENT, 0);
    lv_obj_center(s_bubble_label);
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);

    uint8_t v = VIEW_LIST;
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "libview", &v);
        nvs_close(h);
    }
    s_view = v <= VIEW_AUTHORS ? (view_t)v : VIEW_LIST;
    apply_view();
}

void library_set_books(void)
{
    lv_obj_clean(s_list);
    for (int i = 0; i < g_alpha.count; i++) {
        ui_add_book_row(s_list, g_alpha.idx[i]);
    }

    lv_obj_clean(s_authors);
    for (int a = 0; a < g_author_count; a++) {
        char count[24];
        snprintf(count, sizeof(count), "%d book%s", g_authors[a].books.count, g_authors[a].books.count == 1 ? "" : "s");
        lv_obj_t *btn = lv_list_add_button(s_authors, NULL, NULL);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
        lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 10, 0);
        lv_obj_set_style_pad_row(btn, 2, 0);
        lv_obj_t *n = lv_label_create(btn);
        lv_label_set_text(n, g_authors[a].name);
        ui_one_line(n, &lv_font_montserrat_16);
        lv_obj_set_style_text_color(n, COLOR_TEXT, 0);
        lv_obj_t *c = lv_label_create(btn);
        lv_label_set_text(c, count);
        lv_obj_set_style_text_font(c, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(c, COLOR_MUTED, 0);
        lv_obj_add_event_cb(btn, on_author_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)a);
    }

    // Author indexes changed; drop out of a drilled-in author.
    lv_obj_clean(s_author_books);
    if (s_view == VIEW_AUTHOR_BOOKS) s_view = VIEW_AUTHORS;
    s_author = -1;

    carousel_set_items(s_carousel, g_alpha.idx, g_alpha.count, carousel_pos(s_carousel));
    apply_view();
    lv_arc_set_value(s_ring, 0);
}

void library_refresh(void)
{
    if (s_view == VIEW_COVERS) {
        carousel_refresh(s_carousel);
        int n = carousel_count(s_carousel);
        if (!s_scrubbing && n > 1) lv_arc_set_value(s_ring, 1000 * carousel_pos(s_carousel) / (n - 1));
    }
}


#ifdef UI_CAPTURE
void library_debug_view(int view)
{
    s_view = (view_t)view;
    apply_view();
}

void library_debug_step(int delta)
{
    carousel_step(s_carousel, delta);
}

// Shows the A-Z ring mid-drag, as if the user were scrubbing.
void library_debug_scrub(int value, bool active)
{
    s_scrubbing = active;
    lv_arc_set_value(s_ring, value);
    if (active) {
        lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        scrub_to(value);
    } else {
        lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    }
}
#endif

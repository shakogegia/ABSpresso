// UI shell for the 360x360 round panel: one screen with three pages (Home, Library, Now Playing)
// and a dock of page buttons across the top. Swipes belong to the pages (carousels and rows), so
// page switching is done from the dock. Player state and covers are polled on an LVGL timer, so
// nothing outside the LVGL task touches widgets.

#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"
#include "player.h"
#include "ui_priv.h"

#define RECENT_MAX 30
#define STALE_MS   (5 * 60 * 1000)  // reload the library when Home is opened after this long

const abs_book_t *g_books;
int g_book_count;
book_list_t g_alpha, g_continue, g_recent;
author_t *g_authors;
int g_author_count;

static volatile bool s_refresh_requested;
static uint32_t s_books_loaded_at;

static lv_obj_t *s_scr, *s_msg;
static lv_obj_t *s_pages[PAGE_COUNT];
static lv_obj_t *s_dock[PAGE_COUNT];
static ui_page_t s_page;

/* ---------- helpers ---------- */

lv_obj_t *ui_round_button(lv_obj_t *parent, int size, const char *text, const lv_font_t *font, lv_event_cb_t cb,
                          void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, COLOR_CARD, 0);
    lv_obj_set_style_bg_color(b, COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, COLOR_TEXT, 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int width)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    if (width > 0) {
        lv_obj_set_width(l, width);
    }
    lv_label_set_text(l, "");
    return l;
}

void ui_one_line(lv_obj_t *label, const lv_font_t *font)
{
    // "..." truncation only happens when the height is fixed; otherwise the label wraps.
    lv_obj_set_style_text_font(label, font, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_height(label, lv_font_get_line_height(font));
}

lv_obj_t *ui_page_container(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 360, 360);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

void ui_book_subtitle(const abs_book_t *b, char *buf, size_t len)
{
    if (b->finished) {
        snprintf(buf, len, "%s  " LV_SYMBOL_OK, b->author);
    } else if (b->current_time > 0 && b->progress < 0.01f) {
        snprintf(buf, len, "%s  " LV_SYMBOL_BULLET " <1%%", b->author);
    } else if (b->current_time > 0) {
        snprintf(buf, len, "%s  " LV_SYMBOL_BULLET " %d%%", b->author, (int)(b->progress * 100));
    } else {
        snprintf(buf, len, "%s", b->author);
    }
}

bool ui_book_in_progress(const abs_book_t *b)
{
    return b->current_time > 0 && !b->finished;
}

static void on_book_row_clicked(lv_event_t *e)
{
    ui_open_book((int)(intptr_t)lv_event_get_user_data(e));
}

lv_obj_t *ui_add_book_row(lv_obj_t *list, int book_index)
{
    const abs_book_t *b = &g_books[book_index];
    lv_obj_t *btn = lv_list_add_button(list, NULL, NULL);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
    lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 10, 0);
    lv_obj_set_style_pad_row(btn, 2, 0);

    lv_obj_t *t = lv_label_create(btn);
    lv_label_set_text(t, b->title);
    ui_one_line(t, &lv_font_montserrat_16);
    lv_obj_set_style_text_color(t, COLOR_TEXT, 0);

    char sub[160];
    ui_book_subtitle(b, sub, sizeof(sub));
    lv_obj_t *a = lv_label_create(btn);
    lv_label_set_text(a, sub);
    ui_one_line(a, &lv_font_montserrat_14);
    lv_obj_set_style_text_color(a, ui_book_in_progress(b) ? COLOR_ACCENT : COLOR_MUTED, 0);

    lv_obj_add_event_cb(btn, on_book_row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)book_index);
    return btn;
}

void ui_open_book(int book_index)
{
    if (book_index < 0 || book_index >= g_book_count) return;
    player_open(&g_books[book_index]);
    playing_prepare(book_index);
    ui_show_page(PAGE_PLAYER);
}

/* ---------- pages and dock ---------- */

static void dock_highlight(void)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        bool on = i == (int)s_page;
        lv_obj_set_style_bg_color(s_dock[i], on ? COLOR_ACCENT : COLOR_CARD, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_dock[i], 0), on ? lv_color_black() : COLOR_TEXT, 0);
    }
}

void ui_show_page(ui_page_t page)
{
    s_page = page;
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (i == (int)page) lv_obj_remove_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    dock_highlight();
    if (page == PAGE_HOME && g_book_count && lv_tick_elaps(s_books_loaded_at) > STALE_MS) {
        s_refresh_requested = true;  // pick up progress changes from this and other devices
    }
}

static void on_dock(lv_event_t *e)
{
    ui_show_page((ui_page_t)(intptr_t)lv_event_get_user_data(e));
}

static void refresh_timer(lv_timer_t *t)
{
    switch (s_page) {
    case PAGE_HOME:    home_refresh(); break;
    case PAGE_LIBRARY: library_refresh(); break;
    case PAGE_PLAYER:  playing_refresh(); break;
    default: break;
    }
    // A subtle cue on the dock when something is playing.
    player_status_t st;
    player_get_status(&st);
    bool active = st.state == PLAYER_PLAYING || st.state == PLAYER_BUFFERING || st.state == PLAYER_LOADING;
    lv_obj_set_style_border_width(s_dock[PAGE_PLAYER], active && s_page != PAGE_PLAYER ? 2 : 0, 0);
}

void ui_init(void)
{
    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, COLOR_BG, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    void (*builders[PAGE_COUNT])(lv_obj_t *) = {home_build, library_build, playing_build};
    for (int i = 0; i < PAGE_COUNT; i++) {
        s_pages[i] = ui_page_container(s_scr);
        builders[i](s_pages[i]);
    }

    // Dock last, so it is above the pages.
    static const char *icons[PAGE_COUNT] = {LV_SYMBOL_HOME, LV_SYMBOL_LIST, LV_SYMBOL_AUDIO};
    for (int i = 0; i < PAGE_COUNT; i++) {
        s_dock[i] = ui_round_button(s_scr, 38, icons[i], &lv_font_montserrat_16, on_dock, (void *)(intptr_t)i);
        lv_obj_align(s_dock[i], LV_ALIGN_TOP_MID, (i - 1) * 46, DOCK_Y);
        lv_obj_set_style_border_color(s_dock[i], COLOR_ACCENT, 0);
    }

    s_msg = ui_label(s_scr, &lv_font_montserrat_16, COLOR_MUTED, 240);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_msg);

    ui_show_page(PAGE_HOME);
    lv_timer_create(refresh_timer, 250, NULL);
}

void ui_show_message(const char *msg)
{
    lv_label_set_text(s_msg, msg ? msg : "");
    if (msg && msg[0]) {
        lv_obj_remove_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_msg);
    } else {
        lv_obj_add_flag(s_msg, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------- derived book lists ---------- */

static int cmp_last_update(const void *a, const void *b)
{
    double x = g_books[*(const int *)a].last_update, y = g_books[*(const int *)b].last_update;
    return x < y ? 1 : (x > y ? -1 : 0);
}

static int cmp_added(const void *a, const void *b)
{
    double x = g_books[*(const int *)a].added_at, y = g_books[*(const int *)b].added_at;
    return x < y ? 1 : (x > y ? -1 : 0);
}

static int cmp_author(const void *a, const void *b)
{
    const author_t *x = a, *y = b;
    int r = strcasecmp(x->sort_key, y->sort_key);
    return r ? r : strcasecmp(x->name, y->name);
}

static void *ps_alloc(size_t n)
{
    return heap_caps_calloc(1, n ? n : 1, MALLOC_CAP_SPIRAM);
}

static char *ps_strdup(const char *s, size_t n)
{
    char *d = ps_alloc(n + 1);
    memcpy(d, s, n);
    return d;
}

static void free_lists(void)
{
    free(g_alpha.idx);
    free(g_continue.idx);
    free(g_recent.idx);
    for (int i = 0; i < g_author_count; i++) {
        free(g_authors[i].name);
        free(g_authors[i].sort_key);
        free(g_authors[i].books.idx);
    }
    free(g_authors);
    memset(&g_alpha, 0, sizeof(g_alpha));
    memset(&g_continue, 0, sizeof(g_continue));
    memset(&g_recent, 0, sizeof(g_recent));
    g_authors = NULL;
    g_author_count = 0;
}

// Books can credit several authors ("A, B"); each author gets an entry listing the book.
static void build_authors(void)
{
    int cap = g_book_count * 2 + 1, n = 0;
    author_t *authors = ps_alloc(cap * sizeof(author_t));
    for (int i = 0; i < g_book_count; i++) {
        const char *p = g_books[i].author;
        while (p && *p) {
            const char *end = strstr(p, ", ");
            size_t len = end ? (size_t)(end - p) : strlen(p);
            // Server data can carry stray spaces ("Joe White "), which would break surname sorting.
            const char *next = end ? end + 2 : NULL;
            while (len && *p == ' ') {
                p++;
                len--;
            }
            while (len && p[len - 1] == ' ') len--;
            if (len) {
                int a = 0;
                while (a < n && !(strlen(authors[a].name) == len && strncasecmp(authors[a].name, p, len) == 0)) a++;
                if (a == n && n < cap) {
                    authors[n].name = ps_strdup(p, len);
                    // Sort by surname: the last word of the name.
                    const char *sp = strrchr(authors[n].name, ' ');
                    const char *surname = sp ? sp + 1 : authors[n].name;
                    authors[n].sort_key = ps_strdup(surname, strlen(surname));
                    authors[n].books.idx = ps_alloc(8 * sizeof(int));
                    n++;
                }
                if (a < n) {
                    book_list_t *bl = &authors[a].books;
                    if (bl->count && bl->count % 8 == 0) {
                        bl->idx = heap_caps_realloc(bl->idx, (bl->count + 8) * sizeof(int), MALLOC_CAP_SPIRAM);
                    }
                    bl->idx[bl->count++] = i;
                }
            }
            p = next;
        }
    }
    qsort(authors, n, sizeof(author_t), cmp_author);
    g_authors = authors;
    g_author_count = n;
}

void ui_set_books(const abs_book_t *books, int count)
{
    free_lists();
    g_books = books;
    g_book_count = count;
    s_books_loaded_at = lv_tick_get();

    g_alpha.idx = ps_alloc(count * sizeof(int));
    g_continue.idx = ps_alloc(count * sizeof(int));
    g_recent.idx = ps_alloc(count * sizeof(int));
    for (int i = 0; i < count; i++) {
        g_alpha.idx[g_alpha.count++] = i;  // the server list is already A-Z
        g_recent.idx[g_recent.count++] = i;
        if (ui_book_in_progress(&books[i])) g_continue.idx[g_continue.count++] = i;
    }
    qsort(g_continue.idx, g_continue.count, sizeof(int), cmp_last_update);
    qsort(g_recent.idx, g_recent.count, sizeof(int), cmp_added);
    if (g_recent.count > RECENT_MAX) g_recent.count = RECENT_MAX;
    build_authors();

    home_set_books();
    library_set_books();
    ui_show_message(count ? NULL : "No books found");
}

void ui_request_refresh(void)
{
    s_refresh_requested = true;
}

bool ui_take_refresh_request(void)
{
    bool r = s_refresh_requested;
    s_refresh_requested = false;
    return r;
}

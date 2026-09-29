#pragma once

// Shared between the UI shell (ui.c) and its pages. Everything here runs in the LVGL task.

#include <stdbool.h>
#include <stddef.h>
#include "lvgl.h"
#include "abs_api.h"

#define COLOR_BG     lv_color_hex(0x101418)
#define COLOR_CARD   lv_color_hex(0x1E252C)
#define COLOR_ACCENT lv_color_hex(0xF0A030)
#define COLOR_TEXT   lv_color_hex(0xF2F2F2)
#define COLOR_MUTED  lv_color_hex(0x8C96A0)

// Touch layout rule: rings (Now Playing's progress ring, Library's A-Z ring) claim every touch at
// radius >= ~166 px from the screen centre. Keep tappable things' bounding boxes inside radius
// 160, and round controls visually inside ~150, so they never sit under a ring.

// The dock spans y 39..77; pages start below it.
#define DOCK_Y   39
#define PAGE_TOP 84

typedef enum {
    PAGE_HOME,
    PAGE_LIBRARY,
    PAGE_PLAYER,
    PAGE_COUNT,
} ui_page_t;

// Views of the book array as index lists into g_books.
typedef struct {
    int *idx;
    int count;
} book_list_t;

typedef struct {
    char *name;
    char *sort_key;  // surname first, for A-Z
    book_list_t books;
} author_t;

extern const abs_book_t *g_books;
extern int g_book_count;
extern book_list_t g_alpha;      // all books A-Z
extern book_list_t g_continue;   // started, not finished; most recently listened first
extern book_list_t g_recent;     // newest additions first
extern author_t *g_authors;      // A-Z by surname
extern int g_author_count;

/* helpers (ui.c) */
lv_obj_t *ui_round_button(lv_obj_t *parent, int size, const char *text, const lv_font_t *font, lv_event_cb_t cb,
                          void *user);
lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int width);
lv_obj_t *ui_page_container(lv_obj_t *parent);
void ui_book_subtitle(const abs_book_t *b, char *buf, size_t len);
bool ui_book_in_progress(const abs_book_t *b);
lv_obj_t *ui_add_book_row(lv_obj_t *list, int book_index);
void ui_open_book(int book_index);
void ui_show_page(ui_page_t page);

/* pages */
void home_build(lv_obj_t *page);
void home_set_books(void);
void home_refresh(void);

void library_build(lv_obj_t *page);
void library_set_books(void);
void library_refresh(void);

void playing_build(lv_obj_t *page);
void playing_prepare(int book_index);
void playing_refresh(void);

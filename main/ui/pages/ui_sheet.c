// Book details sheet: a full-screen overlay (long-press any book, or tap the title on Now Playing)
// with play and download controls. Everything tappable stays inside radius ~150 (ui_priv.h).

#include <stdio.h>
#include <string.h>
#include "cover.h"
#include "download.h"
#include "player.h"
#include "storage.h"
#include "ui_priv.h"

#define COVER_BOX     100
#define CONFIRM_MS    3000

static lv_obj_t *s_sheet, *s_cover_box, *s_cover, *s_placeholder, *s_title, *s_author, *s_info, *s_dl_btn,
    *s_dl_label, *s_status;
static const lv_image_dsc_t *s_cover_src;
static int s_book = -1;
static uint32_t s_confirm_until;

static void fmt_duration(char *buf, size_t len, double secs)
{
    int m = (int)(secs / 60);
    snprintf(buf, len, "%dh %02dm", m / 60, m % 60);
}

static void close_sheet(void)
{
    s_book = -1;
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
}

static void on_close(lv_event_t *e) { close_sheet(); }

static void on_play(lv_event_t *e)
{
    int b = s_book;
    close_sheet();
    ui_open_book(b);
}

static void on_download(lv_event_t *e)
{
    if (s_book < 0 || !storage_ready()) return;
    const abs_book_t *b = &g_books[s_book];
    dl_state_t st = download_state(b->id, NULL);
    switch (st) {
    case DL_NONE:
    case DL_ERROR:
        download_start(b);
        break;
    case DL_QUEUED:
    case DL_ACTIVE:
    case DL_DONE:
        // Cancelling or deleting loses data: ask for a second tap.
        if (lv_tick_get() > s_confirm_until) {
            s_confirm_until = lv_tick_get() + CONFIRM_MS;
        } else {
            s_confirm_until = 0;
            player_status_t ps;
            player_get_status(&ps);
            if (strcmp(ps.item_id, b->id) == 0) player_stop();  // it may be playing from the file
            download_remove(b->id);
        }
        break;
    default:
        break;
    }
    ui_sheet_refresh();
}

void ui_sheet_refresh(void)
{
    if (s_book < 0) return;
    const abs_book_t *b = &g_books[s_book];

    const lv_image_dsc_t *d = cover_get(b->id, COVER_THUMB);
    if (d && d != s_cover_src) {
        lv_image_set_src(s_cover, d);
        lv_obj_remove_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
        s_cover_src = d;
    }

    int pct = 0;
    dl_state_t st = download_state(b->id, &pct);
    const bool confirming = lv_tick_get() <= s_confirm_until;
    char label[40], status[64];
    uint64_t free_b, total_b;
    storage_space(&free_b, &total_b);
    snprintf(status, sizeof(status), "%.1f GB free on SD card", free_b / 1e9);

    if (!storage_ready()) {
        snprintf(label, sizeof(label), "No SD card");
        status[0] = 0;
    } else {
        switch (st) {
        case DL_QUEUED:
            snprintf(label, sizeof(label), confirming ? "Tap to cancel" : LV_SYMBOL_CLOSE " Queued");
            snprintf(status, sizeof(status), "Waiting to download");
            break;
        case DL_ACTIVE:
            snprintf(label, sizeof(label), confirming ? "Tap to cancel" : LV_SYMBOL_CLOSE " %d%%", pct);
            snprintf(status, sizeof(status), "Downloading... %d%%", pct);
            break;
        case DL_DONE:
            snprintf(label, sizeof(label), confirming ? "Tap to delete" : LV_SYMBOL_TRASH " Remove");
            snprintf(status, sizeof(status), LV_SYMBOL_SD_CARD " Downloaded  " LV_SYMBOL_BULLET "  %.1f GB free",
                     free_b / 1e9);
            break;
        case DL_ERROR:
            snprintf(label, sizeof(label), LV_SYMBOL_REFRESH " Retry");
            snprintf(status, sizeof(status), "Download failed");
            break;
        case DL_REMOVING:
            snprintf(label, sizeof(label), "Removing...");
            break;
        default:
            snprintf(label, sizeof(label), LV_SYMBOL_DOWNLOAD " Download");
            break;
        }
    }
    if (strcmp(lv_label_get_text(s_dl_label), label) != 0) lv_label_set_text(s_dl_label, label);
    if (strcmp(lv_label_get_text(s_status), status) != 0) lv_label_set_text(s_status, status);
    lv_obj_set_style_bg_color(s_dl_btn, confirming ? lv_color_hex(0xC0392B) : COLOR_CARD, 0);
}

void ui_sheet_show(int book_index)
{
    if (book_index < 0 || book_index >= g_book_count) return;
    if (g_books[book_index].podcast) {  // shows have episodes instead of book details
        ui_episodes_show(book_index);
        return;
    }
    const abs_book_t *b = &g_books[book_index];
    s_book = book_index;
    s_confirm_until = 0;
    s_cover_src = NULL;
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_placeholder, b->title);
    lv_label_set_text(s_title, b->title);
    lv_label_set_text(s_author, b->author);

    char dur[16], info[64];
    fmt_duration(dur, sizeof(dur), b->duration);
    if (b->finished) snprintf(info, sizeof(info), "%s  " LV_SYMBOL_BULLET "  finished", dur);
    else if (b->current_time > 0 && b->progress < 0.01f) snprintf(info, sizeof(info), "%s  " LV_SYMBOL_BULLET "  <1%%", dur);
    else if (b->current_time > 0) snprintf(info, sizeof(info), "%s  " LV_SYMBOL_BULLET "  %d%%", dur, (int)(b->progress * 100));
    else snprintf(info, sizeof(info), "%s", dur);
    lv_label_set_text(s_info, info);

    ui_sheet_refresh();
    lv_obj_remove_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_sheet);
}

void ui_sheet_hide(void)
{
    close_sheet();
}

bool ui_sheet_visible(void)
{
    return s_book >= 0;
}

static lv_obj_t *pill(lv_obj_t *parent, int w, const char *text, lv_event_cb_t cb, bool accent)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 40);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, accent ? COLOR_ACCENT : COLOR_CARD, 0);
    lv_obj_t *l = ui_label(b, &ui_font_14, accent ? lv_color_black() : COLOR_TEXT, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

void sheet_build(lv_obj_t *scr)
{
    // Full-screen and clickable, so it swallows touches meant for the page underneath.
    s_sheet = ui_page_container(scr);
    lv_obj_set_style_bg_color(s_sheet, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *close = ui_round_button(s_sheet, 36, LV_SYMBOL_CLOSE, &ui_font_16, on_close, NULL);
    lv_obj_align(close, LV_ALIGN_CENTER, 0, -128);

    s_cover_box = lv_obj_create(s_sheet);
    lv_obj_remove_style_all(s_cover_box);
    lv_obj_set_size(s_cover_box, COVER_BOX, COVER_BOX);
    lv_obj_align(s_cover_box, LV_ALIGN_CENTER, 0, -52);
    lv_obj_set_style_bg_color(s_cover_box, COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(s_cover_box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_cover_box, 8, 0);
    lv_obj_set_style_clip_corner(s_cover_box, true, 0);
    lv_obj_remove_flag(s_cover_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    s_placeholder = ui_label(s_cover_box, &ui_font_14, COLOR_MUTED, COVER_BOX - 12);
    lv_label_set_long_mode(s_placeholder, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_placeholder);
    // The cached thumbnail is COVER_THUMB_SIZE; scale it down to fit the box.
    s_cover = lv_image_create(s_cover_box);
    lv_image_set_scale(s_cover, 256 * COVER_BOX / COVER_THUMB_SIZE);
    lv_obj_center(s_cover);

    s_title = ui_label(s_sheet, &ui_font_16, COLOR_TEXT, 250);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 14);
    s_author = ui_label(s_sheet, &ui_font_14, COLOR_MUTED, 240);
    lv_label_set_long_mode(s_author, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_author, LV_ALIGN_CENTER, 0, 36);
    s_info = ui_label(s_sheet, &ui_font_14, COLOR_ACCENT, 240);
    lv_obj_align(s_info, LV_ALIGN_CENTER, 0, 56);

    lv_obj_t *play = pill(s_sheet, 104, LV_SYMBOL_PLAY " Play", on_play, true);
    lv_obj_align(play, LV_ALIGN_CENTER, -60, 94);
    s_dl_btn = pill(s_sheet, 116, "", on_download, false);
    lv_obj_align(s_dl_btn, LV_ALIGN_CENTER, 62, 94);
    s_dl_label = lv_obj_get_child(s_dl_btn, 0);

    s_status = ui_label(s_sheet, &ui_font_14, COLOR_MUTED, 210);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 130);

    lv_obj_add_flag(s_sheet, LV_OBJ_FLAG_HIDDEN);
}

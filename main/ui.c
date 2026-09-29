// Two screens sized for the 360x360 round panel: a scrolling library list and a now-playing
// view with a chapter-progress ring around the edge. Player state is polled on an LVGL timer,
// so nothing outside the LVGL task touches widgets.

#include "ui.h"

#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "cover.h"
#include "player.h"

#define COLOR_BG     lv_color_hex(0x101418)
#define COLOR_CARD   lv_color_hex(0x1E252C)
#define COLOR_ACCENT lv_color_hex(0xF0A030)
#define COLOR_TEXT   lv_color_hex(0xF2F2F2)
#define COLOR_MUTED  lv_color_hex(0x8C96A0)

static const abs_book_t *s_books;
static int s_book_count;
static volatile bool s_refresh_requested;

static lv_obj_t *s_lib_scr, *s_lib_list, *s_lib_msg, *s_lib_now_btn;
static lv_obj_t *s_play_scr, *s_cover_img, *s_arc, *s_title, *s_chapter, *s_state, *s_play_label, *s_time, *s_remaining;
static bool s_arc_dragging;
static lv_image_dsc_t *s_cover;  // currently shown on s_cover_img
static uint32_t s_state_override_until;

/* ---------- helpers ---------- */

static void fmt_time(char *buf, size_t len, double secs)
{
    int s = secs < 0 ? 0 : (int)secs;
    if (s >= 3600) {
        snprintf(buf, len, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    } else {
        snprintf(buf, len, "%d:%02d", s / 60, s % 60);
    }
}

static lv_obj_t *round_button(lv_obj_t *parent, int size, const char *text, const lv_font_t *font,
                              lv_event_cb_t cb, void *user)
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

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, int width)
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

static void flash_state(const char *text)
{
    lv_label_set_text(s_state, text);
    s_state_override_until = lv_tick_get() + 1500;
}

/* ---------- events ---------- */

static void on_book_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_book_count) return;
    player_open(&s_books[i]);
    cover_request(s_books[i].id);
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_title, s_books[i].title);
    lv_label_set_text(s_chapter, s_books[i].author);
    lv_label_set_text(s_time, "");
    lv_label_set_text(s_remaining, "");
    lv_arc_set_value(s_arc, 0);
    lv_screen_load_anim(s_play_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void on_now_playing(lv_event_t *e)
{
    lv_screen_load_anim(s_play_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void on_back(lv_event_t *e)
{
    lv_screen_load_anim(s_lib_scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
}

static void on_refresh(lv_event_t *e)
{
    s_refresh_requested = true;
    ui_show_message("Refreshing...");
}

static void on_toggle(lv_event_t *e) { player_toggle(); }
static void on_back30(lv_event_t *e) { player_seek_relative(-30); flash_state("-30 s"); }
static void on_fwd30(lv_event_t *e) { player_seek_relative(30); flash_state("+30 s"); }
static void on_prev_ch(lv_event_t *e) { player_chapter_step(-1); flash_state("Previous chapter"); }
static void on_next_ch(lv_event_t *e) { player_chapter_step(1); flash_state("Next chapter"); }

static void on_volume(lv_event_t *e)
{
    player_status_t st;
    player_get_status(&st);
    int v = st.volume + (int)(intptr_t)lv_event_get_user_data(e);
    v = v < 0 ? 0 : (v > 100 ? 100 : v);
    player_set_volume(v);
    char buf[24];
    snprintf(buf, sizeof(buf), LV_SYMBOL_VOLUME_MAX " %d%%", v);
    flash_state(buf);
}

// Dragging the ring scrubs within the current chapter.
static void on_arc_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    player_status_t st;
    player_get_status(&st);
    double len = st.chapter_end - st.chapter_start;
    double target = st.chapter_start + len * lv_arc_get_value(s_arc) / 1000.0;
    if (code == LV_EVENT_PRESSED) {
        s_arc_dragging = true;
    } else if (code == LV_EVENT_VALUE_CHANGED && s_arc_dragging) {
        char buf[16];
        fmt_time(buf, sizeof(buf), target - st.chapter_start);
        flash_state(buf);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_arc_dragging = false;
        if (len > 0) {
            player_seek_to(target);
        }
    }
}

/* ---------- periodic refresh ---------- */

static void refresh_timer(lv_timer_t *t)
{
    lv_image_dsc_t *cover = cover_take();
    if (cover) {
        lv_image_dsc_t *old = s_cover;
        lv_image_set_src(s_cover_img, cover);
        lv_obj_remove_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        s_cover = cover;
        if (old) {
            lv_image_cache_drop(old);
            cover_free(old);
        }
    }

    player_status_t st;
    player_get_status(&st);

    bool loaded = st.state != PLAYER_IDLE && st.item_id[0];
    if (loaded) {
        lv_obj_remove_flag(s_lib_now_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lib_now_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_screen_active() != s_play_scr) return;

    if (st.title[0] && strcmp(lv_label_get_text(s_title), st.title) != 0) {
        lv_label_set_text(s_title, st.title);
    }
    const char *sub = st.chapter[0] ? st.chapter : st.author;
    if (strcmp(lv_label_get_text(s_chapter), sub) != 0) {
        lv_label_set_text(s_chapter, sub);
    }

    bool playing = st.state == PLAYER_PLAYING || st.state == PLAYER_BUFFERING || st.state == PLAYER_LOADING;
    lv_label_set_text(s_play_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    if (lv_tick_get() > s_state_override_until) {
        const char *msg = "";
        switch (st.state) {
        case PLAYER_LOADING:   msg = "Opening..."; break;
        case PLAYER_BUFFERING: msg = "Buffering..."; break;
        case PLAYER_PAUSED:    msg = "Paused"; break;
        case PLAYER_FINISHED:  msg = "Finished"; break;
        case PLAYER_ERROR:     msg = "Stream error - tap " LV_SYMBOL_PLAY " to retry"; break;
        default: break;
        }
        lv_label_set_text(s_state, msg);
    }

    if (st.duration > 0) {
        double ch_len = st.chapter_end - st.chapter_start;
        double in_ch = st.position - st.chapter_start;
        if (!s_arc_dragging && ch_len > 0) {
            lv_arc_set_value(s_arc, (int)(1000 * in_ch / ch_len));
        }
        char a[16], b[16], buf[48];
        fmt_time(a, sizeof(a), in_ch);
        fmt_time(b, sizeof(b), ch_len);
        snprintf(buf, sizeof(buf), "%s / %s", a, b);
        lv_label_set_text(s_time, buf);

        int left = (int)(st.duration - st.position);
        snprintf(buf, sizeof(buf), "%dh %02dm left  " LV_SYMBOL_BULLET "  %d%%", left / 3600, (left / 60) % 60,
                 (int)(100 * st.position / st.duration));
        lv_label_set_text(s_remaining, buf);
    }
}

/* ---------- screens ---------- */

static void build_library(void)
{
    s_lib_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_lib_scr, COLOR_BG, 0);
    lv_obj_remove_flag(s_lib_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(s_lib_scr, &lv_font_montserrat_20, COLOR_ACCENT, 0);
    lv_label_set_text(title, "Library");
    lv_obj_align(title, LV_ALIGN_TOP_MID, -16, 30);

    lv_obj_t *refresh = round_button(s_lib_scr, 36, LV_SYMBOL_REFRESH, &lv_font_montserrat_14, on_refresh, NULL);
    lv_obj_align_to(refresh, title, LV_ALIGN_OUT_RIGHT_MID, 10, 0);

    s_lib_list = lv_list_create(s_lib_scr);
    lv_obj_set_size(s_lib_list, 280, 236);
    lv_obj_align(s_lib_list, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_opa(s_lib_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_lib_list, 0, 0);
    lv_obj_set_style_pad_row(s_lib_list, 6, 0);
    lv_obj_set_scrollbar_mode(s_lib_list, LV_SCROLLBAR_MODE_OFF);

    s_lib_msg = make_label(s_lib_scr, &lv_font_montserrat_16, COLOR_MUTED, 240);
    lv_label_set_long_mode(s_lib_msg, LV_LABEL_LONG_WRAP);
    lv_obj_center(s_lib_msg);

    s_lib_now_btn = lv_button_create(s_lib_scr);
    lv_obj_set_size(s_lib_now_btn, 150, 40);
    lv_obj_align(s_lib_now_btn, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_radius(s_lib_now_btn, 20, 0);
    lv_obj_set_style_bg_color(s_lib_now_btn, COLOR_ACCENT, 0);
    lv_obj_t *l = lv_label_create(s_lib_now_btn);
    lv_label_set_text(l, LV_SYMBOL_AUDIO "  Now playing");
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(s_lib_now_btn, on_now_playing, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_lib_now_btn, LV_OBJ_FLAG_HIDDEN);
}

static void build_player(void)
{
    s_play_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_play_scr, COLOR_BG, 0);
    lv_obj_remove_flag(s_play_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Cover art fills the round screen behind everything else (pre-dimmed by the loader).
    s_cover_img = lv_image_create(s_play_scr);
    lv_obj_center(s_cover_img);
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);

    s_arc = lv_arc_create(s_play_scr);
    lv_obj_set_size(s_arc, 348, 348);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 270);
    lv_arc_set_bg_angles(s_arc, 0, 360);
    lv_arc_set_range(s_arc, 0, 1000);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, COLOR_CARD, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, COLOR_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_arc, COLOR_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_arc, 3, LV_PART_KNOB);
    lv_obj_add_event_cb(s_arc, on_arc_event, LV_EVENT_ALL, NULL);
    // Only the ring itself should grab touches, not the whole square it sits in.
    lv_obj_add_flag(s_arc, LV_OBJ_FLAG_ADV_HITTEST);

    lv_obj_t *back = round_button(s_play_scr, 44, LV_SYMBOL_LIST, &lv_font_montserrat_16, on_back, NULL);
    lv_obj_align(back, LV_ALIGN_CENTER, 0, -134);

    s_title = make_label(s_play_scr, &lv_font_montserrat_20, COLOR_TEXT, 250);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -88);

    s_chapter = make_label(s_play_scr, &lv_font_montserrat_14, COLOR_MUTED, 240);
    lv_label_set_long_mode(s_chapter, LV_LABEL_LONG_DOT);
    lv_obj_align(s_chapter, LV_ALIGN_CENTER, 0, -62);

    s_state = make_label(s_play_scr, &lv_font_montserrat_14, COLOR_ACCENT, 260);
    lv_obj_align(s_state, LV_ALIGN_CENTER, 0, -40);

    lv_obj_t *play = round_button(s_play_scr, 96, LV_SYMBOL_PLAY, &lv_font_montserrat_40, on_toggle, NULL);
    lv_obj_set_style_bg_color(play, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(0xC07818), LV_STATE_PRESSED);
    s_play_label = lv_obj_get_child(play, 0);
    lv_obj_set_style_text_color(s_play_label, lv_color_black(), 0);
    lv_obj_align(play, LV_ALIGN_CENTER, 0, 12);

    lv_obj_t *b30 = round_button(s_play_scr, 64, "-30", &lv_font_montserrat_20, on_back30, NULL);
    lv_obj_align(b30, LV_ALIGN_CENTER, -100, 12);
    lv_obj_t *f30 = round_button(s_play_scr, 64, "+30", &lv_font_montserrat_20, on_fwd30, NULL);
    lv_obj_align(f30, LV_ALIGN_CENTER, 100, 12);

    s_time = make_label(s_play_scr, &lv_font_montserrat_16, COLOR_TEXT, 220);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, 76);
    s_remaining = make_label(s_play_scr, &lv_font_montserrat_14, COLOR_MUTED, 240);
    lv_obj_align(s_remaining, LV_ALIGN_CENTER, 0, 98);

    static const struct { const char *sym; lv_event_cb_t cb; intptr_t arg; int x; } row[] = {
        {LV_SYMBOL_PREV, on_prev_ch, 0, -84},
        {LV_SYMBOL_VOLUME_MID, on_volume, -10, -28},
        {LV_SYMBOL_VOLUME_MAX, on_volume, 10, 28},
        {LV_SYMBOL_NEXT, on_next_ch, 0, 84},
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = round_button(s_play_scr, 44, row[i].sym, &lv_font_montserrat_16, row[i].cb, (void *)row[i].arg);
        lv_obj_align(b, LV_ALIGN_CENTER, row[i].x, 134);
    }
}

void ui_init(void)
{
    build_library();
    build_player();
    lv_screen_load(s_lib_scr);
    lv_timer_create(refresh_timer, 250, NULL);
}

void ui_show_message(const char *msg)
{
    lv_label_set_text(s_lib_msg, msg ? msg : "");
    if (msg && msg[0]) {
        lv_obj_remove_flag(s_lib_msg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_lib_msg);
    } else {
        lv_obj_add_flag(s_lib_msg, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_set_books(const abs_book_t *books, int count)
{
    lv_obj_clean(s_lib_list);
    s_books = books;
    s_book_count = count;
    for (int i = 0; i < count; i++) {
        const abs_book_t *b = &books[i];
        lv_obj_t *btn = lv_list_add_button(s_lib_list, NULL, NULL);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
        lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 10, 0);
        lv_obj_set_style_pad_row(btn, 2, 0);

        lv_obj_t *t = lv_label_create(btn);
        lv_label_set_text(t, b->title);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, lv_pct(100));
        lv_obj_set_height(t, LV_SIZE_CONTENT);
        lv_obj_set_style_text_color(t, COLOR_TEXT, 0);

        char sub[160];
        if (b->finished) {
            snprintf(sub, sizeof(sub), "%s  " LV_SYMBOL_OK, b->author);
        } else if (b->current_time > 0) {
            snprintf(sub, sizeof(sub), "%s  " LV_SYMBOL_BULLET " %d%%", b->author, (int)(b->progress * 100));
        } else {
            snprintf(sub, sizeof(sub), "%s", b->author);
        }
        lv_obj_t *a = lv_label_create(btn);
        lv_label_set_text(a, sub);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
        lv_obj_set_width(a, lv_pct(100));
        lv_obj_set_style_text_font(a, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(a, (b->current_time > 0 && !b->finished) ? COLOR_ACCENT : COLOR_MUTED, 0);

        lv_obj_add_event_cb(btn, on_book_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    ui_show_message(count ? NULL : "No books found");
}

bool ui_take_refresh_request(void)
{
    bool r = s_refresh_requested;
    s_refresh_requested = false;
    return r;
}

// Now Playing: the loaded book with a chapter-progress ring around the edge. When nothing is loaded
// it offers the most recently listened book so playback can be resumed with one tap.

#include <stdio.h>
#include <string.h>
#include "cover.h"
#include "player.h"
#include "ui_priv.h"

static lv_obj_t *s_backdrop, *s_arc, *s_title, *s_chapter, *s_state, *s_play_label, *s_time, *s_remaining;
static const lv_image_dsc_t *s_backdrop_src;
static bool s_arc_dragging;
static uint32_t s_state_override_until;
static int s_resume = -1;  // book offered for resume while the player is idle

static void fmt_time(char *buf, size_t len, double secs)
{
    int s = secs < 0 ? 0 : (int)secs;
    if (s >= 3600) {
        snprintf(buf, len, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    } else {
        snprintf(buf, len, "%d:%02d", s / 60, s % 60);
    }
}

static void flash_state(const char *text)
{
    lv_label_set_text(s_state, text);
    s_state_override_until = lv_tick_get() + 1500;
}

static bool player_loaded(const player_status_t *st)
{
    return st->state != PLAYER_IDLE && st->item_id[0];
}

/* ---------- events ---------- */

static void on_toggle(lv_event_t *e)
{
    player_status_t st;
    player_get_status(&st);
    if (!player_loaded(&st)) {
        if (s_resume >= 0) ui_open_book(s_resume);
        return;
    }
    player_toggle();
}

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
        if (len > 0 && player_loaded(&st)) {
            player_seek_to(target);
        }
    }
}

/* ---------- refresh ---------- */

static void set_backdrop(const char *item_id)
{
    const lv_image_dsc_t *bd = cover_get(item_id, COVER_BACKDROP);
    if (bd == s_backdrop_src) return;
    if (bd) {
        lv_image_set_src(s_backdrop, bd);
        lv_obj_remove_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    }
    s_backdrop_src = bd;
}

static void set_text(lv_obj_t *label, const char *text)
{
    if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

static void refresh_idle(void)
{
    s_resume = g_continue.count ? g_continue.idx[0] : -1;
    lv_label_set_text(s_play_label, LV_SYMBOL_PLAY);
    if (s_resume < 0) {
        set_backdrop(NULL);
        set_text(s_title, "Nothing playing");
        set_text(s_chapter, "");
        set_text(s_state, "");
        set_text(s_time, "");
        set_text(s_remaining, "");
        lv_arc_set_value(s_arc, 0);
        return;
    }
    const abs_book_t *b = &g_books[s_resume];
    set_backdrop(b->id);
    set_text(s_title, b->title);
    set_text(s_chapter, b->author);
    if (lv_tick_get() > s_state_override_until) set_text(s_state, "Tap " LV_SYMBOL_PLAY " to resume");
    char buf[48];
    int left = (int)(b->duration - b->current_time);
    snprintf(buf, sizeof(buf), "%dh %02dm left  " LV_SYMBOL_BULLET "  %d%%", left / 3600, (left / 60) % 60,
             (int)(b->progress * 100));
    set_text(s_remaining, buf);
    set_text(s_time, "");
    lv_arc_set_value(s_arc, (int)(1000 * b->progress));
}

void playing_refresh(void)
{
    player_status_t st;
    player_get_status(&st);
    if (!player_loaded(&st)) {
        refresh_idle();
        return;
    }
    s_resume = -1;
    set_backdrop(st.item_id);

    if (st.title[0]) set_text(s_title, st.title);
    set_text(s_chapter, st.chapter[0] ? st.chapter : st.author);

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
        set_text(s_state, msg);
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
        set_text(s_time, buf);

        int left = (int)(st.duration - st.position);
        snprintf(buf, sizeof(buf), "%dh %02dm left  " LV_SYMBOL_BULLET "  %d%%", left / 3600, (left / 60) % 60,
                 (int)(100 * st.position / st.duration));
        set_text(s_remaining, buf);
    }
}

void playing_prepare(int book_index)
{
    const abs_book_t *b = &g_books[book_index];
    s_backdrop_src = NULL;
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_title, b->title);
    lv_label_set_text(s_chapter, b->author);
    lv_label_set_text(s_state, "Opening...");
    lv_label_set_text(s_time, "");
    lv_label_set_text(s_remaining, "");
    lv_arc_set_value(s_arc, 0);
}

/* ---------- build ---------- */

void playing_build(lv_obj_t *page)
{
    // Cover art fills the round screen behind everything else (pre-dimmed by the loader).
    s_backdrop = lv_image_create(page);
    lv_obj_center(s_backdrop);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);

    s_arc = lv_arc_create(page);
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

    // Everything tappable stays inside radius ~150 so it never sits under the ring (ui_priv.h).
    s_title = ui_label(page, &lv_font_montserrat_20, COLOR_TEXT, 230);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -90);

    s_chapter = ui_label(page, &lv_font_montserrat_14, COLOR_MUTED, 230);
    lv_label_set_long_mode(s_chapter, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_chapter, LV_ALIGN_CENTER, 0, -67);

    s_state = ui_label(page, &lv_font_montserrat_14, COLOR_ACCENT, 240);
    lv_label_set_long_mode(s_state, LV_LABEL_LONG_DOT);
    lv_obj_align(s_state, LV_ALIGN_CENTER, 0, -48);

    lv_obj_t *play = ui_round_button(page, 80, LV_SYMBOL_PLAY, &lv_font_montserrat_40, on_toggle, NULL);
    lv_obj_set_style_bg_color(play, COLOR_ACCENT, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(0xC07818), LV_STATE_PRESSED);
    s_play_label = lv_obj_get_child(play, 0);
    lv_obj_set_style_text_color(s_play_label, lv_color_black(), 0);
    lv_obj_align(play, LV_ALIGN_CENTER, 0, 4);

    lv_obj_t *b30 = ui_round_button(page, 60, "-30", &lv_font_montserrat_20, on_back30, NULL);
    lv_obj_align(b30, LV_ALIGN_CENTER, -94, 4);
    lv_obj_t *f30 = ui_round_button(page, 60, "+30", &lv_font_montserrat_20, on_fwd30, NULL);
    lv_obj_align(f30, LV_ALIGN_CENTER, 94, 4);

    s_time = ui_label(page, &lv_font_montserrat_16, COLOR_TEXT, 200);
    lv_obj_align(s_time, LV_ALIGN_CENTER, 0, 58);
    s_remaining = ui_label(page, &lv_font_montserrat_14, COLOR_MUTED, 220);
    lv_obj_align(s_remaining, LV_ALIGN_CENTER, 0, 79);

    static const struct { const char *sym; lv_event_cb_t cb; intptr_t arg; int x; } row[] = {
        {LV_SYMBOL_PREV, on_prev_ch, 0, -66},
        {LV_SYMBOL_VOLUME_MID, on_volume, -10, -22},
        {LV_SYMBOL_VOLUME_MAX, on_volume, 10, 22},
        {LV_SYMBOL_NEXT, on_next_ch, 0, 66},
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = ui_round_button(page, 38, row[i].sym, &lv_font_montserrat_16, row[i].cb, (void *)row[i].arg);
        lv_obj_align(b, LV_ALIGN_CENTER, row[i].x, 114);
    }
}

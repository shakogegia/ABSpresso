// Two screens sized for the 360x360 round panel: the library (a scrolling list or a cover
// carousel, toggled from the header) and a now-playing view with a chapter-progress ring
// around the edge. Player state and cover art are polled on an LVGL timer, so nothing outside
// the LVGL task touches widgets.

#include "ui.h"

#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "nvs.h"
#include "cover.h"
#include "player.h"

#define COLOR_BG     lv_color_hex(0x101418)
#define COLOR_CARD   lv_color_hex(0x1E252C)
#define COLOR_ACCENT lv_color_hex(0xF0A030)
#define COLOR_TEXT   lv_color_hex(0xF2F2F2)
#define COLOR_MUTED  lv_color_hex(0x8C96A0)

// Carousel geometry: three cards on a track; neighbours peek in from the screen edges.
#define CARD_SIZE    COVER_THUMB_SIZE
#define CARD_SPACING 185
#define CAROUSEL_Y   62

typedef struct {
    lv_obj_t *root, *img, *placeholder;
    const lv_image_dsc_t *src;
    int index;
} card_t;

static const abs_book_t *s_books;
static int s_book_count;
static volatile bool s_refresh_requested;

static lv_obj_t *s_lib_scr, *s_lib_list, *s_lib_msg, *s_lib_now_btn, *s_view_btn_label;
static lv_obj_t *s_carousel, *s_track, *s_car_title, *s_car_sub, *s_car_count;
static card_t s_cards[3];  // previous, current, next
static int s_car_index;
static bool s_car_animating;
static bool s_carousel_view;

static lv_obj_t *s_play_scr, *s_backdrop, *s_arc, *s_title, *s_chapter, *s_state, *s_play_label, *s_time, *s_remaining;
static const lv_image_dsc_t *s_backdrop_src;
static bool s_arc_dragging;
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

static void book_subtitle(const abs_book_t *b, char *buf, size_t len)
{
    if (b->finished) {
        snprintf(buf, len, "%s  " LV_SYMBOL_OK, b->author);
    } else if (b->current_time > 0) {
        snprintf(buf, len, "%s  " LV_SYMBOL_BULLET " %d%%", b->author, (int)(b->progress * 100));
    } else {
        snprintf(buf, len, "%s", b->author);
    }
}

static bool book_in_progress(const abs_book_t *b)
{
    return b->current_time > 0 && !b->finished;
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

static void open_book(int i)
{
    if (i < 0 || i >= s_book_count) return;
    player_open(&s_books[i]);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
    s_backdrop_src = NULL;
    lv_label_set_text(s_title, s_books[i].title);
    lv_label_set_text(s_chapter, s_books[i].author);
    lv_label_set_text(s_time, "");
    lv_label_set_text(s_remaining, "");
    lv_arc_set_value(s_arc, 0);
    lv_screen_load_anim(s_play_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

/* ---------- carousel ---------- */

static void card_bind(card_t *c, int index)
{
    c->index = index;
    c->src = NULL;
    lv_obj_add_flag(c->img, LV_OBJ_FLAG_HIDDEN);
    if (index < 0 || index >= s_book_count) {
        lv_obj_add_flag(c->root, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(c->root, LV_OBJ_FLAG_HIDDEN);
    // Title stands in until (or unless) the cover arrives.
    lv_label_set_text(c->placeholder, s_books[index].title);
}

static void carousel_bind(void)
{
    for (int k = 0; k < 3; k++) {
        card_bind(&s_cards[k], s_car_index + k - 1);
    }
    if (s_car_index >= s_book_count) {
        lv_label_set_text(s_car_title, "");
        lv_label_set_text(s_car_sub, "");
        lv_label_set_text(s_car_count, "");
        return;
    }
    const abs_book_t *b = &s_books[s_car_index];
    char buf[160];
    lv_label_set_text(s_car_title, b->title);
    book_subtitle(b, buf, sizeof(buf));
    lv_label_set_text(s_car_sub, buf);
    lv_obj_set_style_text_color(s_car_sub, book_in_progress(b) ? COLOR_ACCENT : COLOR_MUTED, 0);
    snprintf(buf, sizeof(buf), "%d / %d", s_car_index + 1, s_book_count);
    lv_label_set_text(s_car_count, buf);
}

// Called every refresh while the carousel is visible. Request order sets download priority
// (most recent first): look-ahead cards, then neighbours, then the centre card.
static void carousel_update_covers(void)
{
    static const int ahead[] = {2, -2};
    for (int i = 0; i < 2; i++) {
        int j = s_car_index + ahead[i];
        if (j >= 0 && j < s_book_count) cover_get(s_books[j].id, COVER_THUMB);
    }
    static const int order[] = {0, 2, 1};
    for (int i = 0; i < 3; i++) {
        card_t *c = &s_cards[order[i]];
        if (c->index < 0 || c->index >= s_book_count) continue;
        const lv_image_dsc_t *d = cover_get(s_books[c->index].id, COVER_THUMB);
        if (d && d != c->src) {
            lv_image_set_src(c->img, d);
            lv_obj_remove_flag(c->img, LV_OBJ_FLAG_HIDDEN);
            c->src = d;
        }
    }
}

static void track_set_x(void *obj, int32_t x)
{
    lv_obj_set_x(obj, x);
}

static void carousel_anim_done(lv_anim_t *a)
{
    s_car_index += (int)(intptr_t)lv_anim_get_user_data(a);
    lv_obj_set_x(s_track, 0);
    carousel_bind();
    carousel_update_covers();
    s_car_animating = false;
}

static void carousel_step(int delta)
{
    int target = s_car_index + delta;
    if (s_car_animating || target < 0 || target >= s_book_count) return;
    s_car_animating = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_track);
    lv_anim_set_exec_cb(&a, track_set_x);
    lv_anim_set_values(&a, 0, -delta * CARD_SPACING);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_user_data(&a, (void *)(intptr_t)delta);
    lv_anim_set_completed_cb(&a, carousel_anim_done);
    lv_anim_start(&a);
}

static void on_card_clicked(lv_event_t *e)
{
    card_t *c = lv_event_get_user_data(e);
    if (s_car_animating) return;
    if (c == &s_cards[1]) {
        open_book(c->index);
    } else {
        carousel_step(c == &s_cards[0] ? -1 : 1);
    }
}

static void on_carousel_gesture(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) {
        carousel_step(1);
    } else if (dir == LV_DIR_RIGHT) {
        carousel_step(-1);
    }
}

static void build_carousel(lv_obj_t *parent)
{
    s_carousel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_carousel);
    lv_obj_set_size(s_carousel, 360, CARD_SIZE);
    lv_obj_align(s_carousel, LV_ALIGN_TOP_MID, 0, CAROUSEL_Y);
    lv_obj_remove_flag(s_carousel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_carousel, on_carousel_gesture, LV_EVENT_GESTURE, NULL);

    s_track = lv_obj_create(s_carousel);
    lv_obj_remove_style_all(s_track);
    lv_obj_set_size(s_track, 360, CARD_SIZE);
    lv_obj_remove_flag(s_track, LV_OBJ_FLAG_SCROLLABLE);

    for (int k = 0; k < 3; k++) {
        card_t *c = &s_cards[k];
        c->root = lv_obj_create(s_track);
        lv_obj_remove_style_all(c->root);
        lv_obj_set_size(c->root, CARD_SIZE, CARD_SIZE);
        lv_obj_set_pos(c->root, 180 - CARD_SIZE / 2 + (k - 1) * CARD_SPACING, 0);
        lv_obj_set_style_bg_color(c->root, COLOR_CARD, 0);
        lv_obj_set_style_bg_opa(c->root, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c->root, 10, 0);
        lv_obj_remove_flag(c->root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(c->root, on_card_clicked, LV_EVENT_CLICKED, c);

        c->placeholder = make_label(c->root, &lv_font_montserrat_16, COLOR_MUTED, CARD_SIZE - 20);
        lv_label_set_long_mode(c->placeholder, LV_LABEL_LONG_WRAP);
        lv_obj_center(c->placeholder);

        c->img = lv_image_create(c->root);
        lv_obj_center(c->img);
        lv_obj_add_flag(c->img, LV_OBJ_FLAG_HIDDEN);

        if (k != 1) {
            // Dim the neighbours without an opacity layer (cheap per-part opacities only).
            lv_obj_set_style_image_opa(c->img, LV_OPA_40, 0);
            lv_obj_set_style_bg_opa(c->root, LV_OPA_40, 0);
            lv_obj_set_style_text_opa(c->placeholder, LV_OPA_40, 0);
        }
    }

    s_car_title = make_label(parent, &lv_font_montserrat_16, COLOR_TEXT, 250);
    lv_label_set_long_mode(s_car_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_car_title, LV_ALIGN_TOP_MID, 0, CAROUSEL_Y + CARD_SIZE + 8);
    s_car_sub = make_label(parent, &lv_font_montserrat_14, COLOR_MUTED, 250);
    lv_label_set_long_mode(s_car_sub, LV_LABEL_LONG_DOT);
    lv_obj_align(s_car_sub, LV_ALIGN_TOP_MID, 0, CAROUSEL_Y + CARD_SIZE + 30);
    s_car_count = make_label(parent, &lv_font_montserrat_14, COLOR_MUTED, 120);
    lv_obj_align(s_car_count, LV_ALIGN_TOP_MID, 0, CAROUSEL_Y + CARD_SIZE + 50);
}

static void apply_view(void)
{
    lv_obj_t *car[] = {s_carousel, s_car_title, s_car_sub, s_car_count};
    for (int i = 0; i < 4; i++) {
        if (s_carousel_view) lv_obj_remove_flag(car[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(car[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_carousel_view) lv_obj_add_flag(s_lib_list, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_lib_list, LV_OBJ_FLAG_HIDDEN);
    // The button shows what you'll switch to.
    lv_label_set_text(s_view_btn_label, s_carousel_view ? LV_SYMBOL_LIST : LV_SYMBOL_IMAGE);
}

static void on_toggle_view(lv_event_t *e)
{
    s_carousel_view = !s_carousel_view;
    apply_view();
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "carousel", s_carousel_view);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------- events ---------- */

static void on_book_clicked(lv_event_t *e)
{
    open_book((int)(intptr_t)lv_event_get_user_data(e));
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

static void refresh_player(const player_status_t *st)
{
    const lv_image_dsc_t *bd = cover_get(st->item_id, COVER_BACKDROP);
    if (bd != s_backdrop_src) {
        if (bd) {
            lv_image_set_src(s_backdrop, bd);
            lv_obj_remove_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
        }
        s_backdrop_src = bd;
    }

    if (st->title[0] && strcmp(lv_label_get_text(s_title), st->title) != 0) {
        lv_label_set_text(s_title, st->title);
    }
    const char *sub = st->chapter[0] ? st->chapter : st->author;
    if (strcmp(lv_label_get_text(s_chapter), sub) != 0) {
        lv_label_set_text(s_chapter, sub);
    }

    bool playing = st->state == PLAYER_PLAYING || st->state == PLAYER_BUFFERING || st->state == PLAYER_LOADING;
    lv_label_set_text(s_play_label, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    if (lv_tick_get() > s_state_override_until) {
        const char *msg = "";
        switch (st->state) {
        case PLAYER_LOADING:   msg = "Opening..."; break;
        case PLAYER_BUFFERING: msg = "Buffering..."; break;
        case PLAYER_PAUSED:    msg = "Paused"; break;
        case PLAYER_FINISHED:  msg = "Finished"; break;
        case PLAYER_ERROR:     msg = "Stream error - tap " LV_SYMBOL_PLAY " to retry"; break;
        default: break;
        }
        lv_label_set_text(s_state, msg);
    }

    if (st->duration > 0) {
        double ch_len = st->chapter_end - st->chapter_start;
        double in_ch = st->position - st->chapter_start;
        if (!s_arc_dragging && ch_len > 0) {
            lv_arc_set_value(s_arc, (int)(1000 * in_ch / ch_len));
        }
        char a[16], b[16], buf[48];
        fmt_time(a, sizeof(a), in_ch);
        fmt_time(b, sizeof(b), ch_len);
        snprintf(buf, sizeof(buf), "%s / %s", a, b);
        lv_label_set_text(s_time, buf);

        int left = (int)(st->duration - st->position);
        snprintf(buf, sizeof(buf), "%dh %02dm left  " LV_SYMBOL_BULLET "  %d%%", left / 3600, (left / 60) % 60,
                 (int)(100 * st->position / st->duration));
        lv_label_set_text(s_remaining, buf);
    }
}

static void refresh_timer(lv_timer_t *t)
{
    player_status_t st;
    player_get_status(&st);

    bool loaded = st.state != PLAYER_IDLE && st.item_id[0];
    if (loaded) {
        lv_obj_remove_flag(s_lib_now_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lib_now_btn, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *active = lv_screen_active();
    if (active == s_play_scr) {
        refresh_player(&st);
    } else if (active == s_lib_scr && s_carousel_view && !s_car_animating) {
        carousel_update_covers();
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
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 26);

    lv_obj_t *refresh = round_button(s_lib_scr, 36, LV_SYMBOL_REFRESH, &lv_font_montserrat_14, on_refresh, NULL);
    lv_obj_align_to(refresh, title, LV_ALIGN_OUT_RIGHT_MID, 12, 0);
    lv_obj_t *view = round_button(s_lib_scr, 36, LV_SYMBOL_IMAGE, &lv_font_montserrat_14, on_toggle_view, NULL);
    lv_obj_align_to(view, title, LV_ALIGN_OUT_LEFT_MID, -12, 0);
    s_view_btn_label = lv_obj_get_child(view, 0);

    s_lib_list = lv_list_create(s_lib_scr);
    lv_obj_set_size(s_lib_list, 280, 236);
    lv_obj_align(s_lib_list, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_opa(s_lib_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_lib_list, 0, 0);
    lv_obj_set_style_pad_row(s_lib_list, 6, 0);
    lv_obj_set_scrollbar_mode(s_lib_list, LV_SCROLLBAR_MODE_OFF);

    build_carousel(s_lib_scr);

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

    uint8_t carousel = 0;
    nvs_handle_t h;
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "carousel", &carousel);
        nvs_close(h);
    }
    s_carousel_view = carousel;
    carousel_bind();
    apply_view();
}

static void build_player(void)
{
    s_play_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_play_scr, COLOR_BG, 0);
    lv_obj_remove_flag(s_play_scr, LV_OBJ_FLAG_SCROLLABLE);

    // Cover art fills the round screen behind everything else (pre-dimmed by the loader).
    s_backdrop = lv_image_create(s_play_scr);
    lv_obj_center(s_backdrop);
    lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);

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
    build_player();
    build_library();
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
        book_subtitle(b, sub, sizeof(sub));
        lv_obj_t *a = lv_label_create(btn);
        lv_label_set_text(a, sub);
        lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
        lv_obj_set_width(a, lv_pct(100));
        lv_obj_set_style_text_font(a, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(a, book_in_progress(b) ? COLOR_ACCENT : COLOR_MUTED, 0);

        lv_obj_add_event_cb(btn, on_book_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (s_car_index >= count) s_car_index = 0;
    carousel_bind();
    ui_show_message(count ? NULL : "No books found");
}

bool ui_take_refresh_request(void)
{
    bool r = s_refresh_requested;
    s_refresh_requested = false;
    return r;
}

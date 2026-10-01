// Settings (a view on the Library page): library and device info, and refreshing the library.
// Room to grow: choosing the library, clearing caches, etc.

#include <stdio.h>
#include <string.h>
#include "abs_api.h"
#include "download.h"
#include "esp_wifi.h"
#include "board.h"
#include "config.h"
#include "nvs.h"
#include "power.h"
#include "storage.h"
#include "ui.h"
#include "ui_priv.h"
#include "wifi.h"

enum {
    ROW_SERVER, ROW_LIBRARY, ROW_CONTENTS, ROW_UPDATED, ROW_WIFI, ROW_SD,
    ROW_BRIGHT, ROW_SCREEN, ROW_SLEEP, ROW_ROTATE,  // tap to cycle
    ROW_SETUP,                                      // tap to open the setup portal
    ROW_COUNT
};
static const char *const s_keys[ROW_COUNT] = {"Server", "Library", "Contents", "Updated", "Wi-Fi", "SD card",
                                              "Brightness", "Screen off", "Sleep", "Rotate 180\xc2\xb0",
                                              "Wi-Fi & login"};

static bool s_rotated = true;  // default: upside down (how this device is mounted)

static void apply_rotation(bool rotated, bool save)
{
    s_rotated = rotated;
    board_set_rotated(rotated);
    lv_obj_invalidate(lv_screen_active());
    if (save) config_set_rotate(rotated);
}

// Choices the power rows cycle through.
static const int BRIGHTNESS[] = {20, 40, 60, 80, 100};
static const int SCREEN_OFF_S[] = {30, 60, 120, 300, 0};
static const int SLEEP_MIN[] = {5, 10, 30, 0};

static int next_choice(const int *choices, int n, int current)
{
    for (int i = 0; i < n; i++) {
        if (choices[i] == current) return choices[(i + 1) % n];
    }
    return choices[0];
}

static void on_power_row(lv_event_t *e)
{
    power_config_t c;
    power_get_config(&c);
    switch ((int)(intptr_t)lv_event_get_user_data(e)) {
    case ROW_BRIGHT: c.brightness = next_choice(BRIGHTNESS, 5, c.brightness); break;
    case ROW_SCREEN: c.screen_off_s = next_choice(SCREEN_OFF_S, 5, c.screen_off_s); break;
    case ROW_SLEEP:  c.sleep_min = next_choice(SLEEP_MIN, 4, c.sleep_min); break;
    case ROW_SETUP:
        ui_setup_show();
        return;
    case ROW_ROTATE:
        apply_rotation(!s_rotated, true);
        settings_refresh();
        return;
    }
    power_set_config(&c);
    settings_refresh();
}

static lv_obj_t *s_values[ROW_COUNT];

/* ---------- library picker ---------- */

static lv_obj_t *s_picker, *s_picker_list;

static void picker_close(void)
{
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
}

static void on_picker_close(lv_event_t *e) { picker_close(); }

static void on_pick(lv_event_t *e)
{
    int n;
    const char *sel;
    const abs_library_t *libs = ui_libraries(&n, &sel);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    picker_close();
    if (i < 0 || i >= n || strcmp(libs[i].id, sel) == 0) return;
    ui_request_library(libs[i].id);
    char msg[96];
    snprintf(msg, sizeof(msg), "Opening %s...", libs[i].name);
    ui_show_message(msg);
}

static void on_library_row(lv_event_t *e)
{
    int n;
    const char *sel;
    const abs_library_t *libs = ui_libraries(&n, &sel);
    if (n < 2) return;
    lv_obj_clean(s_picker_list);
    for (int i = 0; i < n; i++) {
        const bool current = strcmp(libs[i].id, sel) == 0;
        lv_obj_t *btn = lv_list_add_button(s_picker_list, NULL, NULL);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_bg_color(btn, current ? COLOR_ACCENT : COLOR_CARD, 0);
        lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 10, 0);
        lv_obj_set_style_pad_row(btn, 2, 0);
        lv_obj_t *t = lv_label_create(btn);
        lv_label_set_text(t, libs[i].name);
        ui_one_line(t, &ui_font_16);
        lv_obj_set_style_text_color(t, current ? lv_color_black() : COLOR_TEXT, 0);
        lv_obj_t *k = lv_label_create(btn);
        lv_label_set_text(k, libs[i].podcast ? "Podcast library" : "Audiobook library");
        ui_one_line(k, &ui_font_14);
        lv_obj_set_style_text_color(k, current ? lv_color_black() : COLOR_MUTED, 0);
        lv_obj_add_event_cb(btn, on_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_remove_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_picker);
}

bool libpicker_visible(void)
{
    return s_picker && !lv_obj_has_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
}

void libpicker_build(lv_obj_t *scr)
{
    s_picker = ui_page_container(scr);
    lv_obj_set_style_bg_color(s_picker, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_picker, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *close = ui_round_button(s_picker, 36, LV_SYMBOL_CLOSE, &ui_font_16, on_picker_close, NULL);
    lv_obj_align(close, LV_ALIGN_CENTER, 0, -128);
    lv_obj_t *title = ui_label(s_picker, &ui_font_16, COLOR_ACCENT, 200);
    lv_label_set_text(title, "Choose library");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -90);

    s_picker_list = lv_list_create(s_picker);
    lv_obj_set_size(s_picker_list, 240, 210);
    lv_obj_align(s_picker_list, LV_ALIGN_CENTER, 0, 36);
    lv_obj_set_style_bg_opa(s_picker_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_picker_list, 0, 0);
    lv_obj_set_style_pad_all(s_picker_list, 0, 0);
    lv_obj_set_style_pad_row(s_picker_list, 6, 0);
    lv_obj_set_scrollbar_mode(s_picker_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
}

static void on_refresh(lv_event_t *e)
{
    ui_request_refresh();
    ui_show_message("Refreshing...");
}

static void set_value(int row, const char *text)
{
    if (strcmp(lv_label_get_text(s_values[row]), text) != 0) lv_label_set_text(s_values[row], text);
}

void settings_refresh(void)
{
    char buf[80];
    set_value(ROW_SERVER, abs_server());
    int nlibs;
    const char *sel;
    ui_libraries(&nlibs, &sel);
    snprintf(buf, sizeof(buf), "%s%s", abs_library_name()[0] ? abs_library_name() : "-",
             nlibs > 1 ? "  " LV_SYMBOL_RIGHT : "");
    set_value(ROW_LIBRARY, buf);
    snprintf(buf, sizeof(buf), "%d %s, %d authors", g_book_count, ui_library_is_podcast() ? "shows" : "books",
             g_author_count);
    set_value(ROW_CONTENTS, buf);

    uint32_t loaded;
    bool cached;
    ui_get_source(&loaded, &cached);
    uint32_t mins = lv_tick_elaps(loaded) / 60000;
    if (!g_book_count) snprintf(buf, sizeof(buf), "not loaded");
    else if (cached) snprintf(buf, sizeof(buf), "from SD cache");
    else if (mins == 0) snprintf(buf, sizeof(buf), "just now");
    else snprintf(buf, sizeof(buf), "%lu min ago", (unsigned long)mins);
    set_value(ROW_UPDATED, buf);

    wifi_ap_record_t ap;
    if (wifi_is_connected() && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%.32s", (const char *)ap.ssid);
    } else {
        snprintf(buf, sizeof(buf), "offline");
    }
    set_value(ROW_WIFI, buf);

    if (storage_ready()) {
        uint64_t free_b, total_b;
        storage_space(&free_b, &total_b);
        snprintf(buf, sizeof(buf), "%.1f GB free, %d book%s", free_b / 1e9, g_downloaded.count,
                 g_downloaded.count == 1 ? "" : "s");
    } else {
        snprintf(buf, sizeof(buf), "none");
    }
    set_value(ROW_SD, buf);

    power_config_t pc;
    power_get_config(&pc);
    snprintf(buf, sizeof(buf), "%d%%  " LV_SYMBOL_RIGHT, pc.brightness);
    set_value(ROW_BRIGHT, buf);
    if (!pc.screen_off_s) snprintf(buf, sizeof(buf), "Never  " LV_SYMBOL_RIGHT);
    else if (pc.screen_off_s < 60) snprintf(buf, sizeof(buf), "%d s  " LV_SYMBOL_RIGHT, pc.screen_off_s);
    else snprintf(buf, sizeof(buf), "%d min  " LV_SYMBOL_RIGHT, pc.screen_off_s / 60);
    set_value(ROW_SCREEN, buf);
    if (!pc.sleep_min) snprintf(buf, sizeof(buf), "Never  " LV_SYMBOL_RIGHT);
    else snprintf(buf, sizeof(buf), "after %d min  " LV_SYMBOL_RIGHT, pc.sleep_min);
    set_value(ROW_SLEEP, buf);
    set_value(ROW_ROTATE, s_rotated ? "On  " LV_SYMBOL_RIGHT : "Off  " LV_SYMBOL_RIGHT);
    set_value(ROW_SETUP, "Set up via phone  " LV_SYMBOL_RIGHT);
}

void settings_build(lv_obj_t *parent)
{
    apply_rotation(config_get()->rotate180, false);

    lv_obj_t *table = lv_obj_create(parent);
    lv_obj_remove_style_all(table);
    lv_obj_set_size(table, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(table, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_flex_flow(table, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(table, 4, 0);
    lv_obj_remove_flag(table, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < ROW_COUNT; i++) {
        lv_obj_t *row = lv_obj_create(table);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 18);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        if (i >= ROW_BRIGHT) {
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(row, 3);
            lv_obj_add_event_cb(row, on_power_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
        if (i == ROW_LIBRARY) {
            // Tap the library to choose another one.
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_ext_click_area(row, 6);
            lv_obj_add_event_cb(row, on_library_row, LV_EVENT_CLICKED, NULL);
        }
        lv_obj_t *k = ui_label(row, &ui_font_14, COLOR_MUTED, 84);
        lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_long_mode(k, LV_LABEL_LONG_CLIP);  // one line
        lv_label_set_text(k, s_keys[i]);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
        s_values[i] = ui_label(row, &ui_font_14, COLOR_TEXT, 164);
        lv_obj_set_style_text_align(s_values[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(s_values[i], LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_align(s_values[i], LV_ALIGN_RIGHT_MID, 0, 0);
    }

    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 180, 38);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 4 + ROW_COUNT * 22 + 8);
    lv_obj_set_style_radius(btn, 19, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, COLOR_ACCENT, 0);
    lv_obj_t *l = ui_label(btn, &ui_font_14, lv_color_black(), 0);
    lv_label_set_text(l, LV_SYMBOL_REFRESH "  Refresh library");
    lv_obj_center(l);
    lv_obj_add_event_cb(btn, on_refresh, LV_EVENT_CLICKED, NULL);
}

#ifdef UI_CAPTURE
void settings_debug_open_picker(void)
{
    on_library_row(NULL);
}

void settings_debug_close_picker(void)
{
    picker_close();
}
#endif

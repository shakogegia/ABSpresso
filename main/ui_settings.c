// Settings (a view on the Library page): library and device info, and refreshing the library.
// Room to grow: choosing the library, clearing caches, etc.

#include <stdio.h>
#include <string.h>
#include "abs_api.h"
#include "download.h"
#include "esp_wifi.h"
#include "storage.h"
#include "ui.h"
#include "ui_priv.h"
#include "wifi.h"

enum { ROW_SERVER, ROW_LIBRARY, ROW_CONTENTS, ROW_UPDATED, ROW_WIFI, ROW_SD, ROW_COUNT };
static const char *const s_keys[ROW_COUNT] = {"Server", "Library", "Contents", "Updated", "Wi-Fi", "SD card"};

static lv_obj_t *s_values[ROW_COUNT];

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
    set_value(ROW_LIBRARY, abs_library_name()[0] ? abs_library_name() : "-");
    snprintf(buf, sizeof(buf), "%d books " LV_SYMBOL_BULLET " %d authors", g_book_count, g_author_count);
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
}

void settings_build(lv_obj_t *parent)
{
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
        lv_obj_t *k = ui_label(row, &lv_font_montserrat_14, COLOR_MUTED, 70);
        lv_obj_set_style_text_align(k, LV_TEXT_ALIGN_LEFT, 0);
        lv_label_set_text(k, s_keys[i]);
        lv_obj_align(k, LV_ALIGN_LEFT_MID, 0, 0);
        s_values[i] = ui_label(row, &lv_font_montserrat_14, COLOR_TEXT, 176);
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
    lv_obj_t *l = ui_label(btn, &lv_font_montserrat_14, lv_color_black(), 0);
    lv_label_set_text(l, LV_SYMBOL_REFRESH "  Refresh library");
    lv_obj_center(l);
    lv_obj_add_event_cb(btn, on_refresh, LV_EVENT_CLICKED, NULL);
}

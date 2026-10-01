// Status row above the dock: Bluetooth, Wi-Fi, and a battery icon that fills with the charge level
// (bolt while charging). Built last so it stays above pages and overlays; not clickable.

#include <stdio.h>
#include <string.h>
#include "battery.h"
#include "ui_priv.h"
#include "wifi.h"

#define COLOR_OK   lv_color_hex(0x4CAF50)
#define COLOR_LOW  lv_color_hex(0xE53935)
#define COLOR_OFF  lv_color_hex(0x4A545E)  // an "off" icon: visible but clearly inactive
#define BATT_W     30
#define BATT_H     16
#define PCT_W      38  // "100%" in Montserrat 14

static lv_obj_t *s_bt, *s_wifi, *s_batt, *s_fill, *s_nub, *s_bolt, *s_pct;

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void status_refresh(void)
{
    // Bluetooth isn't enabled in the firmware yet, so it always shows as off.
    lv_obj_set_style_text_color(s_wifi, wifi_is_connected() ? COLOR_TEXT : COLOR_OFF, 0);

    battery_status_t b;
    battery_get(&b);
    lv_obj_t *parts[] = {s_batt, s_nub};
    for (int i = 0; i < 2; i++) {
        if (b.present) lv_obj_remove_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(parts[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (!b.present) {
        lv_obj_add_flag(s_pct, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(s_pct, LV_OBJ_FLAG_HIDDEN);

    // One icon, three looks, always with a label beside it: on battery the fill shows the level and
    // the percentage; charging is a green outline with a bolt inside ("Chrg"); full is solid green
    // ("Full"). The voltage says little about the level while charging, so no percentage then.
    const bool on_battery = !b.charging && !b.charged;
    const lv_color_t outline = on_battery ? COLOR_TEXT : COLOR_OK;
    lv_obj_set_style_border_color(s_batt, outline, 0);
    lv_obj_set_style_bg_color(s_nub, outline, 0);

    const int inner = BATT_W - 6;  // inside the 2 px border and 1 px padding
    int fill = 0;
    lv_color_t fill_color = COLOR_OK;
    if (b.charged) {
        fill = inner;
    } else if (on_battery) {
        fill = b.percent > 0 ? LV_MAX(2, inner * b.percent / 100) : 0;
        fill_color = b.percent <= 20 ? COLOR_LOW : COLOR_OK;
    }
    lv_obj_set_width(s_fill, fill);
    lv_obj_set_style_bg_color(s_fill, fill_color, 0);

    if (b.charging) lv_obj_remove_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);

    char txt[8];
    if (b.charging) snprintf(txt, sizeof(txt), "Chrg");
    else if (b.charged) snprintf(txt, sizeof(txt), "Full");
    else snprintf(txt, sizeof(txt), "%d%%", b.percent);
    if (strcmp(lv_label_get_text(s_pct), txt) != 0) lv_label_set_text(s_pct, txt);
}

void status_build(lv_obj_t *scr)
{
    lv_obj_t *row = plain(scr);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 18);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    s_bt = ui_label(row, &ui_font_14, COLOR_OFF, 0);
    lv_label_set_text(s_bt, LV_SYMBOL_BLUETOOTH);
    s_wifi = ui_label(row, &ui_font_14, COLOR_OFF, 0);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);

    // Battery: an outline with a fill bar, a bolt drawn inside it while charging, and a terminal nub.
    lv_obj_t *batt = plain(row);
    lv_obj_set_size(batt, BATT_W + 3, BATT_H);
    s_batt = plain(batt);
    lv_obj_set_size(s_batt, BATT_W, BATT_H);
    lv_obj_set_style_border_width(s_batt, 2, 0);
    lv_obj_set_style_border_color(s_batt, COLOR_TEXT, 0);
    lv_obj_set_style_radius(s_batt, 3, 0);
    lv_obj_set_style_pad_all(s_batt, 1, 0);
    s_fill = plain(s_batt);
    lv_obj_set_size(s_fill, 0, lv_pct(100));
    lv_obj_set_style_bg_opa(s_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_fill, 1, 0);

    // A zig-zag bolt fitted to the inside of the outline (BATT_W - 6 by BATT_H - 6).
    static const lv_point_precise_t bolt[] = {{13, 0}, {8, 5}, {14, 5}, {9, 10}};
    s_bolt = lv_line_create(s_batt);
    lv_line_set_points(s_bolt, bolt, 4);
    lv_obj_set_style_line_width(s_bolt, 2, 0);
    lv_obj_set_style_line_color(s_bolt, COLOR_OK, 0);
    lv_obj_set_style_line_rounded(s_bolt, true, 0);
    lv_obj_align(s_bolt, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);

    s_nub = plain(batt);
    lv_obj_set_size(s_nub, 2, 6);
    lv_obj_align(s_nub, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_nub, COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(s_nub, LV_OPA_COVER, 0);

    // Fixed width (fits "100%"), left-aligned, so switching between "72%", "Chrg" and "Full"
    // never shifts the row.
    s_pct = ui_label(row, &ui_font_14, COLOR_TEXT, PCT_W);
    lv_obj_set_style_text_align(s_pct, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(s_pct, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_pct, "");
}

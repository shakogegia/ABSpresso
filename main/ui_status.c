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
#define BATT_W     24
#define BATT_H     12

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
    lv_obj_t *batt_parts[] = {s_batt, s_nub, s_pct};
    for (int i = 0; i < 3; i++) {
        if (b.present) lv_obj_remove_flag(batt_parts[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(batt_parts[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (b.charging && b.present) lv_obj_remove_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);
    if (!b.present) return;

    const int inner = BATT_W - 6;  // inside the 2 px border and 1 px padding
    lv_obj_set_width(s_fill, b.percent > 0 ? LV_MAX(2, inner * b.percent / 100) : 0);
    lv_color_t c = b.charging ? COLOR_ACCENT : (b.percent <= 20 ? COLOR_LOW : COLOR_OK);
    lv_obj_set_style_bg_color(s_fill, c, 0);

    char txt[16];
    if (b.charged) snprintf(txt, sizeof(txt), "Full");
    else snprintf(txt, sizeof(txt), "%d%%", b.percent);
    if (strcmp(lv_label_get_text(s_pct), txt) != 0) lv_label_set_text(s_pct, txt);
    lv_obj_set_style_text_color(s_bolt, b.charged ? COLOR_OK : COLOR_ACCENT, 0);
}

void status_build(lv_obj_t *scr)
{
    lv_obj_t *row = plain(scr);
    lv_obj_set_size(row, LV_SIZE_CONTENT, 18);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 14);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);

    s_bt = ui_label(row, &lv_font_montserrat_14, COLOR_OFF, 0);
    lv_label_set_text(s_bt, LV_SYMBOL_BLUETOOTH);
    s_wifi = ui_label(row, &lv_font_montserrat_14, COLOR_OFF, 0);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);

    // Battery: an outline with a fill bar and a terminal nub, then a bolt and the percentage.
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
    s_nub = plain(batt);
    lv_obj_set_size(s_nub, 2, 6);
    lv_obj_align(s_nub, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_nub, COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(s_nub, LV_OPA_COVER, 0);

    s_bolt = ui_label(row, &lv_font_montserrat_14, COLOR_ACCENT, 0);
    lv_label_set_text(s_bolt, LV_SYMBOL_CHARGE);
    s_pct = ui_label(row, &lv_font_montserrat_14, COLOR_TEXT, 0);
    lv_label_set_text(s_pct, "");

    lv_obj_add_flag(s_bolt, LV_OBJ_FLAG_HIDDEN);
}

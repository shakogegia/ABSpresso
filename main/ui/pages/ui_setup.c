// Setup screen: shows how to reach the setup portal (portal.c) from a phone or laptop: a QR code
// that joins the device's own Wi-Fi, the network name/password in text, and the progress of a save.

#include <stdio.h>
#include <string.h>
#include "config.h"
#include "portal.h"
#include "ui_priv.h"

static lv_obj_t *s_overlay, *s_qr, *s_net, *s_status, *s_close;
static char s_shown[64];

static void on_close(lv_event_t *e)
{
    portal_stop();
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

bool ui_setup_visible(void)
{
    return s_overlay && !lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

void ui_setup_refresh(void)
{
    char msg[160];
    bool busy;
    portal_status(msg, sizeof(msg), &busy);
    if (strcmp(lv_label_get_text(s_status), msg) != 0) lv_label_set_text(s_status, msg);
    // No going back without a configuration to go back to.
    if (config_complete() && !busy) lv_obj_remove_flag(s_close, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_close, LV_OBJ_FLAG_HIDDEN);
}

void ui_setup_show(void)
{
    portal_start();
    char ssid[33], pass[16], qr[96], txt[96];
    portal_network(ssid, sizeof(ssid), pass, sizeof(pass));
    // The standard Wi-Fi QR format: phone cameras offer to join the network.
    snprintf(qr, sizeof(qr), "WIFI:T:WPA;S:%s;P:%s;;", ssid, pass);
    if (strcmp(qr, s_shown) != 0) {
        lv_qrcode_update(s_qr, qr, strlen(qr));
        strlcpy(s_shown, qr, sizeof(s_shown));
    }
    snprintf(txt, sizeof(txt), "%s\nPassword  %s", ssid, pass);
    lv_label_set_text(s_net, txt);
    ui_setup_refresh();
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
}

void setup_build(lv_obj_t *scr)
{
    s_overlay = ui_page_container(scr);
    lv_obj_set_style_bg_color(s_overlay, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    s_close = ui_round_button(s_overlay, 36, LV_SYMBOL_CLOSE, &ui_font_16, on_close, NULL);
    lv_obj_align(s_close, LV_ALIGN_CENTER, 0, -128);

    lv_obj_t *title = ui_label(s_overlay, &ui_font_16, COLOR_ACCENT, 220);
    lv_label_set_text(title, "Scan to set up");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -96);

    // Dark modules on a light background, with a quiet zone, so phone cameras read it easily.
    s_qr = lv_qrcode_create(s_overlay);
    lv_qrcode_set_size(s_qr, 118);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_obj_set_style_border_color(s_qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_qr, 6, 0);
    lv_obj_align(s_qr, LV_ALIGN_CENTER, 0, -18);

    s_net = ui_label(s_overlay, &ui_font_14, COLOR_TEXT, 240);
    lv_obj_align(s_net, LV_ALIGN_CENTER, 0, 70);
    lv_obj_t *hint = ui_label(s_overlay, &ui_font_14, COLOR_MUTED, 220);
    lv_label_set_text(hint, "then open 192.168.4.1");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 100);
    s_status = ui_label(s_overlay, &ui_font_14, COLOR_ACCENT, 200);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 300);

    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

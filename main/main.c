#include <math.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "board.h"

static const char *TAG = "main";
static int s_taps;

static void btn_cb(lv_event_t *e)
{
    lv_obj_t *label = lv_event_get_user_data(e);
    lv_label_set_text_fmt(label, "Taps: %d", ++s_taps);
    ESP_LOGI(TAG, "tap %d", s_taps);
}

static void tone_test(void)
{
    const int rate = 22050, ms = 600;
    static int16_t buf[2 * 441];
    board_audio_set_volume(40);
    if (board_audio_open(rate, 2, 16) != ESP_OK) {
        return;
    }
    for (int n = 0; n < rate * ms / 1000; n += 441) {
        for (int i = 0; i < 441; i++) {
            int16_t v = (int16_t)(6000 * sinf(2 * M_PI * 440 * (n + i) / rate));
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        board_audio_write(buf, sizeof(buf));
    }
    board_audio_close();
    ESP_LOGI(TAG, "tone done");
}

void app_main(void)
{
    ESP_ERROR_CHECK(board_display_init(NULL));

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x102040), 0);
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "ABS Player");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, 160, 70);
    lv_obj_center(btn);
    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, "Tap me");
    lv_obj_center(label);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, label);
    lv_obj_t *arc = lv_arc_create(scr);
    lv_obj_set_size(arc, 340, 340);
    lv_obj_center(arc);
    lv_arc_set_value(arc, 40);
    lvgl_port_unlock();

    if (board_audio_init() == ESP_OK) {
        tone_test();
    }
}

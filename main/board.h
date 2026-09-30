#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#define BOARD_LCD_H_RES 360
#define BOARD_LCD_V_RES 360

// Brings up I2C, the IO expander, the QSPI display, touch and LVGL.
esp_err_t board_display_init(lv_display_t **out_disp);

// 0-100
void board_set_backlight(int percent);
// Panel sleep (display off + sleep-in) and wake. The backlight is separate.
void board_display_power(bool on);
// Reads the touch controller directly (for waking while LVGL is paused).
bool board_touch_pressed(void);
// Screen off and touch controller into its low-power scan, ready for deep sleep.
void board_prepare_deep_sleep(void);

// Brings up I2S to the PCM5101 DAC.
esp_err_t board_audio_init(void);

// Opens the output at the given format. Safe to call again when the format changes.
esp_err_t board_audio_open(int sample_rate, int channels, int bits);
void board_audio_close(void);
// Blocks until queued. Applies volume by scaling the buffer in place.
esp_err_t board_audio_write(int16_t *samples, int len);
// 0-100
void board_audio_set_volume(int volume);

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

// Brings up I2S to the PCM5101 DAC.
esp_err_t board_audio_init(void);

// Opens the output at the given format. Safe to call again when the format changes.
esp_err_t board_audio_open(int sample_rate, int channels, int bits);
void board_audio_close(void);
// Blocks until queued. Applies volume by scaling the buffer in place.
esp_err_t board_audio_write(int16_t *samples, int len);
// 0-100
void board_audio_set_volume(int volume);

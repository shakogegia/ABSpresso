#include "cover.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "abs_api.h"
#include "board.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_decoder.h"

static const char *TAG = "cover";

// Covers sit behind the player controls, so darken them enough for white text to stay readable.
#define DIM_NUM 2
#define DIM_DEN 5

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static char s_wanted[40];          // latest requested item
static lv_image_dsc_t *s_ready;    // finished cover waiting for the UI

static lv_image_dsc_t *decode(const uint8_t *jpg, size_t len)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpg,
        .indata_size = len,
        .out_format = JPEG_IMAGE_FORMAT_RGB888,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK || info.width == 0) {
        ESP_LOGW(TAG, "not a decodable JPEG");
        return NULL;
    }
    // Downscale on decode if the server sent something bigger than the screen.
    while (cfg.out_scale < JPEG_IMAGE_SCALE_1_8 &&
           (info.width >> cfg.out_scale) > 2 * BOARD_LCD_H_RES) {
        cfg.out_scale++;
    }
    const int w = info.width >> cfg.out_scale, h = info.height >> cfg.out_scale;

    // Decode to RGB888 and convert ourselves: that pins down the RGB565 byte order and lets the
    // dimming happen in the same pass. Everything lives in PSRAM.
    cfg.outbuf_size = w * h * 3;
    cfg.outbuf = heap_caps_malloc(cfg.outbuf_size, MALLOC_CAP_SPIRAM);
    lv_image_dsc_t *dsc = heap_caps_calloc(1, sizeof(*dsc), MALLOC_CAP_SPIRAM);
    uint16_t *px = heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM);
    if (!cfg.outbuf || !dsc || !px || esp_jpeg_decode(&cfg, &info) != ESP_OK) {
        ESP_LOGW(TAG, "decode failed (%dx%d)", w, h);
        free(cfg.outbuf);
        free(dsc);
        free(px);
        return NULL;
    }
    const uint8_t *rgb = cfg.outbuf;
    for (int i = 0; i < w * h; i++, rgb += 3) {
        uint32_t r = rgb[0] * DIM_NUM / DIM_DEN, g = rgb[1] * DIM_NUM / DIM_DEN, b = rgb[2] * DIM_NUM / DIM_DEN;
        px[i] = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    }
    free(cfg.outbuf);

    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = w * 2;
    dsc->data_size = w * h * 2;
    dsc->data = (const uint8_t *)px;
    return dsc;
}

static void cover_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        char id[40];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(id, s_wanted, sizeof(id));
        xSemaphoreGive(s_lock);
        if (!id[0]) continue;

        int64_t t0 = esp_timer_get_time();
        uint8_t *jpg = NULL;
        size_t len = 0;
        lv_image_dsc_t *dsc = NULL;
        if (abs_get_cover(id, BOARD_LCD_H_RES, &jpg, &len) == ESP_OK) {
            dsc = decode(jpg, len);
        }
        free(jpg);
        if (!dsc) {
            ESP_LOGI(TAG, "no cover for %s", id);
            continue;
        }
        ESP_LOGI(TAG, "%dx%d from %u B JPEG in %d ms (internal free %u)", (int)dsc->header.w, (int)dsc->header.h,
                 (unsigned)len, (int)((esp_timer_get_time() - t0) / 1000), heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (strcmp(id, s_wanted) == 0) {
            // A cover the UI hasn't picked up yet is stale now; replace it.
            lv_image_dsc_t *stale = s_ready;
            s_ready = dsc;
            dsc = stale;
        }
        xSemaphoreGive(s_lock);
        cover_free(dsc);  // either the stale one, or ours if the request moved on
    }
}

void cover_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    // Stack in PSRAM: this task never touches flash, and internal RAM is the scarce resource.
    xTaskCreatePinnedToCoreWithCaps(cover_task, "cover", 8192, NULL, 3, &s_task, 0, MALLOC_CAP_SPIRAM);
}

void cover_request(const char *item_id)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_wanted, item_id, sizeof(s_wanted));
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

lv_image_dsc_t *cover_take(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lv_image_dsc_t *d = s_ready;
    s_ready = NULL;
    xSemaphoreGive(s_lock);
    return d;
}

void cover_free(lv_image_dsc_t *dsc)
{
    if (!dsc) return;
    free((void *)dsc->data);
    free(dsc);
}

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

// Backdrops sit behind the player controls, so darken them enough for white text to stay readable.
#define DIM_NUM 2
#define DIM_DEN 5

#define CACHE_SLOTS    24
#define CACHE_BUDGET   (3 * 1024 * 1024)  // bytes of PSRAM for decoded covers
#define QUEUE_LEN      6
#define EVICT_AFTER_MS 2000               // entries requested more recently are assumed on screen

typedef struct {
    char id[40];
    cover_kind_t kind;
} cover_key_t;

typedef struct {
    cover_key_t key;
    lv_image_dsc_t *dsc;  // NULL = download failed; don't retry this boot
    uint32_t last_used;
    bool used;
} entry_t;

// Owned by the LVGL task.
static entry_t s_cache[CACHE_SLOTS];

// Shared with the loader task, under s_lock.
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static cover_key_t s_queue[QUEUE_LEN];  // [0] is served next
static int s_queue_len;
static cover_key_t s_inflight;
static bool s_done_ready;
static cover_key_t s_done_key;
static lv_image_dsc_t *s_done_dsc;

static bool key_eq(const cover_key_t *a, const char *id, cover_kind_t kind)
{
    return a->kind == kind && strcmp(a->id, id) == 0;
}

static size_t dsc_bytes(const lv_image_dsc_t *d)
{
    return d ? d->data_size + sizeof(*d) : 0;
}

static void free_dsc(lv_image_dsc_t *d)
{
    if (!d) return;
    lv_image_cache_drop(d);
    free((void *)d->data);
    free(d);
}

/* ---------- loader task ---------- */

static lv_image_dsc_t *decode(const uint8_t *jpg, size_t len, bool dim)
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
    // Downscale on decode if the server sent something much bigger than the screen.
    while (cfg.out_scale < JPEG_IMAGE_SCALE_1_8 && (info.width >> cfg.out_scale) > 2 * BOARD_LCD_H_RES) {
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
    const int num = dim ? DIM_NUM : 1, den = dim ? DIM_DEN : 1;
    const uint8_t *rgb = cfg.outbuf;
    for (int i = 0; i < w * h; i++, rgb += 3) {
        uint32_t r = rgb[0] * num / den, g = rgb[1] * num / den, b = rgb[2] * num / den;
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
        for (;;) {
            // Wait for the UI to collect the previous result before starting another.
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_done_ready || s_queue_len == 0) {
                xSemaphoreGive(s_lock);
                break;
            }
            cover_key_t k = s_queue[0];
            memmove(&s_queue[0], &s_queue[1], --s_queue_len * sizeof(cover_key_t));
            s_inflight = k;
            xSemaphoreGive(s_lock);

            int64_t t0 = esp_timer_get_time();
            const int size = k.kind == COVER_BACKDROP ? BOARD_LCD_H_RES : COVER_THUMB_SIZE;
            uint8_t *jpg = NULL;
            size_t len = 0;
            lv_image_dsc_t *dsc = NULL;
            if (abs_get_cover(k.id, size, &jpg, &len) == ESP_OK) {
                dsc = decode(jpg, len, k.kind == COVER_BACKDROP);
            }
            free(jpg);
            if (dsc) {
                ESP_LOGI(TAG, "%.8s %dx%d in %d ms", k.id, (int)dsc->header.w, (int)dsc->header.h,
                         (int)((esp_timer_get_time() - t0) / 1000));
            } else {
                ESP_LOGI(TAG, "no cover for %.8s", k.id);
            }

            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_inflight.id[0] = 0;
            s_done_key = k;
            s_done_dsc = dsc;
            s_done_ready = true;
            xSemaphoreGive(s_lock);
        }
    }
}

/* ---------- LVGL-task side ---------- */

static entry_t *cache_find(const char *id, cover_kind_t kind)
{
    for (int i = 0; i < CACHE_SLOTS; i++) {
        if (s_cache[i].used && key_eq(&s_cache[i].key, id, kind)) return &s_cache[i];
    }
    return NULL;
}

// Frees least-recently-used covers until `incoming` more bytes fit, leaving on-screen ones alone.
static entry_t *cache_make_room(size_t incoming)
{
    const uint32_t now = lv_tick_get();
    for (;;) {
        size_t total = incoming;
        entry_t *lru = NULL, *free_slot = NULL;
        for (int i = 0; i < CACHE_SLOTS; i++) {
            entry_t *e = &s_cache[i];
            if (!e->used) {
                if (!free_slot) free_slot = e;
                continue;
            }
            total += dsc_bytes(e->dsc);
            if (now - e->last_used > EVICT_AFTER_MS && (!lru || e->last_used < lru->last_used)) lru = e;
        }
        if ((total <= CACHE_BUDGET && free_slot) || !lru) {
            return free_slot;  // may be NULL if everything is on screen; the new cover is then dropped
        }
        free_dsc(lru->dsc);
        memset(lru, 0, sizeof(*lru));
    }
}

static void collect_done(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_done_ready) {
        xSemaphoreGive(s_lock);
        return;
    }
    cover_key_t k = s_done_key;
    lv_image_dsc_t *d = s_done_dsc;
    s_done_ready = false;
    s_done_dsc = NULL;
    const bool more = s_queue_len > 0;
    xSemaphoreGive(s_lock);
    if (more) xTaskNotifyGive(s_task);

    entry_t *e = cache_make_room(dsc_bytes(d));
    if (!e) {
        free_dsc(d);
        return;
    }
    e->used = true;
    e->key = k;
    e->dsc = d;
    e->last_used = lv_tick_get();
}

static void enqueue(const char *id, cover_kind_t kind)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = key_eq(&s_inflight, id, kind) || (s_done_ready && key_eq(&s_done_key, id, kind));
    if (!busy) {
        // Move to the front (or insert), dropping the oldest request if full.
        int at = s_queue_len < QUEUE_LEN ? s_queue_len : QUEUE_LEN - 1;
        for (int i = 0; i < s_queue_len; i++) {
            if (key_eq(&s_queue[i], id, kind)) {
                at = i;
                break;
            }
        }
        if (at == s_queue_len) s_queue_len++;
        memmove(&s_queue[1], &s_queue[0], at * sizeof(cover_key_t));
        strlcpy(s_queue[0].id, id, sizeof(s_queue[0].id));
        s_queue[0].kind = kind;
    }
    xSemaphoreGive(s_lock);
    if (!busy) xTaskNotifyGive(s_task);
}

const lv_image_dsc_t *cover_get(const char *item_id, cover_kind_t kind)
{
    if (!item_id || !item_id[0]) return NULL;
    collect_done();
    entry_t *e = cache_find(item_id, kind);
    if (e) {
        e->last_used = lv_tick_get();
        return e->dsc;
    }
    enqueue(item_id, kind);
    return NULL;
}

void cover_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    // Stack in PSRAM: this task never touches flash, and internal RAM is the scarce resource.
    xTaskCreatePinnedToCoreWithCaps(cover_task, "cover", 8192, NULL, 3, &s_task, 0, MALLOC_CAP_SPIRAM);
}

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "nvs_flash.h"
#include "abs_api.h"
#include "board.h"
#include "cover.h"
#include "player.h"
#include "storage.h"
#include "download.h"
#include "esp_timer.h"
#include "ui.h"
#include "wifi.h"

static void show_message(const char *msg)
{
    lvgl_port_lock(0);
    ui_show_message(msg);
    lvgl_port_unlock();
}

#define CACHE_ITEMS STORAGE_ROOT "/items.json"
#define CACHE_ME    STORAGE_ROOT "/me.json"
#define CACHE_NAME  STORAGE_ROOT "/library.txt"
#define RETRY_US    (30 * 1000000LL)

static abs_book_t *s_books;
static int s_count;

static void show_books(abs_book_t *fresh, int n, bool cached)
{
    lvgl_port_lock(0);
    ui_set_books(fresh, n);
    ui_set_source(cached);
    lvgl_port_unlock();
    abs_free_books(s_books, s_count);
    s_books = fresh;
    s_count = n;
}

// The last library the server sent, from the SD card: instant start, and works offline.
static bool load_cached(void)
{
    size_t items_len = 0;
    char *items = storage_read_file(CACHE_ITEMS, &items_len);
    char *me = storage_read_file(CACHE_ME, NULL);
    abs_book_t *fresh = NULL;
    int n = 0;
    bool ok = items && abs_parse_books(items, me, &fresh, &n) == ESP_OK && n > 0;
    free(items);
    free(me);
    if (!ok) {
        if (storage_ready()) ESP_LOGW("main", "no usable library cache (%u bytes)", (unsigned)items_len);
        return false;
    }
    download_sync_progress(fresh, n, false);
    char *name = storage_read_file(CACHE_NAME, NULL);
    abs_set_library_name(name);
    free(name);
    ESP_LOGI("main", "showing %d books from the SD cache", n);
    show_books(fresh, n, true);
    return true;
}

static bool load_network(bool quiet)
{
    abs_book_t *fresh = NULL;
    int n = 0;
    char *items = NULL, *me = NULL;
    if (abs_get_books(&fresh, &n, &items, &me) != ESP_OK) {
        if (!quiet) show_message("Couldn't reach Audiobookshelf.\nTap " LV_SYMBOL_REFRESH " to retry.");
        return false;
    }
    if (storage_ready()) {
        storage_write_file(CACHE_ITEMS, items, strlen(items));
        storage_write_file(CACHE_ME, me, strlen(me));
        storage_write_file(CACHE_NAME, abs_library_name(), strlen(abs_library_name()));
    }
    free(items);
    free(me);
    download_sync_progress(fresh, n, true);
    show_books(fresh, n, false);
    return true;
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_ERROR_CHECK(board_display_init(NULL));
    lvgl_port_lock(0);
    ui_init();
    ui_show_message("Connecting to Wi-Fi...");
    lvgl_port_unlock();

    ESP_ERROR_CHECK(board_audio_init());
    storage_init();
    abs_api_init();
    cover_init();
    player_init();
    download_init();
    wifi_start();

    const bool cached = load_cached();
    bool online = false;
    int64_t last_try = -RETRY_US, started = esp_timer_get_time();
    bool first = true;

    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (!online && wifi_is_connected() && now - last_try >= RETRY_US) {
            if (!cached) show_message("Loading library...");
            online = load_network(cached);
            last_try = now;
            if (online && first) {
                first = false;
                ESP_LOGI("main", "free after library load: internal %u, PSRAM %u",
                         heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#ifdef UI_CAPTURE
                void ui_capture_start(void);
                ui_capture_start();
#endif
            }
        } else if (!online && !cached && !wifi_is_connected() && now - started > 15000000LL) {
            show_message("Still waiting for Wi-Fi...");
        }
        if (ui_take_refresh_request()) {
            if (wifi_is_connected()) {
                online = load_network(false);
                last_try = now;
            } else {
                show_message("Offline - showing the saved library.");
                vTaskDelay(pdMS_TO_TICKS(2000));
                show_message(NULL);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lvgl_port.h"
#include "nvs_flash.h"
#include "abs_api.h"
#include "board.h"
#include "cover.h"
#include "player.h"
#include "ui.h"
#include "wifi.h"

static void show_message(const char *msg)
{
    lvgl_port_lock(0);
    ui_show_message(msg);
    lvgl_port_unlock();
}

static void load_library(abs_book_t **books, int *count)
{
    abs_book_t *fresh = NULL;
    int n = 0;
    if (abs_get_books(&fresh, &n) != ESP_OK) {
        show_message("Couldn't reach Audiobookshelf.\nTap " LV_SYMBOL_REFRESH " to retry.");
        return;
    }
    lvgl_port_lock(0);
    ui_set_books(fresh, n);
    lvgl_port_unlock();
    abs_free_books(*books, *count);
    *books = fresh;
    *count = n;
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
    abs_api_init();
    cover_init();
    player_init();
    wifi_start();

    while (!wifi_wait_connected(15000)) {
        show_message("Still waiting for Wi-Fi...");
    }
    show_message("Loading library...");

    abs_book_t *books = NULL;
    int count = 0;
    load_library(&books, &count);

    for (;;) {
        if (ui_take_refresh_request()) {
            load_library(&books, &count);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"

static const char *TAG = "main";

void app_main(void)
{
    uint32_t flash_size = 0;
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "Hello from ESP32-S3-Touch-LCD-1.85C");
    ESP_LOGI(TAG, "Flash: %lu MB, PSRAM free: %u KB", flash_size >> 20,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

#include "wifi.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "config.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
#define BIT_CONNECTED BIT0
#define BIT_FAILED    BIT1  // the last connection attempt ended in a disconnect

static volatile bool s_auto_reconnect = true;  // off while setup is scanning or testing

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_auto_reconnect && config_get()->wifi_ssid[0]) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        xEventGroupSetBits(s_events, BIT_FAILED);
        if (s_auto_reconnect) {
            ESP_LOGW(TAG, "disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t wifi_start(void)
{
    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, config_get()->wifi_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, config_get()->wifi_pass, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep between beacons saves a lot of power; the ~minute of buffered audio rides out
    // the added latency.
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    return ESP_OK;
}

bool wifi_wait_connected(int timeout_ms)
{
    return xEventGroupWaitBits(s_events, BIT_CONNECTED, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms)) & BIT_CONNECTED;
}

bool wifi_is_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & BIT_CONNECTED);
}

esp_err_t wifi_start_ap(const char *ssid, const char *password)
{
    wifi_config_t ap = {
        .ap = {
            .channel = 1,
            .max_connection = 2,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strlcpy((char *)ap.ap.ssid, ssid, sizeof(ap.ap.ssid));
    ap.ap.ssid_len = strlen(ssid);
    strlcpy((char *)ap.ap.password, password, sizeof(ap.ap.password));
    // Station stays up alongside, for scanning and for testing the new home network.
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    // Full power while setting up: modem sleep makes the setup page sluggish.
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "setup network '%s' up: %s", ssid, esp_err_to_name(err));
    return err;
}

void wifi_stop_ap(void)
{
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    s_auto_reconnect = true;
    if (!wifi_is_connected() && config_get()->wifi_ssid[0]) esp_wifi_connect();
}

int wifi_scan(wifi_ap_record_t *out, int max)
{
    // A station that keeps retrying a network blocks scans, so stop that while scanning.
    s_auto_reconnect = false;
    if (!wifi_is_connected()) esp_wifi_disconnect();
    const wifi_scan_config_t sc = {.show_hidden = false};
    uint16_t n = max;
    if (esp_wifi_scan_start(&sc, true) != ESP_OK || esp_wifi_scan_get_ap_records(&n, out) != ESP_OK) n = 0;
    return n;
}

bool wifi_try_connect(const char *ssid, const char *password, int timeout_ms)
{
    s_auto_reconnect = false;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    // Wait for an address; a disconnect along the way (wrong password, not found) is a failure,
    // but allow a couple of retries since the first attempt after switching can be refused.
    const TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    int failures = 0;
    while (xTaskGetTickCount() < end) {
        EventBits_t b = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED, pdTRUE, pdFALSE, pdMS_TO_TICKS(500));
        if (b & BIT_CONNECTED) {
            xEventGroupSetBits(s_events, BIT_CONNECTED);  // WaitBits cleared it
            return true;
        }
        if ((b & BIT_FAILED) && ++failures < 3) esp_wifi_connect();
        else if (b & BIT_FAILED) return false;
    }
    return false;
}

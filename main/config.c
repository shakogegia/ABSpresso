#include "config.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/task.h"
#include "nvs.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif

static const char *TAG = "config";

static app_config_t *s_cfg;  // in PSRAM: the tokens are large
static SemaphoreHandle_t s_lock;

typedef struct {
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
} flash_job_t;

static void flash_job(void *p)
{
    flash_job_t *j = p;
    j->fn(j->arg);
    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}

void flash_safe(void (*fn)(void *), void *arg)
{
    if (esp_ptr_in_dram((const void *)esp_cpu_get_sp())) {
        fn(arg);
        return;
    }
    StaticSemaphore_t buf;
    flash_job_t job = {fn, arg, xSemaphoreCreateBinaryStatic(&buf)};
    if (xTaskCreate(flash_job, "flash", 4096, &job, uxTaskPriorityGet(NULL), NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory to save settings");
        return;
    }
    xSemaphoreTake(job.done, portMAX_DELAY);
}

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) out[0] = 0;
}

void config_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cfg = heap_caps_calloc(1, sizeof(*s_cfg), MALLOC_CAP_SPIRAM);
    s_cfg->skip_back_s = 30;
    s_cfg->skip_fwd_s = 30;
    s_cfg->rotate180 = true;

    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) == ESP_OK) {
        get_str(h, "ssid", s_cfg->wifi_ssid, sizeof(s_cfg->wifi_ssid));
        get_str(h, "pass", s_cfg->wifi_pass, sizeof(s_cfg->wifi_pass));
        get_str(h, "server", s_cfg->server, sizeof(s_cfg->server));
        get_str(h, "user", s_cfg->username, sizeof(s_cfg->username));
        get_str(h, "access", s_cfg->access, sizeof(s_cfg->access));
        get_str(h, "refresh", s_cfg->refresh, sizeof(s_cfg->refresh));
        uint8_t v;
        if (nvs_get_u8(h, "skipb", &v) == ESP_OK) s_cfg->skip_back_s = v;
        if (nvs_get_u8(h, "skipf", &v) == ESP_OK) s_cfg->skip_fwd_s = v;
        nvs_close(h);
    }
    if (nvs_open("ui", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "rot180", &v) == ESP_OK) s_cfg->rotate180 = v;
        nvs_close(h);
    }

    // Development builds can bake in defaults (main/secrets.h); saved settings always win.
#ifdef WIFI_SSID
    if (!s_cfg->wifi_ssid[0]) {
        strlcpy(s_cfg->wifi_ssid, WIFI_SSID, sizeof(s_cfg->wifi_ssid));
        strlcpy(s_cfg->wifi_pass, WIFI_PASSWORD, sizeof(s_cfg->wifi_pass));
    }
#endif
#ifdef ABS_SERVER
    if (!s_cfg->server[0]) snprintf(s_cfg->server, sizeof(s_cfg->server), "https://%s", ABS_SERVER);
#endif
#ifdef ABS_TOKEN
    if (!s_cfg->access[0]) strlcpy(s_cfg->access, ABS_TOKEN, sizeof(s_cfg->access));
#endif
    ESP_LOGI(TAG, "wifi '%s', server %s, %s", s_cfg->wifi_ssid, s_cfg->server,
             s_cfg->refresh[0] ? "signed in" : (s_cfg->access[0] ? "API key" : "no credentials"));
}

const app_config_t *config_get(void)
{
    return s_cfg;
}

static void write_all(void *unused)
{
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "ssid", s_cfg->wifi_ssid);
    nvs_set_str(h, "pass", s_cfg->wifi_pass);
    nvs_set_str(h, "server", s_cfg->server);
    nvs_set_str(h, "user", s_cfg->username);
    nvs_set_str(h, "access", s_cfg->access);
    nvs_set_str(h, "refresh", s_cfg->refresh);
    nvs_set_u8(h, "skipb", s_cfg->skip_back_s);
    nvs_set_u8(h, "skipf", s_cfg->skip_fwd_s);
    nvs_commit(h);
    nvs_close(h);
    if (nvs_open("ui", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "rot180", s_cfg->rotate180);
    nvs_commit(h);
    nvs_close(h);
}

void config_save(const app_config_t *cfg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (cfg != s_cfg) *s_cfg = *cfg;
    flash_safe(write_all, NULL);
    xSemaphoreGive(s_lock);
}

void config_set_tokens(const char *access, const char *refresh)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cfg->access, access, sizeof(s_cfg->access));
    if (refresh) strlcpy(s_cfg->refresh, refresh, sizeof(s_cfg->refresh));
    flash_safe(write_all, NULL);
    xSemaphoreGive(s_lock);
}

void config_set_rotate(bool rotate180)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg->rotate180 = rotate180;
    flash_safe(write_all, NULL);
    xSemaphoreGive(s_lock);
}

bool config_complete(void)
{
    return s_cfg->wifi_ssid[0] && s_cfg->server[0] && s_cfg->access[0];
}

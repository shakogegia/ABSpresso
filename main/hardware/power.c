#include "power.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "board.h"
#include "download.h"
#include "driver/rtc_io.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "config.h"
#include "portal.h"
#include "player.h"
#include "ui.h"

static const char *TAG = "power";

#define PIN_TOUCH_INT GPIO_NUM_4  // pulses low on a touch, even in the controller's low-power scan
#define PIN_BOOT      GPIO_NUM_0  // BOOT button, low when pressed
#define POLL_MS       100
#define DIM_PERCENT   25          // of the configured brightness
#define LONG_PRESS_MS 2000        // BOOT held this long: sleep

typedef enum { SCREEN_ON, SCREEN_DIM, SCREEN_OFF } screen_t;

static power_config_t s_cfg = {.brightness = 80, .screen_off_s = 60, .sleep_min = 10};
static volatile int64_t s_last_touch_us;
static volatile bool s_swallow;     // hold back the touch that woke the screen until it lifts
static volatile screen_t s_screen = SCREEN_ON;
static int64_t s_off_since_us;
static esp_pm_lock_handle_t s_cpu_lock;  // full CPU speed while the screen is on

/* ---------- settings ---------- */

static void load_config(void)
{
    nvs_handle_t h;
    if (nvs_open("power", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t b;
    uint16_t off, sleep;
    if (nvs_get_u8(h, "bright", &b) == ESP_OK) s_cfg.brightness = b;
    if (nvs_get_u16(h, "off", &off) == ESP_OK) s_cfg.screen_off_s = off;
    if (nvs_get_u16(h, "sleep", &sleep) == ESP_OK) s_cfg.sleep_min = sleep;
    nvs_close(h);
}

void power_get_config(power_config_t *out)
{
    *out = s_cfg;
}

static void save_config(void *unused)
{
    nvs_handle_t h;
    if (nvs_open("power", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bright", s_cfg.brightness);
        nvs_set_u16(h, "off", s_cfg.screen_off_s);
        nvs_set_u16(h, "sleep", s_cfg.sleep_min);
        nvs_commit(h);
        nvs_close(h);
    }
}

void power_set_config(const power_config_t *cfg)
{
    s_cfg = *cfg;
    if (s_screen == SCREEN_ON) board_set_backlight(s_cfg.brightness);
    s_last_touch_us = esp_timer_get_time();
    flash_safe(save_config, NULL);
}

/* ---------- screen ---------- */

static int dim_after_s(void)
{
    // Dim for the last half of the screen-off timeout (never dim when it never turns off).
    return s_cfg.screen_off_s ? s_cfg.screen_off_s / 2 : 0;
}

static void screen_on(void)
{
    if (s_screen == SCREEN_OFF) {
        esp_pm_lock_acquire(s_cpu_lock);
        board_display_power(true);
        lvgl_port_resume();
        lvgl_port_lock(0);
        lv_obj_invalidate(lv_screen_active());
        lvgl_port_unlock();
        vTaskDelay(pdMS_TO_TICKS(80));  // let a fresh frame reach the panel before lighting it
    }
    board_set_backlight(s_cfg.brightness);
    s_screen = SCREEN_ON;
}

static void screen_dim(void)
{
    board_set_backlight(LV_MAX(4, s_cfg.brightness * DIM_PERCENT / 100));
    s_screen = SCREEN_DIM;
}

static void screen_off(void)
{
    board_set_backlight(0);
    lvgl_port_stop();
    vTaskDelay(pdMS_TO_TICKS(60));  // let any frame in flight finish
    board_display_power(false);
    esp_pm_lock_release(s_cpu_lock);
    s_screen = SCREEN_OFF;
    s_off_since_us = esp_timer_get_time();
    ESP_LOGI(TAG, "screen off");
}

static volatile bool s_keep_awake;

void power_keep_awake(bool on)
{
    s_keep_awake = on;
}

bool power_filter_touch(bool pressed)
{
    if (s_swallow) {
        if (!pressed) s_swallow = false;
        return false;
    }
    if (pressed) {
        s_last_touch_us = esp_timer_get_time();
        if (s_screen == SCREEN_DIM) screen_on();  // a dimmed screen is still usable: let it through
    }
    return pressed;
}

/* ---------- deep sleep ---------- */

static bool busy(void)
{
    player_status_t st;
    player_get_status(&st);
    const bool playing = st.state == PLAYER_PLAYING || st.state == PLAYER_BUFFERING || st.state == PLAYER_LOADING;
    // With a USB host attached we're on power, and sleeping would drop the console and flashing.
    return playing || download_busy() || usb_serial_jtag_is_connected();
}

static void deep_sleep(void)
{
    // Both wake pins must be idle (high) or we'd wake straight away.
    if (!gpio_get_level(PIN_TOUCH_INT) || !gpio_get_level(PIN_BOOT)) return;
    ESP_LOGI(TAG, "deep sleep; touch the screen or press BOOT to wake");
    player_stop();
    vTaskDelay(pdMS_TO_TICKS(500));  // let the player close its session
    esp_wifi_stop();
    board_prepare_deep_sleep();
    const gpio_num_t pins[] = {PIN_TOUCH_INT, PIN_BOOT};
    for (int i = 0; i < 2; i++) {
        rtc_gpio_pullup_en(pins[i]);
        rtc_gpio_pulldown_dis(pins[i]);
    }
    esp_sleep_enable_ext1_wakeup_io((1ULL << PIN_TOUCH_INT) | (1ULL << PIN_BOOT), ESP_EXT1_WAKEUP_ANY_LOW);
    esp_deep_sleep_start();
}

/* ---------- BOOT button ---------- */

// The only free physical button (RST is a hardware reset). Polled with everything else:
// a short press plays/pauses (waking the screen), holding it for 2 s puts the device to sleep.
static void boot_button(int64_t now)
{
    static bool armed;          // false until BOOT is seen released: the press that woke us doesn't count
    static int64_t down_since;  // 0 while released
    static bool long_press;
    const bool down = !gpio_get_level(PIN_BOOT);
    if (!armed) {
        armed = !down;
        return;
    }
    if (down) {
        if (!down_since) {
            down_since = now;
        } else if (!long_press && now - down_since >= LONG_PRESS_MS * 1000LL) {
            long_press = true;
            s_last_touch_us = now;
            if (s_screen != SCREEN_ON) screen_on();
            ESP_LOGI(TAG, "BOOT held: sleeping on release");
            ui_show_notice(LV_SYMBOL_POWER, "Release to sleep");
        }
        return;
    }
    if (!down_since) return;
    const bool was_long = long_press;
    down_since = 0;
    long_press = false;
    if (was_long) {
        // Asked for, so no "busy" check: playback stops and we sleep even on USB power.
        deep_sleep();
        ui_show_notice(NULL, NULL);  // still here: the screen was being touched
        return;
    }
    ESP_LOGI(TAG, "BOOT pressed: play/pause");
    s_last_touch_us = now;
    if (s_screen != SCREEN_ON) screen_on();
    lvgl_port_lock(0);
    ui_toggle_play();
    lvgl_port_unlock();
}

/* ---------- task ---------- */

static void power_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        const int64_t now = esp_timer_get_time();
        // Setup shows its progress on screen while you're busy on the phone: stay awake.
        if (portal_active() || s_keep_awake) s_last_touch_us = now;
        boot_button(now);
        const int64_t idle_s = (now - s_last_touch_us) / 1000000;

        if (s_screen == SCREEN_OFF) {
            // LVGL is paused, so watch the touch controller here. Require the press on two polls in
            // a row, so a stray reading can't light the screen.
            static int presses;
            presses = board_touch_pressed() ? presses + 1 : 0;
            if (presses >= 2) {
                presses = 0;
                s_swallow = true;
                s_last_touch_us = now;
                screen_on();
                ESP_LOGI(TAG, "screen on (touch)");
                continue;
            }
            if (s_cfg.sleep_min && (now - s_off_since_us) / 60000000 >= s_cfg.sleep_min && !busy()) {
                deep_sleep();
                s_off_since_us = now;  // couldn't sleep yet (a pin was active): try again later
            }
            continue;
        }
        if (s_cfg.screen_off_s && idle_s >= s_cfg.screen_off_s) {
            screen_off();
        } else if (s_screen == SCREEN_ON && dim_after_s() && idle_s >= dim_after_s()) {
            screen_dim();
        }
    }
}

void power_init(void)
{
    load_config();
    // Full speed while the screen is on (smooth UI); down to 80 MHz when it's off. No automatic
    // light sleep: it would stop the backlight PWM and USB console.
    const esp_pm_config_t pm = {.max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false};
    if (esp_pm_configure(&pm) != ESP_OK) ESP_LOGW(TAG, "frequency scaling unavailable");
    esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "screen", &s_cpu_lock);
    esp_pm_lock_acquire(s_cpu_lock);

    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) ESP_LOGI(TAG, "woke from deep sleep");
    // The wake pins are left in RTC mode after a deep sleep; make them plain inputs again.
    const gpio_num_t pins[] = {PIN_TOUCH_INT, PIN_BOOT};
    for (int i = 0; i < 2; i++) {
        if (rtc_gpio_is_valid_gpio(pins[i])) rtc_gpio_deinit(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
    }
    s_last_touch_us = esp_timer_get_time();
    board_set_backlight(s_cfg.brightness);
    xTaskCreatePinnedToCore(power_task, "power", 3072, NULL, 3, NULL, 0);
}

#include "battery.h"

#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "battery";

#define ADC_CH        ADC_CHANNEL_7  // GPIO8
#define DIVIDER       3.0f           // 100k / (100k + 200k)
#define SAMPLE_MS     1000
#define HISTORY_EVERY 10             // samples between history entries (10 s)
#define HISTORY_LEN   18             // 3 minutes of history
#define JUMP_V        0.04f          // plug/unplug step over ~30 s
#define TREND_V       0.010f         // rise/fall over 3 minutes that counts as a trend
#define CV_V          4.17f          // at or above: the charger is holding the charge voltage
#define FULL_MIN_V    4.05f          // on power, flat between this and CV_V: charging has finished
#define FLAT_V        0.005f         // "flat" over the 3-minute window

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static SemaphoreHandle_t s_lock;
static battery_status_t s_status;

// Typical single-cell LiPo voltage -> charge, lightly loaded.
static const struct { float v; int pct; } CURVE[] = {
    {4.20f, 100}, {4.15f, 95}, {4.11f, 90}, {4.08f, 85}, {4.02f, 80}, {3.98f, 75}, {3.95f, 70},
    {3.91f, 65},  {3.87f, 60}, {3.85f, 55}, {3.84f, 50}, {3.82f, 45}, {3.80f, 40}, {3.79f, 35},
    {3.77f, 30},  {3.75f, 25}, {3.73f, 20}, {3.71f, 15}, {3.69f, 10}, {3.61f, 5},  {3.27f, 0},
};

static int percent_from(float v)
{
    const int n = sizeof(CURVE) / sizeof(CURVE[0]);
    if (v >= CURVE[0].v) return 100;
    for (int i = 1; i < n; i++) {
        if (v >= CURVE[i].v) {
            float f = (v - CURVE[i].v) / (CURVE[i - 1].v - CURVE[i].v);
            return (int)lroundf(CURVE[i].pct + f * (CURVE[i - 1].pct - CURVE[i].pct));
        }
    }
    return 0;
}

static float read_volts(void)
{
    int sum = 0, n = 0;
    for (int i = 0; i < 8; i++) {
        int raw, mv;
        if (adc_oneshot_read(s_adc, ADC_CH, &raw) != ESP_OK) continue;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) sum += mv;
        else sum += raw * 3100 / 4095;
        n++;
    }
    return n ? DIVIDER * sum / n / 1000.0f : 0;
}

static void battery_task(void *arg)
{
    float ema = read_volts(), history[HISTORY_LEN];
    int hist_n = 0, tick = 0;
    float trend = 0;
    bool trend_valid = false;
    bool volt_power = false;  // external power inferred from the voltage (e.g. a wall charger)
    bool was_power = false, full = false;
    for (;;) {
        ema += 0.2f * (read_volts() - ema);

        if (++tick % HISTORY_EVERY == 0) {
            if (hist_n == HISTORY_LEN) memmove(history, history + 1, (HISTORY_LEN - 1) * sizeof(float));
            else hist_n++;
            history[hist_n - 1] = ema;

            // Plug/unplug: a step within the last ~30 s.
            if (hist_n >= 4) {
                float step = ema - history[hist_n - 4];
                if (step > JUMP_V) volt_power = true;
                else if (step < -JUMP_V) volt_power = false;
            }
            // Otherwise the trend over the window.
            if (hist_n == HISTORY_LEN) {
                trend = ema - history[0];
                trend_valid = true;
                if (trend > TREND_V) volt_power = true;
                else if (trend < -TREND_V && ema < CV_V) volt_power = false;
            }
            if (tick % 60 == 0) {
                ESP_LOGI(TAG, "%.3f V (3 min %+.3f), %d%%, usb host %d, %s", ema, trend, percent_from(ema),
                         usb_serial_jtag_is_connected(),
                         !was_power ? "on battery" : (full ? "full" : "charging"));
            }
        }

        // A USB host (computer, powered hub) talking to us means external power. A plain wall
        // charger isn't visible that way, so the voltage-based inference above covers it.
        const bool on_power = usb_serial_jtag_is_connected() || volt_power;
        if (on_power != was_power) {
            // Plugged in or unplugged: judge "full" on readings from this state only.
            was_power = on_power;
            full = false;
            hist_n = 0;
            trend = 0;
            trend_valid = false;
        }
        // The charger holds the cell at ~4.2 V while charging, then stops at full and the cell
        // rests a little lower and flat. So on power: flat, high, but below the charge voltage
        // for the whole window means full; a clear rise again means it's charging again.
        if (on_power && trend_valid) {
            if (!full && ema >= FULL_MIN_V && ema < CV_V && fabsf(trend) < FLAT_V) full = true;
            else if (full && trend > TREND_V) full = false;
        }

        battery_status_t st = {
            .present = ema > 2.5f && ema < 4.6f,
            .volts = ema,
            .percent = percent_from(ema),
            .charging = on_power && !full,
            .charged = on_power && full,
        };
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_status = st;
        xSemaphoreGive(s_lock);
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

void battery_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    const adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC unavailable");
        return;
    }
    const adc_oneshot_chan_cfg_t ch = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    adc_oneshot_config_channel(s_adc, ADC_CH, &ch);
    const adc_cali_curve_fitting_config_t cali = {
        .unit_id = ADC_UNIT_1, .chan = ADC_CH, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (adc_cali_create_scheme_curve_fitting(&cali, &s_cali) != ESP_OK) s_cali = NULL;
    // Stack in PSRAM: the task only reads the ADC (internal RAM is the scarce resource).
    xTaskCreatePinnedToCoreWithCaps(battery_task, "battery", 3072, NULL, 2, NULL, 0, MALLOC_CAP_SPIRAM);
}

#ifdef BATT_DEMO
int g_batt_demo = -1;  // capture builds: force a state to screenshot it
#endif

void battery_get(battery_status_t *out)
{
#ifdef BATT_DEMO
    if (g_batt_demo >= 0) {
        static const battery_status_t demo[] = {
            {true, 3.70f, 18, false, false}, {true, 3.95f, 72, false, false},
            {true, 4.00f, 72, true, false},  {true, 4.13f, 92, false, true},
        };
        *out = demo[g_batt_demo];
        return;
    }
#endif
    if (!s_lock) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_lock);
}

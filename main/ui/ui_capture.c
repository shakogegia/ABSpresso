// Debug-only screen capture (build with UI_CAPTURE defined). Runs a scripted tour of the UI and
// streams each frame over the console as base64 RGB565, for tools/capture_to_media.py to turn
// into PNGs and GIFs.
//
// LVGL's clock is replaced by a virtual one that only moves when the script advances it, so
// animations are captured at exact points even though sending a frame takes seconds.

#ifdef UI_CAPTURE

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "mbedtls/base64.h"
#include "player.h"
#include "ui_priv.h"

void home_debug_next_row(void);
void library_debug_view(int view);
void library_debug_step(int delta);
void library_debug_scrub(int value, bool active);

#define ROWS_PER_LINE 3

static volatile uint32_t s_vtime;
static int s_frame;

static uint32_t virtual_tick(void)
{
    return s_vtime;
}

// Moves LVGL's clock forward, running its timers (animations, refresh) as it goes.
static void advance(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        lvgl_port_lock(0);
        s_vtime += 10;
        lv_timer_handler();
        lvgl_port_unlock();
    }
}

// Real waiting (for downloads, the player) with the UI kept in step.
static void wait_real(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 50) {
        advance(50);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// Emits one frame: "SHOT <name> <w> <h> <hold_ms>", base64 lines "D ...", then "END".
static void shot(const char *name, int hold_ms)
{
    lvgl_port_lock(0);
    lv_obj_update_layout(lv_screen_active());
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    lvgl_port_unlock();
    if (!buf) {
        printf("SHOTFAIL %s\n", name);
        return;
    }
    const int w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    printf("SHOT %s %d %d %d\n", name, w, h, hold_ms);
    static uint8_t rows[360 * 2 * ROWS_PER_LINE];
    static unsigned char line[sizeof(rows) * 4 / 3 + 8];
    for (int y = 0; y < h; y += ROWS_PER_LINE) {
        int n = (h - y) < ROWS_PER_LINE ? h - y : ROWS_PER_LINE;
        for (int r = 0; r < n; r++) {
            memcpy(rows + r * w * 2, buf->data + (y + r) * stride, w * 2);
        }
        size_t olen = 0;
        mbedtls_base64_encode(line, sizeof(line), &olen, rows, n * w * 2);
        line[olen] = 0;
        printf("D %s\n", line);
        if ((y / ROWS_PER_LINE) % 8 == 7) {
            // Let the idle task run, or the task watchdog fires and prints into the stream.
            vTaskDelay(1);
        }
    }
    printf("END %s\n", name);
    fflush(stdout);
    lvgl_port_lock(0);
    lv_draw_buf_destroy(buf);
    lvgl_port_unlock();
}

static void frame(const char *gif, int hold_ms)
{
    char name[48];
    snprintf(name, sizeof(name), "%s_%03d", gif, s_frame++);
    shot(name, hold_ms);
}

static void locked(void (*fn)(int), int arg)
{
    lvgl_port_lock(0);
    fn(arg);
    lvgl_port_unlock();
}

static void show_page(int p) { ui_show_page((ui_page_t)p); }
static void show_sheet(int i) { ui_sheet_show(i); }
static void hide_sheet(int unused)
{
    ui_sheet_hide();
    ui_episodes_hide();  // in a podcast library the "sheet" is the episode list
}
static void open_book(int i) { ui_open_book(i); }
static void next_row(int unused) { home_debug_next_row(); }
static void lib_view(int v) { library_debug_view(v); }
static void lib_step(int d) { library_debug_step(d); }
static void scrub_on(int v) { library_debug_scrub(v, true); }
static void scrub_off(int v) { library_debug_scrub(v, false); }

static void __attribute__((unused)) capture_task(void *arg)
{
    esp_log_level_set("*", ESP_LOG_ERROR);
    lvgl_port_lock(0);
    s_vtime = lv_tick_get();
    lv_tick_set_cb(virtual_tick);
    lvgl_port_unlock();
    printf("CAPTURE BEGIN\n");

#ifdef PORTAL_TEST
    locked(show_page, PAGE_LIBRARY);
    locked(lib_view, 3);  // Settings
    advance(300);
    shot("settings", 0);
    lvgl_port_lock(0);
    ui_setup_show();
    lvgl_port_unlock();
    wait_real(4500);  // the setup network starts after a ~3 s scan
    shot("setup", 0);
    printf("CAPTURE DONE\n");
    vTaskDelete(NULL);
#endif
#ifdef BATT_DEMO
    extern int g_batt_demo;
    locked(show_page, PAGE_HOME);
    for (int d = 0; d < 4; d++) {
        g_batt_demo = d;
        advance(1100);
        char name[16];
        snprintf(name, sizeof(name), "batt_%d", d);
        shot(name, 0);
    }
    g_batt_demo = -1;
    printf("CAPTURE DONE\n");
    vTaskDelete(NULL);
#endif
    // Stills.
    locked(show_sheet, g_continue.count ? g_continue.idx[0] : 0);
    wait_real(2500);
    shot("book_sheet", 0);
    locked(hide_sheet, 0);

    locked(show_page, PAGE_HOME);
    wait_real(5000);
    shot("home_continue", 0);
    locked(next_row, 0);
    wait_real(4000);
    shot("home_recent", 0);

    locked(show_page, PAGE_LIBRARY);
    locked(lib_view, 1);  // Books
    advance(200);
    shot("library_list", 0);
    locked(scrub_on, 430);
    advance(200);
    shot("library_scrub", 0);
    locked(scrub_off, 430);
    locked(lib_view, 2);  // Authors
    advance(200);
    shot("library_authors", 0);
    locked(lib_view, 3);  // Settings
    advance(200);
    shot("library_settings", 0);
    locked(lib_view, 0);  // Covers
    wait_real(4000);
    shot("library_covers", 0);

    locked(show_page, PAGE_PLAYER);
    wait_real(3000);
    shot("player_resume", 0);

    // GIF: browsing the cover carousel.
    locked(show_page, PAGE_LIBRARY);
    locked(lib_view, 0);
    wait_real(1500);
    s_frame = 0;
    frame("carousel", 900);
    for (int i = 0; i < 5; i++) {
        locked(lib_step, 1);
        for (int f = 0; f < 5; f++) {
            advance(40);
            frame("carousel", 40);
        }
        wait_real(900);  // let the newly visible covers arrive
        frame("carousel", 900);
    }

    // GIF: resuming a book from Now Playing.
    locked(show_page, PAGE_PLAYER);
    wait_real(1500);
    s_frame = 0;
    frame("playing", 1200);
    if (g_continue.count) {
        locked(open_book, g_continue.idx[0]);
        for (int i = 0; i < 14; i++) {
            wait_real(700);
            frame("playing", 700);
        }
        player_seek_relative(30);
        for (int i = 0; i < 4; i++) {
            wait_real(700);
            frame("playing", 700);
        }
        player_toggle();  // pause
        wait_real(1000);
        frame("playing", 1500);
        player_stop();
    }

    printf("CAPTURE DONE\n");
    lvgl_port_lock(0);
    lv_tick_set_cb(NULL);
    lvgl_port_unlock();
    esp_log_level_set("*", ESP_LOG_INFO);
    vTaskDelete(NULL);
}

#ifdef CAPTURE_LIBS
#include <dirent.h>
#include "storage.h"
#include "ui.h"
void settings_debug_open_picker(void);
void settings_debug_close_picker(void);
void episodes_debug_play(int i);

static void open_picker(int unused) { settings_debug_open_picker(); }
static void close_picker(int unused) { settings_debug_close_picker(); }
static void show_episodes(int i) { ui_episodes_show(i); }
static void play_ep(int i) { episodes_debug_play(i); }

// Picks the first library of the given kind and asks the main loop to switch to it.
static void request_kind(int podcast)
{
    int n;
    const char *sel;
    const abs_library_t *libs = ui_libraries(&n, &sel);
    for (int i = 0; i < n; i++) {
        if (libs[i].podcast == (bool)podcast) {
            printf("LIBTEST switching to %s\n", libs[i].name);
            ui_request_library(libs[i].id);
            return;
        }
    }
}

static void list_cache(void)
{
    DIR *d = opendir(STORAGE_ROOT "/lib");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char sub[320];
        const char *kids[] = {"covers", "episodes"};
        printf("LIBTEST cache lib/%.8s:", e->d_name);
        for (int k = 0; k < 2; k++) {
            snprintf(sub, sizeof(sub), STORAGE_ROOT "/lib/%s/%s", e->d_name, kids[k]);
            DIR *c = opendir(sub);
            int n = 0;
            while (c && readdir(c)) n++;
            if (c) closedir(c);
            printf(" %s=%d", kids[k], n);
        }
        snprintf(sub, sizeof(sub), STORAGE_ROOT "/lib/%s/items.json", e->d_name);
        FILE *f = fopen(sub, "rb");
        long sz = 0;
        if (f) { fseek(f, 0, SEEK_END); sz = ftell(f); fclose(f); }
        printf(" items.json=%ld\n", sz);
    }
    if (d) closedir(d);
}

static void libs_task(void *arg)
{
    esp_log_level_set("*", ESP_LOG_ERROR);
    esp_log_level_set("catalog", ESP_LOG_INFO);
    esp_log_level_set("abs", ESP_LOG_INFO);
    esp_log_level_set("player", ESP_LOG_INFO);
    lvgl_port_lock(0);
    s_vtime = lv_tick_get();
    lv_tick_set_cb(virtual_tick);
    lvgl_port_unlock();
    printf("CAPTURE BEGIN\n");

    locked(show_page, PAGE_LIBRARY);
    locked(lib_view, 3);
    wait_real(500);
    shot("settings", 0);
    locked(open_picker, 0);
    wait_real(300);
    shot("library_picker", 0);

    locked(close_picker, 0);  // a real tap closes it
    locked(request_kind, 1);  // Podcasts
    wait_real(15000);
    locked(show_page, PAGE_HOME);
    wait_real(4000);
    shot("podcast_home", 0);
    locked(show_page, PAGE_LIBRARY);
    locked(lib_view, 1);  // Shows
    wait_real(1500);
    shot("podcast_shows", 0);
    locked(show_episodes, g_alpha.count ? g_alpha.idx[0] : 0);
    wait_real(8000);
    shot("podcast_episodes", 0);
    locked(play_ep, 0);
    wait_real(9000);
    shot("podcast_playing", 0);
    player_stop();
    wait_real(1500);

    locked(request_kind, 0);  // back to Audiobooks
    wait_real(15000);
    locked(show_page, PAGE_HOME);
    wait_real(3000);
    shot("books_home_again", 0);
    list_cache();

    printf("CAPTURE DONE\n");
    vTaskDelete(NULL);
}
#endif

void ui_capture_start(void)
{
    // PSRAM stack: internal RAM is tight, and this task never touches flash.
#ifdef CAPTURE_LIBS
    xTaskCreatePinnedToCoreWithCaps(libs_task, "capture", 8192, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
#else
    xTaskCreatePinnedToCoreWithCaps(capture_task, "capture", 8192, NULL, 2, NULL, 1, MALLOC_CAP_SPIRAM);
#endif
}

#endif

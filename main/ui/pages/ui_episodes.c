// Podcast episode list: a full-screen overlay opened by tapping (or long-pressing) a show. Episodes
// load in a background task from the server, falling back to the copy cached on the SD card
// (<library cache dir>/episodes/<item id>.json) when offline.

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "catalog.h"
#include "esp_heap_caps.h"
#include "player.h"
#include "storage.h"
#include "ui_priv.h"
#include "wifi.h"

#define MAX_ROWS 150  // very long feeds: show the in-progress and newest episodes

static lv_obj_t *s_overlay, *s_title, *s_status, *s_list;
static int s_book = -1;
static bool s_built_list;

// Loader state, shared with the task under s_lock.
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static char s_want[40];            // item to load
static char s_have_id[40];         // item the loaded episodes belong to
static abs_episode_t *s_eps;
static int s_ep_count;
static bool s_ready, s_failed, s_offline;

/* ---------- loader ---------- */

static void cache_path(const char *item_id, char *out, size_t len)
{
    char dir[96];
    catalog_dir(dir, sizeof(dir));
    if (!dir[0]) {
        out[0] = 0;
        return;
    }
    snprintf(out, len, "%s/episodes", dir);
    storage_mkdirs(out);
    strlcat(out, "/", len);
    strlcat(out, item_id, len);
    strlcat(out, ".json", len);
}

static void loader_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        char id[40], path[160];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(id, s_want, sizeof(id));
        xSemaphoreGive(s_lock);
        cache_path(id, path, sizeof(path));

        abs_episode_t *eps = NULL;
        int n = 0;
        bool offline = false;
        if (wifi_is_connected() && abs_get_episodes(id, &eps, &n) == ESP_OK) {
            char *json = path[0] ? abs_episodes_to_json(eps, n) : NULL;
            if (json) storage_write_file(path, json, strlen(json));
            free(json);
        } else if (path[0]) {
            char *json = storage_read_file(path, NULL);
            if (json && abs_episodes_from_json(json, &eps, &n) == ESP_OK) offline = true;
            free(json);
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        abs_free_episodes(s_eps, s_ep_count);
        s_eps = eps;
        s_ep_count = n;
        strlcpy(s_have_id, id, sizeof(s_have_id));
        s_failed = eps == NULL;
        s_offline = offline;
        s_ready = true;
        xSemaphoreGive(s_lock);
    }
}

/* ---------- UI ---------- */

static void close_overlay(void)
{
    s_book = -1;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void on_close(lv_event_t *e) { close_overlay(); }

static void play_episode(int i)
{
    if (s_book < 0 || i < 0 || i >= s_ep_count) return;
    const abs_book_t *show = &g_books[s_book];
    player_open_episode(show->id, s_eps[i].id, s_eps[i].title, show->title);
    playing_prepare(s_book);
    close_overlay();
    ui_show_page(PAGE_PLAYER);
}

static void on_episode(lv_event_t *e)
{
    play_episode((int)(intptr_t)lv_event_get_user_data(e));
}

#ifdef UI_CAPTURE
void episodes_debug_play(int i)
{
    play_episode(i);
}
#endif

static void fmt_sub(const abs_episode_t *ep, char *buf, size_t len)
{
    char date[24] = "";
    if (ep->published_at > 0) {
        time_t t = (time_t)(ep->published_at / 1000);
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(date, sizeof(date), "%d %b %Y", &tm);
    }
    int m = (int)(ep->duration / 60);
    char dur[16];
    if (m >= 60) snprintf(dur, sizeof(dur), "%dh %02dm", m / 60, m % 60);
    else snprintf(dur, sizeof(dur), "%d min", m);
    if (ep->finished) snprintf(buf, len, "%s  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_OK, date, dur);
    else if (ep->current_time > 0) snprintf(buf, len, "%s  " LV_SYMBOL_BULLET "  %s  " LV_SYMBOL_BULLET "  %d%%", date, dur, (int)(ep->progress * 100) ?: 1);
    else snprintf(buf, len, "%s  " LV_SYMBOL_BULLET "  %s", date, dur);
}

static void build_rows(void)
{
    lv_obj_clean(s_list);
    const int n = s_ep_count < MAX_ROWS ? s_ep_count : MAX_ROWS;
    for (int i = 0; i < n; i++) {
        const abs_episode_t *ep = &s_eps[i];
        lv_obj_t *btn = lv_list_add_button(s_list, NULL, NULL);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_bg_color(btn, COLOR_CARD, 0);
        lv_obj_set_style_bg_color(btn, COLOR_ACCENT, LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 10, 0);
        lv_obj_set_style_pad_row(btn, 2, 0);

        lv_obj_t *t = lv_label_create(btn);
        lv_label_set_text(t, ep->title);
        ui_one_line(t, &ui_font_16);
        lv_obj_set_style_text_color(t, COLOR_TEXT, 0);

        char sub[80];
        fmt_sub(ep, sub, sizeof(sub));
        lv_obj_t *s = lv_label_create(btn);
        lv_label_set_text(s, sub);
        ui_one_line(s, &ui_font_14);
        const bool active = ep->current_time > 0 && !ep->finished;
        lv_obj_set_style_text_color(s, active ? COLOR_ACCENT : COLOR_MUTED, 0);

        lv_obj_add_event_cb(btn, on_episode, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
}

void ui_episodes_refresh(void)
{
    if (s_book < 0 || s_built_list) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ready = s_ready && strcmp(s_have_id, g_books[s_book].id) == 0;
    const bool failed = s_failed, offline = s_offline;
    xSemaphoreGive(s_lock);
    if (!ready) return;
    s_built_list = true;
    if (failed) {
        lv_label_set_text(s_status, "Couldn't load episodes");
        return;
    }
    // The UI reads s_eps only after this point, and the task only replaces it on the next request.
    build_rows();
    char status[48];
    if (offline) snprintf(status, sizeof(status), "Offline - saved list");
    else snprintf(status, sizeof(status), "%d episode%s", s_ep_count, s_ep_count == 1 ? "" : "s");
    lv_label_set_text(s_status, status);
}

void ui_episodes_show(int book_index)
{
    if (book_index < 0 || book_index >= g_book_count) return;
    s_book = book_index;
    s_built_list = false;
    lv_label_set_text(s_title, g_books[book_index].title);
    lv_label_set_text(s_status, "Loading episodes...");
    lv_obj_clean(s_list);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_want, g_books[book_index].id, sizeof(s_want));
    s_ready = false;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
}

void ui_episodes_hide(void)
{
    close_overlay();
}

bool ui_episodes_visible(void)
{
    return s_book >= 0;
}

void episodes_build(lv_obj_t *scr)
{
    s_lock = xSemaphoreCreateMutex();
    // Stack in PSRAM: network and SD work only, never the internal flash.
    xTaskCreatePinnedToCoreWithCaps(loader_task, "episodes", 8192, NULL, 3, &s_task, 0, MALLOC_CAP_SPIRAM);

    // Full-screen and clickable, so it swallows touches meant for the page underneath.
    s_overlay = ui_page_container(scr);
    lv_obj_set_style_bg_color(s_overlay, COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *close = ui_round_button(s_overlay, 36, LV_SYMBOL_CLOSE, &ui_font_16, on_close, NULL);
    lv_obj_align(close, LV_ALIGN_CENTER, 0, -128);

    s_title = ui_label(s_overlay, &ui_font_16, COLOR_TEXT, 230);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -92);
    s_status = ui_label(s_overlay, &ui_font_14, COLOR_MUTED, 220);
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, -72);

    s_list = lv_list_create(s_overlay);
    lv_obj_set_size(s_list, 240, 196);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 44);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 6, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

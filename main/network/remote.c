// Remote control web page and JSON API (see remote.h).

#include "remote.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "mdns.h"

#include "catalog.h"
#include "config.h"
#include "cover.h"
#include "download.h"
#include "player.h"
#include "power.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "remote";

extern const char remote_page_start[] asm("_binary_remote_page_html_start");
extern const char remote_page_end[] asm("_binary_remote_page_html_end");

static httpd_handle_t s_httpd;

/* ---------- JSON into a PSRAM buffer ---------- */

// Responses are built as text in PSRAM: cJSON would make one small (internal RAM) allocation per
// value, and a large library has thousands.
typedef struct {
    char *p;
    size_t len, cap;
    bool failed;
} buf_t;

static void buf_append(buf_t *b, const char *s, size_t n)
{
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (b->len + n + 1 > cap) cap *= 2;
        char *p = heap_caps_realloc(b->p, cap, MALLOC_CAP_SPIRAM);
        if (!p) {
            b->failed = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void buf_printf(buf_t *b, const char *fmt, ...)
{
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n > 0) buf_append(b, tmp, n < (int)sizeof(tmp) ? n : sizeof(tmp) - 1);
}

// "key":"escaped value"
static void buf_str(buf_t *b, const char *key, const char *val)
{
    buf_printf(b, "\"%s\":\"", key);
    for (const char *s = val ? val : ""; *s; s++) {
        const unsigned char c = *s;
        if (c == '"' || c == '\\') {
            const char esc[2] = {'\\', c};
            buf_append(b, esc, 2);
        } else if (c < 0x20) {
            buf_printf(b, "\\u%04x", c);
        } else {
            buf_append(b, s, 1);
        }
    }
    buf_append(b, "\"", 1);
}

static esp_err_t send_buf(httpd_req_t *req, buf_t *b)
{
    if (b->failed || !b->p) {
        free(b->p);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_send(req, b->p, b->len);
    free(b->p);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ---------- query parameters ---------- */

static bool query(httpd_req_t *req, const char *key, char *out, size_t len)
{
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) return false;
    return httpd_query_key_value(q, key, out, len) == ESP_OK;
}

static bool query_int(httpd_req_t *req, const char *key, int *out)
{
    char v[16];
    if (!query(req, key, v, sizeof(v)) || !v[0]) return false;
    char *end;
    const long n = strtol(v, &end, 10);
    if (*end) return false;
    *out = (int)n;
    return true;
}

// Item and library ids are UUIDs or "li_..." / "lib_..." style: letters, digits, '-' and '_'.
static bool valid_id(const char *id)
{
    if (!id[0]) return false;
    for (const char *s = id; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_') return false;
    }
    return true;
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* ---------- handlers ---------- */

static esp_err_t on_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, remote_page_start, remote_page_end - remote_page_start - 1);
}

static const char *dl_name(dl_state_t s)
{
    switch (s) {
    case DL_QUEUED:   return "queued";
    case DL_ACTIVE:   return "active";
    case DL_DONE:     return "done";
    case DL_ERROR:    return "error";
    case DL_REMOVING: return "removing";
    default:          return "none";
    }
}

static const char *state_name(player_state_t s)
{
    switch (s) {
    case PLAYER_LOADING:   return "loading";
    case PLAYER_BUFFERING: return "buffering";
    case PLAYER_PLAYING:   return "playing";
    case PLAYER_PAUSED:    return "paused";
    case PLAYER_FINISHED:  return "finished";
    case PLAYER_ERROR:     return "error";
    default:               return "idle";
    }
}

static esp_err_t on_status(httpd_req_t *req)
{
    player_status_t st;
    player_get_status(&st);
    const app_config_t *cfg = config_get();
    buf_t b = {0};
    buf_append(&b, "{", 1);
    buf_str(&b, "state", state_name(st.state));
    buf_append(&b, ",", 1);
    buf_str(&b, "id", st.item_id);
    buf_append(&b, ",", 1);
    buf_str(&b, "episode", st.episode_id);
    buf_append(&b, ",", 1);
    buf_str(&b, "title", st.title);
    buf_append(&b, ",", 1);
    buf_str(&b, "author", st.author);
    buf_append(&b, ",", 1);
    buf_str(&b, "chapter", st.chapter);
    buf_printf(&b, ",\"chapter_index\":%d,\"chapter_count\":%d", st.chapter_index, st.chapter_count);
    buf_printf(&b, ",\"position\":%.1f,\"duration\":%.1f", st.position, st.duration);
    buf_printf(&b, ",\"chapter_start\":%.1f,\"chapter_end\":%.1f", st.chapter_start, st.chapter_end);
    buf_printf(&b, ",\"volume\":%d,\"buffer\":%d", st.volume, st.buffer_percent);
    buf_printf(&b, ",\"skip_back\":%d,\"skip_fwd\":%d,", cfg->skip_back_s, cfg->skip_fwd_s);
    buf_str(&b, "server", cfg->server);  // the page loads covers from it (they need no sign-in)
    buf_append(&b, "}", 1);
    return send_buf(req, &b);
}

static esp_err_t on_books(httpd_req_t *req)
{
    buf_t b = {0};
    buf_append(&b, "[", 1);
    lvgl_port_lock(0);  // the UI owns the book array and swaps it under this lock
    const abs_book_t *books;
    const int n = ui_book_list(&books);
    for (int i = 0; i < n; i++) {
        const abs_book_t *bk = &books[i];
        buf_append(&b, i ? ",{" : "{", i ? 2 : 1);
        buf_str(&b, "id", bk->id);
        buf_append(&b, ",", 1);
        buf_str(&b, "title", bk->title);
        buf_append(&b, ",", 1);
        buf_str(&b, "author", bk->author);
        buf_printf(&b, ",\"duration\":%.0f,\"progress\":%.3f,\"finished\":%s,\"updated\":%.0f,\"added\":%.0f",
                   bk->duration, bk->progress, bk->finished ? "true" : "false", bk->last_update, bk->added_at);
        int pct;
        const dl_state_t dl = download_state(bk->id, &pct);
        buf_printf(&b, ",\"podcast\":%s,\"dl\":\"%s\",\"dl_pct\":%d}", bk->podcast ? "true" : "false",
                   dl_name(dl), pct);
    }
    lvgl_port_unlock();
    buf_append(&b, "]", 1);
    return send_buf(req, &b);
}

static esp_err_t on_cover(httpd_req_t *req)
{
    char id[48], dir[96], path[160];
    catalog_dir(dir, sizeof(dir));
    if (!query(req, "id", id, sizeof(id)) || !valid_id(id) || !dir[0] || !storage_ready()) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    }
    snprintf(path, sizeof(path), "%s/covers/%s_%d.jpg", dir, id, COVER_THUMB_SIZE);
    size_t len = 0;
    char *jpg = storage_read_file(path, &len);
    if (!jpg) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    const esp_err_t err = httpd_resp_send(req, jpg, len);
    free(jpg);
    return err;
}

static esp_err_t on_toggle(httpd_req_t *req)
{
    lvgl_port_lock(0);
    ui_toggle_play();  // also resumes the latest book when nothing is loaded
    lvgl_port_unlock();
    return send_ok(req);
}

static esp_err_t on_play(httpd_req_t *req)
{
    char id[48];
    if (!query(req, "id", id, sizeof(id)) || !valid_id(id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id");
    }
    lvgl_port_lock(0);
    const bool found = ui_play_item(id);
    lvgl_port_unlock();
    if (!found) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not in this library");
    return send_ok(req);
}

static esp_err_t on_skip(httpd_req_t *req)
{
    int dir;
    if (!query_int(req, "dir", &dir) || dir == 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "dir");
    const app_config_t *cfg = config_get();
    player_seek_relative(dir < 0 ? -cfg->skip_back_s : cfg->skip_fwd_s);
    return send_ok(req);
}

static esp_err_t on_seek(httpd_req_t *req)
{
    int to;
    if (!query_int(req, "to", &to) || to < 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "to");
    player_seek_to(to);
    return send_ok(req);
}

static esp_err_t on_chapter(httpd_req_t *req)
{
    int d;
    if (!query_int(req, "d", &d) || d == 0) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "d");
    player_chapter_step(d < 0 ? -1 : 1);
    return send_ok(req);
}

static esp_err_t on_volume(httpd_req_t *req)
{
    int v;
    if (!query_int(req, "v", &v)) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "v");
    player_set_volume(clamp(v, 0, 100));
    return send_ok(req);
}

static esp_err_t on_stop(httpd_req_t *req)
{
    player_stop();
    return send_ok(req);
}

// Downloads in progress or saved, and the card's space; polled while the library is open.
static esp_err_t on_downloads(httpd_req_t *req)
{
    uint64_t free_b = 0, total_b = 0;
    storage_space(&free_b, &total_b);
    buf_t b = {0};
    buf_printf(&b, "{\"sd\":%s,\"free\":%llu,\"total\":%llu,\"items\":[", storage_ready() ? "true" : "false",
               (unsigned long long)free_b, (unsigned long long)total_b);
    lvgl_port_lock(0);
    const abs_book_t *books;
    const int n = ui_book_list(&books);
    bool first = true;
    for (int i = 0; i < n; i++) {
        int pct;
        const dl_state_t dl = download_state(books[i].id, &pct);
        if (dl == DL_NONE) continue;
        buf_append(&b, first ? "{" : ",{", first ? 1 : 2);
        first = false;
        buf_str(&b, "id", books[i].id);
        buf_printf(&b, ",\"dl\":\"%s\",\"dl_pct\":%d}", dl_name(dl), pct);
    }
    lvgl_port_unlock();
    buf_append(&b, "]}", 2);
    return send_buf(req, &b);
}

// ?id= starts (or retries) a download; ?id=&remove=1 cancels it or deletes the saved copy.
static esp_err_t on_download(httpd_req_t *req)
{
    char id[48];
    if (!query(req, "id", id, sizeof(id)) || !valid_id(id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id");
    }
    int remove = 0;
    query_int(req, "remove", &remove);
    if (!storage_ready()) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no SD card");
    esp_err_t err = ESP_OK;
    lvgl_port_lock(0);
    const abs_book_t *books;
    const int n = ui_book_list(&books);
    const abs_book_t *book = NULL;
    for (int i = 0; i < n && !book; i++) {
        if (strcmp(books[i].id, id) == 0) book = &books[i];
    }
    if (book && remove) {
        player_status_t ps;
        player_get_status(&ps);
        if (strcmp(ps.item_id, id) == 0) player_stop();  // it may be playing from the file
        download_remove(id);
    } else if (book && !book->podcast) {
        err = download_start(book);
    }
    const bool podcast = book && book->podcast;
    lvgl_port_unlock();
    if (!book) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not in this library");
    if (podcast && !remove) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "podcasts can't be downloaded");
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "couldn't queue it");
    return send_ok(req);
}

static esp_err_t on_get_settings(httpd_req_t *req)
{
    power_config_t pc;
    power_get_config(&pc);
    const app_config_t *cfg = config_get();
    buf_t b = {0};
    buf_printf(&b, "{\"brightness\":%d,\"screen_off_s\":%d,\"sleep_min\":%d", pc.brightness, pc.screen_off_s,
               pc.sleep_min);
    buf_printf(&b, ",\"skip_back_s\":%d,\"skip_fwd_s\":%d,\"rotate180\":%s,", cfg->skip_back_s, cfg->skip_fwd_s,
               cfg->rotate180 ? "true" : "false");
    lvgl_port_lock(0);
    int n;
    const char *selected;
    const abs_library_t *libs = ui_library_list(&n, &selected);
    buf_str(&b, "library", selected);
    buf_append(&b, ",\"libraries\":[", 14);
    for (int i = 0; i < n; i++) {
        buf_append(&b, i ? ",{" : "{", i ? 2 : 1);
        buf_str(&b, "id", libs[i].id);
        buf_append(&b, ",", 1);
        buf_str(&b, "name", libs[i].name);
        buf_printf(&b, ",\"podcast\":%s}", libs[i].podcast ? "true" : "false");
    }
    lvgl_port_unlock();
    buf_append(&b, "]}", 2);
    return send_buf(req, &b);
}

// Any subset of: brightness, screen_off_s, sleep_min, skip_back_s, skip_fwd_s, rotate180 (0/1).
static esp_err_t on_set_settings(httpd_req_t *req)
{
    power_config_t pc;
    power_get_config(&pc);
    bool power_changed = false;
    int v;
    if (query_int(req, "brightness", &v)) {
        pc.brightness = clamp(v, 10, 100);
        power_changed = true;
    }
    if (query_int(req, "screen_off_s", &v)) {
        pc.screen_off_s = clamp(v, 0, 3600);
        power_changed = true;
    }
    if (query_int(req, "sleep_min", &v)) {
        pc.sleep_min = clamp(v, 0, 240);
        power_changed = true;
    }
    const app_config_t *cfg = config_get();
    int back = cfg->skip_back_s, fwd = cfg->skip_fwd_s;
    bool skip_changed = query_int(req, "skip_back_s", &back);
    skip_changed = query_int(req, "skip_fwd_s", &fwd) || skip_changed;
    int rotate;
    const bool rotate_changed = query_int(req, "rotate180", &rotate);

    lvgl_port_lock(0);  // the Settings page does these from LVGL events, under this lock
    if (power_changed) power_set_config(&pc);
    if (skip_changed) config_set_skip(clamp(back, 5, 300), clamp(fwd, 5, 300));
    if (rotate_changed) ui_set_rotation(rotate != 0);
    ui_settings_changed();
    lvgl_port_unlock();
    return send_ok(req);
}

static esp_err_t on_library(httpd_req_t *req)
{
    char id[48];
    if (!query(req, "id", id, sizeof(id)) || !valid_id(id)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id");
    }
    lvgl_port_lock(0);
    int n;
    const char *selected;
    const abs_library_t *libs = ui_library_list(&n, &selected);
    int found = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(libs[i].id, id) == 0) found = i;
    }
    if (found >= 0 && strcmp(id, selected) != 0) {
        ui_request_library(id);  // the main loop loads it
        char msg[96];
        snprintf(msg, sizeof(msg), "Opening %s...", libs[found].name);
        ui_show_message(msg);
    }
    lvgl_port_unlock();
    if (found < 0) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such library");
    return send_ok(req);
}

/* ---------- start / stop ---------- */

static void mdns_start(void)
{
    static bool started;
    if (started) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mDNS failed to start; use the device's IP address");
        return;
    }
    mdns_hostname_set("abspresso");
    mdns_instance_name_set("ABSpresso");
    mdns_service_add("ABSpresso", "_http", "_tcp", 80, NULL, 0);
    started = true;
}

void remote_start(void)
{
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;  // internal RAM is scarce
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 20;
    // Stay out of the audio pipeline's way: below its tasks' priorities, off the core the
    // fetch task streams on, and few sockets (lwIP has 10 in all; streaming needs some).
    cfg.task_priority = 2;
    cfg.core_id = 1;
    cfg.max_open_sockets = 3;
    cfg.backlog_conn = 2;
    cfg.lru_purge_enable = true;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
        s_httpd = NULL;
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = on_page},
        {.uri = "/api/status", .method = HTTP_GET, .handler = on_status},
        {.uri = "/api/books", .method = HTTP_GET, .handler = on_books},
        {.uri = "/api/cover", .method = HTTP_GET, .handler = on_cover},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = on_get_settings},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = on_set_settings},
        {.uri = "/api/library", .method = HTTP_POST, .handler = on_library},
        {.uri = "/api/toggle", .method = HTTP_POST, .handler = on_toggle},
        {.uri = "/api/play", .method = HTTP_POST, .handler = on_play},
        {.uri = "/api/skip", .method = HTTP_POST, .handler = on_skip},
        {.uri = "/api/seek", .method = HTTP_POST, .handler = on_seek},
        {.uri = "/api/chapter", .method = HTTP_POST, .handler = on_chapter},
        {.uri = "/api/volume", .method = HTTP_POST, .handler = on_volume},
        {.uri = "/api/stop", .method = HTTP_POST, .handler = on_stop},
        {.uri = "/api/downloads", .method = HTTP_GET, .handler = on_downloads},
        {.uri = "/api/download", .method = HTTP_POST, .handler = on_download},
    };
    for (int i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_httpd, &uris[i]);
    mdns_start();
    ESP_LOGI(TAG, "remote control on http://abspresso.local (internal RAM free %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void remote_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
}

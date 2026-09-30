#include "abs_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "secrets.h"

static const char *TAG = "abs";

// The server's reverse proxy rejects some default user agents.
#define USER_AGENT "abs-esp32/0.1"
#define DEVICE_NAME "ESP32 ABS Player"

static char s_auth[600];
static char s_device_id[24];
static char s_device_id_dl[28];
struct abs_stream {
    esp_http_client_handle_t client;
};

// Library listings are hundreds of KB of JSON; keep cJSON's many small nodes out of internal RAM.
static void *psram_malloc(size_t n)
{
    return heap_caps_malloc_prefer(n, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
}

static char *psram_strdup(const char *s)
{
    size_t n = strlen(s ? s : "") + 1;
    char *d = psram_malloc(n);
    if (d) memcpy(d, s ? s : "", n);
    return d;
}

void abs_api_init(void)
{
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    snprintf(s_auth, sizeof(s_auth), "Bearer %s", ABS_TOKEN);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "esp32-%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_device_id_dl, sizeof(s_device_id_dl), "%s-dl", s_device_id);
}

static esp_http_client_handle_t new_client(const char *path, esp_http_client_method_t method)
{
    char url[256];
    snprintf(url, sizeof(url), "https://%s%s", ABS_SERVER, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = USER_AGENT,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        esp_http_client_set_header(c, "Authorization", s_auth);
    }
    return c;
}

// Performs a request and returns the body (NUL-terminated, PSRAM) in *out and its length in *out_len.
// With `keep`, the connection is reused across calls (skipping a multi-second TLS handshake each
// time); *keep must only be used from one task. Returns HTTP status or -1.
static int request_len(esp_http_client_method_t method, const char *path, const char *body, char **out,
                       size_t *out_len, esp_http_client_handle_t *keep)
{
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    const bool reused = keep && *keep;
    esp_http_client_handle_t c;
    if (reused) {
        char url[256];
        snprintf(url, sizeof(url), "https://%s%s", ABS_SERVER, path);
        c = *keep;
        esp_http_client_set_url(c, url);
        esp_http_client_set_method(c, method);
    } else {
        c = new_client(path, method);
        if (!c) return -1;
    }
    int body_len = body ? strlen(body) : 0;
    if (body) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
    }
    int status = -1;
    bool complete = false;
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: connect failed: %s", path, esp_err_to_name(err));
        goto done;
    }
    if (body_len && esp_http_client_write(c, body, body_len) != body_len) {
        ESP_LOGE(TAG, "%s: write failed", path);
        goto done;
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        goto done;
    }
    status = esp_http_client_get_status_code(c);

    size_t cap = 16 * 1024, len = 0;
    char *buf = psram_malloc(cap);
    while (buf) {
        if (cap - len < 4097) {
            char *nb = heap_caps_realloc(buf, cap * 2, MALLOC_CAP_SPIRAM);
            if (!nb) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        int n = esp_http_client_read(c, buf + len, 4096);
        if (n <= 0) break;
        len += n;
    }
    complete = esp_http_client_is_complete_data_received(c);
    if (buf) {
        buf[len] = 0;
        if (out_len) *out_len = len;
        if (out) *out = buf; else free(buf);
    } else {
        ESP_LOGE(TAG, "%s: out of memory reading response", path);
        status = -1;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "%s %s -> %d", method == HTTP_METHOD_GET ? "GET" : (method == HTTP_METHOD_POST ? "POST" : "PATCH"),
                 path, status);
    }
done:
    if (keep && status > 0 && complete) {
        *keep = c;
        return status;
    }
    esp_http_client_cleanup(c);
    if (keep) *keep = NULL;
    if (reused && status < 0) {
        // The server probably dropped the idle connection; try once on a fresh one.
        return request_len(method, path, body, out, out_len, keep);
    }
    return status;
}

static int request(esp_http_client_method_t method, const char *path, const char *body, char **out)
{
    return request_len(method, path, body, out, NULL, NULL);
}

static const char *json_str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static double json_num(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static int book_cmp(const void *pa, const void *pb)
{
    const abs_book_t *a = pa, *b = pb;
    return strcasecmp(a->sort_title, b->sort_title);
}

esp_err_t abs_parse_books(const char *items_json, const char *me_json, abs_book_t **out_books, int *out_count)
{
    *out_books = NULL;
    *out_count = 0;
    cJSON *root = cJSON_Parse(items_json);
    if (!root) return ESP_FAIL;
    const cJSON *results = cJSON_GetObjectItem(root, "results");
    int n = cJSON_GetArraySize(results);
    abs_book_t *books = heap_caps_calloc(n ? n : 1, sizeof(abs_book_t), MALLOC_CAP_SPIRAM);
    int count = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, results) {
        const cJSON *media = cJSON_GetObjectItem(it, "media");
        const cJSON *meta = cJSON_GetObjectItem(media, "metadata");
        const char *id = json_str(it, "id");
        if (!id || !books) continue;
        abs_book_t *b = &books[count++];
        strlcpy(b->id, id, sizeof(b->id));
        b->title = psram_strdup(json_str(meta, "title") ?: "Untitled");
        b->sort_title = psram_strdup(json_str(meta, "titleIgnorePrefix") ?: b->title);
        b->added_at = json_num(it, "addedAt");
        b->author = psram_strdup(json_str(meta, "authorName") ?: "");
        b->duration = json_num(media, "duration");
    }
    cJSON_Delete(root);

    // Progress lives on the user record.
    root = me_json ? cJSON_Parse(me_json) : NULL;
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(root, "mediaProgress")) {
        const char *item = json_str(p, "libraryItemId");
        if (!item || json_str(p, "episodeId")) continue;
        for (int i = 0; i < count; i++) {
            if (strcmp(books[i].id, item) == 0) {
                books[i].current_time = json_num(p, "currentTime");
                books[i].progress = json_num(p, "progress");
                books[i].finished = cJSON_IsTrue(cJSON_GetObjectItem(p, "isFinished"));
                books[i].last_update = json_num(p, "lastUpdate");
                break;
            }
        }
    }
    cJSON_Delete(root);

    qsort(books, count, sizeof(abs_book_t), book_cmp);
    *out_books = books;
    *out_count = count;
    return ESP_OK;
}

esp_err_t abs_get_books(abs_book_t **out_books, int *out_count, char **items_json, char **me_json)
{
    *out_books = NULL;
    *out_count = 0;
    *items_json = *me_json = NULL;
    char *body = NULL;
    char lib_id[40] = "";

    if (request(HTTP_METHOD_GET, "/api/libraries", NULL, &body) != 200) {
        free(body);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *lib;
    cJSON_ArrayForEach(lib, cJSON_GetObjectItem(root, "libraries")) {
        const char *type = json_str(lib, "mediaType");
        if (type && strcmp(type, "book") == 0) {
            strlcpy(lib_id, json_str(lib, "id"), sizeof(lib_id));
            break;
        }
    }
    cJSON_Delete(root);
    if (!lib_id[0]) {
        ESP_LOGE(TAG, "no book library found");
        return ESP_FAIL;
    }

    char path[160];
    snprintf(path, sizeof(path), "/api/libraries/%s/items?limit=1000&minified=1", lib_id);
    if (request(HTTP_METHOD_GET, path, NULL, items_json) != 200 ||
        request(HTTP_METHOD_GET, "/api/me", NULL, me_json) != 200) {
        free(*items_json);
        free(*me_json);
        *items_json = *me_json = NULL;
        return ESP_FAIL;
    }
    esp_err_t err = abs_parse_books(*items_json, *me_json, out_books, out_count);
    ESP_LOGI(TAG, "loaded %d books", *out_count);
    return err;
}

esp_err_t abs_patch_progress(const char *item_id, double current_time, double duration, bool finished)
{
    char path[96], body[160];
    snprintf(path, sizeof(path), "/api/me/progress/%s", item_id);
    snprintf(body, sizeof(body), "{\"currentTime\":%.2f,\"duration\":%.2f,\"progress\":%.5f,\"isFinished\":%s}",
             current_time, duration, duration > 0 ? current_time / duration : 0, finished ? "true" : "false");
    return request(HTTP_METHOD_PATCH, path, body, NULL) == 200 ? ESP_OK : ESP_FAIL;
}

void abs_free_books(abs_book_t *books, int count)
{
    for (int i = 0; i < count; i++) {
        free(books[i].title);
        free(books[i].sort_title);
        free(books[i].author);
    }
    free(books);
}

esp_err_t abs_start_session(const char *item_id, bool for_download, abs_session_t *out)
{
    memset(out, 0, sizeof(*out));
    char path[96], req[320];
    snprintf(path, sizeof(path), "/api/items/%s/play", item_id);
    // Only advertising audio/mpeg plus forceTranscode makes the server hand back an HLS stream of
    // MPEG-TS segments (AAC, or MP3 for MP3 sources) regardless of the book's file format.
    snprintf(req, sizeof(req),
             "{\"deviceInfo\":{\"clientName\":\"" DEVICE_NAME "\",\"deviceId\":\"%s\"},"
             "\"supportedMimeTypes\":[\"audio/mpeg\"],\"mediaPlayer\":\"esp32\",\"forceTranscode\":true}",
             // Downloads use their own device id so the server never treats them as the same
             // session as playback (it may close one device's session when another starts).
             for_download ? s_device_id_dl : s_device_id);
    char *body = NULL;
    if (request(HTTP_METHOD_POST, path, req, &body) != 200) {
        free(body);
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const char *id = json_str(root, "id");
    const cJSON *track = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "audioTracks"), 0);
    const char *url = json_str(track, "contentUrl");
    if (!id || !url || strncmp(url, "/hls/", 5) != 0) {
        ESP_LOGE(TAG, "unexpected play response (playMethod %d)", (int)json_num(root, "playMethod"));
        cJSON_Delete(root);
        return ESP_FAIL;
    }
    strlcpy(out->id, id, sizeof(out->id));
    strlcpy(out->hls_path, url, sizeof(out->hls_path));
    out->current_time = json_num(root, "currentTime");
    out->duration = json_num(root, "duration");

    const cJSON *chapters = cJSON_GetObjectItem(root, "chapters");
    int n = cJSON_GetArraySize(chapters);
    if (n > 0) {
        out->chapters = heap_caps_calloc(n, sizeof(abs_chapter_t), MALLOC_CAP_SPIRAM);
        const cJSON *ch;
        cJSON_ArrayForEach(ch, chapters) {
            if (!out->chapters) break;
            abs_chapter_t *c = &out->chapters[out->chapter_count++];
            c->title = psram_strdup(json_str(ch, "title") ?: "");
            c->start = json_num(ch, "start");
            c->end = json_num(ch, "end");
        }
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "session %s at %.0f/%.0f s, %d chapters", out->id, out->current_time, out->duration,
             out->chapter_count);
    return ESP_OK;
}

void abs_free_session(abs_session_t *s)
{
    for (int i = 0; i < s->chapter_count; i++) {
        free(s->chapters[i].title);
    }
    free(s->chapters);
    memset(s, 0, sizeof(*s));
}

esp_err_t abs_sync_session(const char *session_id, double current_time, double time_listening, double duration)
{
    char path[96], body[160];
    snprintf(path, sizeof(path), "/api/session/%s/sync", session_id);
    snprintf(body, sizeof(body), "{\"currentTime\":%.2f,\"timeListening\":%.2f,\"duration\":%.2f}",
             current_time, time_listening, duration);
    return request(HTTP_METHOD_POST, path, body, NULL) == 200 ? ESP_OK : ESP_FAIL;
}

esp_err_t abs_close_session(const char *session_id, double current_time, double time_listening)
{
    char path[96], body[128];
    snprintf(path, sizeof(path), "/api/session/%s/close", session_id);
    if (time_listening > 0) {
        snprintf(body, sizeof(body), "{\"currentTime\":%.2f,\"timeListening\":%.2f}", current_time, time_listening);
    } else {
        // Nothing was played; don't let the close create or move a progress record.
        strcpy(body, "{}");
    }
    return request(HTTP_METHOD_POST, path, body, NULL) == 200 ? ESP_OK : ESP_FAIL;
}

abs_stream_t *abs_stream_create(void)
{
    return heap_caps_calloc(1, sizeof(abs_stream_t), MALLOC_CAP_SPIRAM);
}

int abs_stream_begin(abs_stream_t *st, const char *path)
{
    char url[256];
    snprintf(url, sizeof(url), "https://%s%s", ABS_SERVER, path);
    if (!st->client) {
        st->client = new_client(path, HTTP_METHOD_GET);
        if (!st->client) return -1;
    } else {
        esp_http_client_set_url(st->client, url);
    }
    esp_err_t err = esp_http_client_open(st->client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "segment connect failed: %s", esp_err_to_name(err));
        abs_stream_end(st, false);
        return -1;
    }
    if (esp_http_client_fetch_headers(st->client) < 0) {
        abs_stream_end(st, false);
        return -1;
    }
    return esp_http_client_get_status_code(st->client);
}

int abs_stream_read(abs_stream_t *st, char *buf, int len)
{
    return esp_http_client_read(st->client, buf, len);
}

bool abs_stream_complete(abs_stream_t *st)
{
    return st->client && esp_http_client_is_complete_data_received(st->client);
}

void abs_stream_end(abs_stream_t *st, bool keep_alive)
{
    if (!st->client) return;
    if (keep_alive) {
        // Drain anything left so the connection can carry the next request.
        esp_http_client_flush_response(st->client, NULL);
    } else {
        esp_http_client_cleanup(st->client);
        st->client = NULL;
    }
}

esp_err_t abs_get_cover(const char *item_id, int width, uint8_t **out, size_t *out_len)
{
    char path[128];
    // The server resizes and re-encodes, so this is a small baseline JPEG.
    snprintf(path, sizeof(path), "/api/items/%s/cover?width=%d&format=jpeg", item_id, width);
    char *body = NULL;
    // Only the cover loader task calls this, so it can own a persistent connection.
    static esp_http_client_handle_t conn;
    if (request_len(HTTP_METHOD_GET, path, NULL, &body, out_len, &conn) != 200 || *out_len == 0) {
        free(body);
        return ESP_FAIL;
    }
    *out = (uint8_t *)body;
    return ESP_OK;
}

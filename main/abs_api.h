#pragma once

// Minimal Audiobookshelf REST client. All calls block; call them off the UI thread.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char id[40];
    char *title;
    char *sort_title;      // title without a leading "The"/"A" (server-provided), for A-Z
    char *author;          // may list several authors separated by ", "
    double duration;
    double current_time;   // saved progress, seconds
    float progress;        // 0..1
    bool finished;
    double last_update;    // progress timestamp (ms), 0 if never played
    double added_at;       // when the item was added to the library (ms)
} abs_book_t;

typedef struct {
    char *title;
    double start;
    double end;
} abs_chapter_t;

typedef struct {
    char id[40];
    char hls_path[96];     // e.g. /hls/<session>/output.m3u8
    double current_time;
    double duration;
    abs_chapter_t *chapters;
    int chapter_count;
} abs_session_t;

void abs_api_init(void);

// Books from the first book library, sorted A-Z by sort_title. Also returns the raw item and
// user JSON (caller frees) so they can be cached and re-parsed offline.
esp_err_t abs_get_books(abs_book_t **out_books, int *out_count, char **items_json, char **me_json);
esp_err_t abs_parse_books(const char *items_json, const char *me_json, abs_book_t **out_books, int *out_count);
void abs_free_books(abs_book_t *books, int count);

// Starts an HLS (transcoded) playback session at the saved position.
esp_err_t abs_start_session(const char *item_id, bool for_download, abs_session_t *out);
void abs_free_session(abs_session_t *s);
esp_err_t abs_sync_session(const char *session_id, double current_time, double time_listening, double duration);
esp_err_t abs_close_session(const char *session_id, double current_time, double time_listening);
// Sets progress directly, without a playback session (used for downloaded books).
esp_err_t abs_patch_progress(const char *item_id, double current_time, double duration, bool finished);

// Downloads the item's cover as a JPEG scaled to `width` px wide. Caller frees *out.
// Keeps its connection open between calls; call from a single task only.
esp_err_t abs_get_cover(const char *item_id, int width, uint8_t **out, size_t *out_len);

// Streaming GETs for HLS segments over a keep-alive connection. Each user (player, downloader)
// owns its own stream; a stream must only be used from one task.
typedef struct abs_stream abs_stream_t;
abs_stream_t *abs_stream_create(void);
// Returns the HTTP status (or <0 on error). On 200 read with abs_stream_read(), then abs_stream_end().
int abs_stream_begin(abs_stream_t *st, const char *path);
int abs_stream_read(abs_stream_t *st, char *buf, int len);
bool abs_stream_complete(abs_stream_t *st);
void abs_stream_end(abs_stream_t *st, bool keep_alive);

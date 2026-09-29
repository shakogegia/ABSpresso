#pragma once

// Minimal Audiobookshelf REST client. All calls block; call them off the UI thread.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    char id[40];
    char *title;
    char *author;
    double duration;
    double current_time;   // saved progress, seconds
    float progress;        // 0..1
    bool finished;
    double last_update;    // progress timestamp (ms), 0 if never played
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

// Books from the first book library, in-progress first (most recent), then by title.
esp_err_t abs_get_books(abs_book_t **out_books, int *out_count);
void abs_free_books(abs_book_t *books, int count);

// Starts an HLS (transcoded) playback session at the saved position.
esp_err_t abs_start_session(const char *item_id, abs_session_t *out);
void abs_free_session(abs_session_t *s);
esp_err_t abs_sync_session(const char *session_id, double current_time, double time_listening, double duration);
esp_err_t abs_close_session(const char *session_id, double current_time, double time_listening);

// Downloads the item's cover as a JPEG scaled to `width` px wide. Caller frees *out.
// Keeps its connection open between calls; call from a single task only.
esp_err_t abs_get_cover(const char *item_id, int width, uint8_t **out, size_t *out_len);

// Opens a streaming GET for an HLS segment of the session. Returns the HTTP status (or <0 on error).
// On 200 the caller reads with abs_stream_read() and must call abs_stream_end().
int abs_stream_begin(const char *path);
int abs_stream_read(char *buf, int len);
bool abs_stream_complete(void);
void abs_stream_end(bool keep_alive);

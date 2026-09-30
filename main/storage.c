#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

static const char *TAG = "storage";

// 1-bit SDMMC (pins from Waveshare's demo for this board).
#define PIN_SD_CLK 14
#define PIN_SD_CMD 17
#define PIN_SD_D0  16

static sdmmc_card_t *s_card;

esp_err_t storage_init(void)
{
    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,  // never wipe a card the user put in
        .max_files = 8,
        .allocation_unit_size = 32 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0 = PIN_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no usable SD card (%s); caching and downloads disabled", esp_err_to_name(err));
        s_card = NULL;
        return err;
    }
    storage_mkdirs(STORAGE_ROOT);
    uint64_t free_b, total_b;
    storage_space(&free_b, &total_b);
    ESP_LOGI(TAG, "SD card '%s' mounted: %llu MB free of %llu MB", s_card->cid.name, free_b >> 20, total_b >> 20);
    return ESP_OK;
}

bool storage_ready(void)
{
    return s_card != NULL;
}

void storage_space(uint64_t *free_bytes, uint64_t *total_bytes)
{
    *free_bytes = *total_bytes = 0;
    if (s_card) esp_vfs_fat_info("/sdcard", total_bytes, free_bytes);
}

// SDMMC DMA can't target PSRAM reliably (it corrupts data), so file data always passes through
// a small internal buffer. Large PSRAM buffers are copied in and out of it chunk by chunk.
#define BOUNCE 4096

char *storage_read_file(const char *path, size_t *len)
{
    if (len) *len = 0;
    if (!s_card) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = n >= 0 ? heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM) : NULL;
    uint8_t *bounce = heap_caps_malloc(BOUNCE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    for (long off = 0; buf && bounce && off < n;) {
        size_t chunk = (n - off) < BOUNCE ? (n - off) : BOUNCE;
        if (fread(bounce, 1, chunk, f) != chunk) {
            free(buf);
            buf = NULL;
            break;
        }
        memcpy(buf + off, bounce, chunk);
        off += chunk;
    }
    if (!bounce) {
        free(buf);
        buf = NULL;
    }
    free(bounce);
    fclose(f);
    if (buf) {
        buf[n] = 0;
        if (len) *len = n;
    }
    return buf;
}

esp_err_t storage_write_file(const char *path, const void *data, size_t len)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    char tmp[160];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return ESP_FAIL;
    uint8_t *bounce = heap_caps_malloc(BOUNCE, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    bool ok = bounce != NULL;
    for (size_t off = 0; ok && off < len;) {
        size_t chunk = (len - off) < BOUNCE ? (len - off) : BOUNCE;
        memcpy(bounce, (const uint8_t *)data + off, chunk);
        ok = fwrite(bounce, 1, chunk, f) == chunk;
        off += chunk;
    }
    free(bounce);
    ok &= fclose(f) == 0;
    if (ok) {
        unlink(path);
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        ESP_LOGW(TAG, "writing %s failed (errno %d)", path, errno);
        unlink(tmp);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t storage_mkdirs(const char *path)
{
    char p[160];
    strlcpy(p, path, sizeof(p));
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0775);
            *s = '/';
        }
    }
    return (mkdir(p, 0775) == 0 || errno == EEXIST) ? ESP_OK : ESP_FAIL;
}

esp_err_t storage_remove_tree(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    struct dirent *e;
    char child[320];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, e->d_name) >= (int)sizeof(child)) continue;
        if (e->d_type == DT_DIR) storage_remove_tree(child);
        else unlink(child);
    }
    closedir(d);
    return rmdir(path) == 0 ? ESP_OK : ESP_FAIL;
}

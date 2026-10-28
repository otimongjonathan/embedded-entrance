#include "storage.h"
#include "config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "storage";
static FILE *s_fp = NULL;
static SemaphoreHandle_t s_mtx = NULL;

void storage_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    s_mtx = xSemaphoreCreateMutex();

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI3_HOST;
    slot.gpio_cs = PIN_SD_CS;

    esp_vfs_fat_sdmmc_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s); RAM-only logging", esp_err_to_name(err));
        return;
    }
    s_fp = fopen("/sdcard/access.csv", "a");
    if (s_fp) {
        fseek(s_fp, 0, SEEK_END);
        if (ftell(s_fp) == 0) {
            fprintf(s_fp, "epoch,iso8601,tag_hex,dir,result\n");
            fflush(s_fp);
        }
    }
}

void storage_load_schedule(int *start, int *end) {
    *start = -1; *end = -1;
    nvs_handle_t h;
    if (nvs_open("cocis", NVS_READONLY, &h) != ESP_OK) return;
    int32_t s = 0, e = 0;
        int32_t version = 0;
        nvs_get_i32(h, "sched_start", &s);
        nvs_get_i32(h, "sched_end",   &e);
        esp_err_t version_err = nvs_get_i32(h, "sched_version", &version);
    *start = s; *end = e;
    nvs_close(h);

        if (version_err != ESP_OK) {
            if (s == 7 * 60 && e == 21 * 60) { s = 0; e = 0; }
            storage_save_schedule(s, e);
            *start = s; *end = e;
        }
}

void storage_save_schedule(int start, int end) {
    nvs_handle_t h;
    if (nvs_open("cocis", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "sched_start", start);
    nvs_set_i32(h, "sched_end",   end);
        nvs_set_i32(h, "sched_version", 1);
        nvs_commit(h);
    nvs_close(h);
}

void storage_load_inside(uint32_t *tags, int *n) {
    *n = 0;
    nvs_handle_t h;
    if (nvs_open("cocis", NVS_READONLY, &h) != ESP_OK) return;
    uint8_t buf[sizeof(int) + INSIDE_MAX * sizeof(uint32_t)];
    size_t len = sizeof(buf);
    if (nvs_get_blob(h, "inside", buf, &len) == ESP_OK && len >= sizeof(int)) {
        int cnt; memcpy(&cnt, buf, sizeof(int));
        if (cnt < 0 || cnt > INSIDE_MAX) cnt = 0;
        memcpy(tags, buf + sizeof(int), cnt * sizeof(uint32_t));
        *n = cnt;
    }
    nvs_close(h);
}

void storage_save_inside(const uint32_t *tags, int n) {
    if (n < 0 || n > INSIDE_MAX) return;
    nvs_handle_t h;
    if (nvs_open("cocis", NVS_READWRITE, &h) != ESP_OK) return;
    uint8_t buf[sizeof(int) + INSIDE_MAX * sizeof(uint32_t)] = {0};
    memcpy(buf, &n, sizeof(int));
    memcpy(buf + sizeof(int), tags, n * sizeof(uint32_t));
    nvs_set_blob(h, "inside", buf, sizeof(buf));
    nvs_commit(h);
    nvs_close(h);
}

void storage_log_event(time_t ts, uint32_t tag, bool is_in, const char *result) {
    if (!s_fp) return;
    char iso[32]; struct tm tm; localtime_r(&ts, &tm);
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tm);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    fprintf(s_fp, "%ld,%s,%08lX,%s,%s\n",
            (long)ts, iso, (unsigned long)tag,
            is_in ? "IN" : "OUT", result);
    fflush(s_fp);
    xSemaphoreGive(s_mtx);
}
#include "time_source.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

/* Fallbacks so the module still compiles against an sdkconfig that predates
 * the "Time" menu (same pattern the energy meter uses for its own knobs). */
#ifndef CONFIG_APP_TIME_TZ
#define CONFIG_APP_TIME_TZ "ICT-7"
#endif
#ifndef CONFIG_APP_TIME_NVS_SAVE_PERIOD_S
#define CONFIG_APP_TIME_NVS_SAVE_PERIOD_S 600
#endif

#define TIME_NVS_NAMESPACE  "timekeep"
#define TIME_NVS_KEY_EPOCH  "epoch"
#define TIME_NVS_KEY_BOOTS  "boots"

static const char *TAG = "time_src";

static SemaphoreHandle_t s_lock;
static time_source_kind_t s_kind = TIME_SOURCE_NONE;
static time_quality_t s_quality = TIME_QUALITY_NONE;
static uint32_t s_boot_count;
static int64_t s_last_save_us;       /* esp_timer stamp of the last NVS write */
static bool s_initialized;

/* Persist the current epoch as a floor. Caller must NOT hold s_lock: NVS can
 * block on a flash erase and the getters must stay responsive meanwhile. */
static esp_err_t time_store_epoch(time_t epoch)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(TIME_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_i64(nvs, TIME_NVS_KEY_EPOCH, (int64_t)epoch);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

esp_err_t time_source_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    /* Timezone first: every stamp below renders through localtime(). */
    setenv("TZ", CONFIG_APP_TIME_TZ, 1);
    tzset();

    int64_t stored_epoch = 0;
    uint32_t boots = 0;
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(TIME_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_OK) {
        if (nvs_get_i64(nvs, TIME_NVS_KEY_EPOCH, &stored_epoch) != ESP_OK) {
            stored_epoch = 0;
        }
        if (nvs_get_u32(nvs, TIME_NVS_KEY_BOOTS, &boots) != ESP_OK) {
            boots = 0;
        }
        boots++;
        if (nvs_set_u32(nvs, TIME_NVS_KEY_BOOTS, boots) == ESP_OK) {
            nvs_commit(nvs);
        }
        nvs_close(nvs);
    } else {
        ESP_LOGW(TAG, "open NVS failed (%s); boot count and epoch floor unavailable",
                 esp_err_to_name(ret));
        boots = 1;
    }

    s_boot_count = boots;
    s_last_save_us = esp_timer_get_time();
    s_initialized = true;

    /* A stored floor is a lower bound, never a true reading: the device was
     * powered off for an unknown span. Restore it so log rows at least sort
     * after the previous session, and mark them 'E' so nobody mistakes the
     * date for a measurement. A floor below TIME_SOURCE_EPOCH_MIN is junk. */
    if (stored_epoch >= (int64_t)TIME_SOURCE_EPOCH_MIN) {
        struct timeval tv = { .tv_sec = (time_t)stored_epoch, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) == 0) {
            s_kind = TIME_SOURCE_NVS;
            s_quality = TIME_QUALITY_ESTIMATE;
            char stamp[TIME_SOURCE_STAMP_LEN];
            time_source_format_stamp(stamp, sizeof(stamp));
            ESP_LOGI(TAG, "clock restored from NVS floor: %s (estimate), boot #%u",
                     stamp, (unsigned)s_boot_count);
        }
    }

    if (s_quality == TIME_QUALITY_NONE) {
        /* Expected until the DS1307 is wired: timestamps read 1970 and carry
         * the 'U' flag. uptime_s + boot count remain the usable time axis. */
        ESP_LOGI(TAG, "no time source (boot #%u); stamps are uptime-only ('U')",
                 (unsigned)s_boot_count);
    }
    return ESP_OK;
}

esp_err_t time_source_set(time_t epoch, time_source_kind_t kind)
{
    if (epoch < (time_t)TIME_SOURCE_EPOCH_MIN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_kind = kind;
    /* Only a live backend earns 'S'; a restored floor stays an estimate. */
    s_quality = (kind == TIME_SOURCE_RTC || kind == TIME_SOURCE_NTP)
                    ? TIME_QUALITY_SYNCED : TIME_QUALITY_ESTIMATE;
    xSemaphoreGive(s_lock);

    s_last_save_us = esp_timer_get_time();
    esp_err_t ret = time_store_epoch(epoch);

    char stamp[TIME_SOURCE_STAMP_LEN];
    time_source_format_stamp(stamp, sizeof(stamp));
    ESP_LOGI(TAG, "clock set to %s (source %d, quality '%c')",
             stamp, (int)kind, time_source_quality_char());
    return ret;
}

void time_source_service(void)
{
    if (!s_initialized || s_quality == TIME_QUALITY_NONE) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_last_save_us) < ((int64_t)CONFIG_APP_TIME_NVS_SAVE_PERIOD_S * 1000000LL)) {
        return;
    }
    s_last_save_us = now_us;

    /* Refresh the floor so the next boot resumes near the right date. One
     * write per CONFIG_APP_TIME_NVS_SAVE_PERIOD_S — negligible flash wear. */
    esp_err_t ret = time_store_epoch(time(NULL));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "epoch floor save failed: %s", esp_err_to_name(ret));
    }
}

esp_err_t time_source_flush(void)
{
    if (!s_initialized || s_quality == TIME_QUALITY_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    s_last_save_us = esp_timer_get_time();
    return time_store_epoch(time(NULL));
}

time_source_kind_t time_source_kind(void)
{
    if (s_lock == NULL) {
        return s_kind;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    time_source_kind_t k = s_kind;
    xSemaphoreGive(s_lock);
    return k;
}

time_quality_t time_source_quality(void)
{
    if (s_lock == NULL) {
        return s_quality;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    time_quality_t q = s_quality;
    xSemaphoreGive(s_lock);
    return q;
}

char time_source_quality_char(void)
{
    switch (time_source_quality()) {
    case TIME_QUALITY_SYNCED:   return 'S';
    case TIME_QUALITY_ESTIMATE: return 'E';
    default:                    return 'U';
    }
}

bool time_source_is_valid(void)
{
    return time_source_quality() != TIME_QUALITY_NONE;
}

time_t time_source_now(void)
{
    return time(NULL);
}

uint32_t time_source_uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000LL);
}

uint32_t time_source_boot_count(void)
{
    return s_boot_count;
}

size_t time_source_format_stamp(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }

    time_t now = time(NULL);
    struct tm tm_local;
    /* localtime_r never fails for a valid time_t; the epoch renders as
     * 1970-01-01 plus the TZ offset, which is exactly what we want to log
     * while no backend has set the clock. */
    localtime_r(&now, &tm_local);

    size_t n = strftime(buf, cap, "%Y-%m-%d %H:%M:%S", &tm_local);
    if (n == 0) {
        /* Buffer too small for the full stamp: keep the field non-empty so
         * the CSV column count never changes. */
        snprintf(buf, cap, "?");
        n = strlen(buf);
    }
    return n;
}

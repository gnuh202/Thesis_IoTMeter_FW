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

#if CONFIG_APP_TIME_RTC_ENABLE
#include "ds1307.h"
#include "i2c_bus.h"
#endif

#if CONFIG_APP_TIME_SYNC_ENABLE
#include "esp_sntp.h"
#include "network_manager.h"
#endif

/* Fallbacks so the module still compiles against an sdkconfig that predates
 * the "Time" menu (same pattern the energy meter uses for its own knobs). */
#ifndef CONFIG_APP_TIME_TZ
#define CONFIG_APP_TIME_TZ "ICT-7"
#endif
#ifndef CONFIG_APP_TIME_NVS_SAVE_PERIOD_S
#define CONFIG_APP_TIME_NVS_SAVE_PERIOD_S 600
#endif
#ifndef CONFIG_APP_TIME_RTC_I2C_ADDR
#define CONFIG_APP_TIME_RTC_I2C_ADDR 0x68
#endif
#ifndef CONFIG_APP_TIME_SYNC_SERVER
#define CONFIG_APP_TIME_SYNC_SERVER "pool.ntp.org"
#endif
#ifndef CONFIG_APP_TIME_SYNC_PERIOD_DAYS
#define CONFIG_APP_TIME_SYNC_PERIOD_DAYS 7
#endif
#ifndef CONFIG_APP_TIME_SYNC_TIMEOUT_S
#define CONFIG_APP_TIME_SYNC_TIMEOUT_S 30
#endif

/* Once a live backend drives the clock the floor only has to stay fresh, so it
 * is written far less often than in the estimate case. */
#define TIME_SAVE_PERIOD_SYNCED_S 3600

/* A failed sync attempt is retried on this cadence rather than immediately —
 * a down link or a blocked UDP 123 does not get better within seconds. */
#define TIME_SYNC_RETRY_S 3600

#define TIME_NVS_NAMESPACE  "timekeep"
#define TIME_NVS_KEY_EPOCH  "epoch"
#define TIME_NVS_KEY_BOOTS  "boots"
#define TIME_NVS_KEY_SYNC   "lastsync"

static const char *TAG = "time_src";

static SemaphoreHandle_t s_lock;
static time_source_kind_t s_kind = TIME_SOURCE_NONE;
static time_quality_t s_quality = TIME_QUALITY_NONE;
static uint32_t s_boot_count;
static int64_t s_last_save_us;       /* esp_timer stamp of the last NVS write */
static uint32_t s_jump_count;        /* bumped whenever the clock steps */
static bool s_initialized;

#if CONFIG_APP_TIME_RTC_ENABLE
static ds1307_handle_t s_rtc;
#endif

#if CONFIG_APP_TIME_SYNC_ENABLE
static time_t s_last_sync;           /* epoch of the last good sync, 0 = never */
static bool s_sync_requested;        /* forced sync pending (clock not trusted) */
static bool s_sync_running;          /* an SNTP attempt is in flight */
static int64_t s_sync_started_us;
static int64_t s_sync_checked_us;    /* last time the period was evaluated */
static volatile bool s_sync_done;    /* set from the SNTP callback */
static volatile time_t s_sync_epoch;
#endif

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


#if CONFIG_APP_TIME_RTC_ENABLE
/* Bring up the DS1307 on the shared I2C bus. i2c_bus_init() is idempotent, so
 * this works whichever module happens to touch the bus first. */
static void rtc_attach(void)
{
    esp_err_t ret = i2c_bus_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed (%s); no RTC", esp_err_to_name(ret));
        return;
    }

    ret = ds1307_create(i2c_bus_get_handle(), CONFIG_APP_TIME_RTC_I2C_ADDR, &s_rtc);
    if (ret != ESP_OK) {
        /* Same rule as the other bus peripherals: log, leave the handle NULL,
         * carry on booting. The clock degrades, the device does not. */
        ESP_LOGW(TAG, "DS1307 at 0x%02X not available: %s",
                 CONFIG_APP_TIME_RTC_I2C_ADDR, esp_err_to_name(ret));
        s_rtc = NULL;
    }
}

/* Read the chip as an epoch. Wraps the driver's broken-down time so callers
 * deal in one currency. */
static esp_err_t rtc_read_epoch(time_t *out)
{
    if (s_rtc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    struct tm tm_rtc;
    esp_err_t ret = ds1307_get_time(s_rtc, &tm_rtc);
    if (ret != ESP_OK) {
        return ret;
    }

    time_t epoch = mktime(&tm_rtc);
    if (epoch == (time_t)-1) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = epoch;
    return ESP_OK;
}

static esp_err_t rtc_write_epoch(time_t epoch)
{
    if (s_rtc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    struct tm tm_local;
    localtime_r(&epoch, &tm_local);
    return ds1307_set_time(s_rtc, &tm_local);
}
#endif /* CONFIG_APP_TIME_RTC_ENABLE */

#if CONFIG_APP_TIME_SYNC_ENABLE
/* Runs on the SNTP task. Does the minimum — hand the value to the service tick
 * and get out; settimeofday(), the RTC write and NVS all happen there so they
 * stay on one task and inside the normal code path. */
static void sntp_sync_cb(struct timeval *tv)
{
    s_sync_epoch = tv->tv_sec;
    s_sync_done = true;
}

static void sync_start(void)
{
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    s_sync_done = false;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, CONFIG_APP_TIME_SYNC_SERVER);
    /* SNTP_SYNC_MODE_IMMED: a first sync from 1970 is a huge step, and the
     * smooth mode would take days to close it. */
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);
    esp_sntp_init();

    s_sync_running = true;
    s_sync_started_us = esp_timer_get_time();
    ESP_LOGI(TAG, "SNTP sync started (%s)", CONFIG_APP_TIME_SYNC_SERVER);
}

static void sync_finish(bool ok)
{
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    s_sync_running = false;
    s_sync_checked_us = esp_timer_get_time();
    if (!ok) {
        ESP_LOGW(TAG, "SNTP sync timed out; retrying in %d min",
                 TIME_SYNC_RETRY_S / 60);
    }
}

static void sync_store_stamp(time_t epoch)
{
    nvs_handle_t nvs;
    if (nvs_open(TIME_NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    if (nvs_set_i64(nvs, TIME_NVS_KEY_SYNC, (int64_t)epoch) == ESP_OK) {
        nvs_commit(nvs);
    }
    nvs_close(nvs);
}

/* True when the link can carry a sync right now. The AP portal is excluded on
 * purpose: in config mode the only route is the phone that is configuring us. */
static bool sync_link_ready(void)
{
    if (network_manager_is_config_mode()) {
        return false;
    }
    network_status_t st;
    if (network_manager_get_status(&st) != ESP_OK) {
        return false;
    }
    return st.has_ip;
}

/* Drive the one-shot state machine. Called once a second from the service. */
static void sync_service(void)
{
    int64_t now_us = esp_timer_get_time();

    if (s_sync_running) {
        if (s_sync_done) {
            time_t epoch = s_sync_epoch;
            sync_finish(true);
            if (time_source_set(epoch, TIME_SOURCE_NTP) == ESP_OK) {
                s_last_sync = epoch;
                s_sync_requested = false;
                sync_store_stamp(epoch);
#if CONFIG_APP_TIME_RTC_ENABLE
                /* Push it back to the chip so the next cold boot starts
                 * accurate without a network. */
                esp_err_t ret = rtc_write_epoch(epoch);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "RTC updated from network time");
                } else if (ret != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "RTC write-back failed: %s", esp_err_to_name(ret));
                }
#endif
            }
        } else if ((now_us - s_sync_started_us) >
                   ((int64_t)CONFIG_APP_TIME_SYNC_TIMEOUT_S * 1000000LL)) {
            sync_finish(false);
        }
        return;
    }

    /* Decide whether a new attempt is due. A forced request (clock untrusted)
     * still honours the retry spacing so a dead link cannot spin. */
    int64_t due_s = s_sync_requested ? TIME_SYNC_RETRY_S
                                     : (int64_t)CONFIG_APP_TIME_SYNC_PERIOD_DAYS * 86400LL;
    if (s_sync_checked_us != 0 && (now_us - s_sync_checked_us) < (due_s * 1000000LL)) {
        return;
    }
    /* After a boot with a trusted clock, the period runs from the last
     * successful sync rather than from this boot. */
    if (!s_sync_requested && s_last_sync > 0 && time_source_is_valid()) {
        time_t now = time(NULL);
        if (now > s_last_sync && (now - s_last_sync) < due_s) {
            s_sync_checked_us = now_us;
            return;
        }
    }
    if (!sync_link_ready()) {
        return;
    }
    sync_start();
}
#endif /* CONFIG_APP_TIME_SYNC_ENABLE */

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
#if CONFIG_APP_TIME_SYNC_ENABLE
        int64_t last_sync = 0;
        if (nvs_get_i64(nvs, TIME_NVS_KEY_SYNC, &last_sync) == ESP_OK) {
            s_last_sync = (time_t)last_sync;
        }
#endif
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

    time_t floor = (stored_epoch >= (int64_t)TIME_SOURCE_EPOCH_MIN)
                       ? (time_t)stored_epoch : (time_t)0;

#if CONFIG_APP_TIME_RTC_ENABLE
    rtc_attach();

    if (s_rtc != NULL) {
        time_t rtc_epoch = 0;
        esp_err_t rtc_ret = rtc_read_epoch(&rtc_epoch);
        if (rtc_ret == ESP_OK && rtc_epoch >= floor) {
            /* The one path that earns 'S' at boot: the chip answered, the
             * oscillator is running, the BCD is in range, and the reading did
             * not go backwards past anything we wrote ourselves. */
            time_source_set(rtc_epoch, TIME_SOURCE_RTC);
        } else if (rtc_ret == ESP_OK) {
            /* CH is clear and the date looks plausible, yet it predates the
             * floor. That is the dead-battery signature the CH bit cannot
             * report, so the reading is dropped and the network is asked. */
            ESP_LOGW(TAG, "RTC reads before the stored floor (%lld < %lld); not trusted",
                     (long long)rtc_epoch, (long long)floor);
            time_source_request_sync();
        } else if (rtc_ret == ESP_ERR_INVALID_STATE) {
            /* CH is set: the oscillator has never been started. That is how a
             * chip leaves the factory, so it is a state to report, not a
             * fault. The first successful sync writes it and clears CH. */
            ESP_LOGI(TAG, "RTC oscillator halted (never set); waiting for a sync");
            time_source_request_sync();
        } else {
            ESP_LOGW(TAG, "RTC read failed: %s", esp_err_to_name(rtc_ret));
            time_source_request_sync();
        }
    } else {
        time_source_request_sync();
    }
#else
    time_source_request_sync();
#endif

    /* No usable RTC. The floor is not a reading — the device was off for an
     * unknown span — but restoring it keeps log rows sorting after the last
     * session, and 'E' tells anyone reading them not to trust the date. */
    if (s_quality == TIME_QUALITY_NONE && floor > 0) {
        struct timeval tv = { .tv_sec = floor, .tv_usec = 0 };
        if (settimeofday(&tv, NULL) == 0) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_kind = TIME_SOURCE_NVS;
            s_quality = TIME_QUALITY_ESTIMATE;
            xSemaphoreGive(s_lock);
            char stamp[TIME_SOURCE_STAMP_LEN];
            time_source_format_stamp(stamp, sizeof(stamp));
            ESP_LOGI(TAG, "clock restored from NVS floor: %s (estimate), boot #%u",
                     stamp, (unsigned)s_boot_count);
        }
    }

    if (s_quality == TIME_QUALITY_NONE) {
        /* Neither an RTC nor a floor: timestamps read 1970 and carry the 'U'
         * flag. uptime_s + boot count remain the usable time axis. */
        ESP_LOGI(TAG, "no time source (boot #%u); stamps are uptime-only ('U')",
                 (unsigned)s_boot_count);
    }

    return ESP_OK;
}

static time_quality_t quality_for_kind(time_source_kind_t kind)
{
    /* Only a live backend earns 'S'; a restored floor stays an estimate. */
    return (kind == TIME_SOURCE_RTC || kind == TIME_SOURCE_NTP)
               ? TIME_QUALITY_SYNCED : TIME_QUALITY_ESTIMATE;
}

esp_err_t time_source_set(time_t epoch, time_source_kind_t kind)
{
    if (epoch < (time_t)TIME_SOURCE_EPOCH_MIN) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Anti-rollback: within one boot session a weaker source must never
     * overwrite a stronger one. Without this the floor writer could undo an
     * NTP sync simply by running later. */
    time_quality_t quality = quality_for_kind(kind);
    if (quality < time_source_quality()) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t before = time(NULL);
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_kind = kind;
    s_quality = quality;
    /* Anything that moves the wall clock by more than a minute invalidates a
     * filename or a window boundary someone derived from the old value. The
     * counter lets those consumers notice without polling the clock itself. */
    time_t delta = epoch > before ? (epoch - before) : (before - epoch);
    if (delta > 60) {
        s_jump_count++;
    }
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
    if (!s_initialized) {
        return;
    }

#if CONFIG_APP_TIME_SYNC_ENABLE
    /* Runs even while the clock is unset — a sync is exactly what fixes that. */
    sync_service();
#endif

    if (s_quality == TIME_QUALITY_NONE) {
        return;
    }

    /* A trusted backend already knows the real time at the next boot, so the
     * floor is only a safety net and does not need refreshing often. Without
     * one it is the only thing standing between a reboot and 1970. */
    int64_t period_s = (s_quality == TIME_QUALITY_SYNCED)
                           ? TIME_SAVE_PERIOD_SYNCED_S
                           : CONFIG_APP_TIME_NVS_SAVE_PERIOD_S;

    int64_t now_us = esp_timer_get_time();
    if ((now_us - s_last_save_us) < (period_s * 1000000LL)) {
        return;
    }
    s_last_save_us = now_us;

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

bool time_source_rtc_present(void)
{
#if CONFIG_APP_TIME_RTC_ENABLE
    return s_rtc != NULL;
#else
    return false;
#endif
}

esp_err_t time_source_rtc_read(time_t *epoch)
{
    if (epoch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_APP_TIME_RTC_ENABLE
    return rtc_read_epoch(epoch);
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t time_source_rtc_write(time_t epoch)
{
    if (epoch < (time_t)TIME_SOURCE_EPOCH_MIN) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_APP_TIME_RTC_ENABLE
    esp_err_t ret = rtc_write_epoch(epoch);
    if (ret != ESP_OK) {
        return ret;
    }
    /* A hand-set clock is as good as an RTC read, and the operator expects it
     * to take effect now rather than at the next boot. */
    time_source_set(epoch, TIME_SOURCE_RTC);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void time_source_request_sync(void)
{
#if CONFIG_APP_TIME_SYNC_ENABLE
    s_sync_requested = true;
    s_sync_checked_us = 0;   /* evaluate at the next service tick */
#endif
}

time_t time_source_last_sync(void)
{
#if CONFIG_APP_TIME_SYNC_ENABLE
    return s_last_sync;
#else
    return (time_t)0;
#endif
}

uint32_t time_source_jump_count(void)
{
    if (s_lock == NULL) {
        return s_jump_count;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t n = s_jump_count;
    xSemaphoreGive(s_lock);
    return n;
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

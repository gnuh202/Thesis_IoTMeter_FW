#include "sd_card.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#include "spi_bus_shared.h"

#define SD_MOUNT_POINT "/sdcard"
#define SD_EVENTS_DIR SD_MOUNT_POINT "/events"
#define SD_ENERGY_DIR SD_MOUNT_POINT "/energy"
#define SD_EVENTS_FILE SD_EVENTS_DIR "/events.log"
#define SD_ENERGY_FILE SD_ENERGY_DIR "/energy.csv"

#define SD_MONITOR_TASK_STACK 4096
#define SD_MONITOR_TASK_PRIO 4
#define SD_MONITOR_POLL_MS 500
#define SD_DEBOUNCE_SAMPLES 3

static const char *TAG = "sd_card";

static bool s_detect_initialized;
static bool s_mounted;
static sdmmc_card_t *s_card;
static SemaphoreHandle_t s_lock;
static bool s_started;

esp_err_t sd_card_detect_init(void)
{
    if (s_detect_initialized) {
        return ESP_OK;
    }

    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << CONFIG_APP_SD_DET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&io_config), TAG, "configure SD detect pin failed");
    s_detect_initialized = true;
    return ESP_OK;
}

bool sd_card_is_inserted(void)
{
    if (!s_detect_initialized) {
        return false;
    }
    /* Card-detect switch pulls the pin low when a card is seated. */
    return gpio_get_level(CONFIG_APP_SD_DET_GPIO) == 0;
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

static esp_err_t ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return ESP_OK;
    }
    if (mkdir(path, 0775) != 0) {
        ESP_LOGW(TAG, "mkdir %s failed (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t sd_mount_locked(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(spi_bus_shared_init(), TAG, "shared SPI init failed");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = spi_bus_shared_get_host();

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = CONFIG_APP_SD_CS_GPIO;
    slot_config.host_id = host.slot;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "mount failed: %s", esp_err_to_name(ret));
        s_card = NULL;
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted at %s", SD_MOUNT_POINT);
    if (s_card != NULL) {
        sdmmc_card_print_info(stdout, s_card);
    }

    ensure_dir(SD_EVENTS_DIR);
    ensure_dir(SD_ENERGY_DIR);
    return ESP_OK;
}

static void sd_unmount_locked(void)
{
    if (!s_mounted) {
        return;
    }

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "unmount failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SD unmounted");
    }
    s_card = NULL;
    s_mounted = false;
}

static esp_err_t append_line(const char *path, const char *line)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(path, "a");
    if (f == NULL) {
        ESP_LOGW(TAG, "open %s failed (errno=%d)", path, errno);
        return ESP_FAIL;
    }
    fprintf(f, "%s\n", line);
    fclose(f);
    return ESP_OK;
}

esp_err_t sd_card_log_event(const char *fmt, ...)
{
    ESP_RETURN_ON_FALSE(fmt != NULL, ESP_ERR_INVALID_ARG, TAG, "fmt is NULL");
    if (!s_mounted || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = append_line(SD_EVENTS_FILE, line);
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t sd_card_log_energy(const char *line)
{
    ESP_RETURN_ON_FALSE(line != NULL, ESP_ERR_INVALID_ARG, TAG, "line is NULL");
    if (!s_mounted || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = append_line(SD_ENERGY_FILE, line);
    xSemaphoreGive(s_lock);
    return ret;
}

static void sd_monitor_task(void *arg)
{
    (void)arg;
    uint8_t stable_present = 0;
    uint8_t stable_absent = 0;

    while (1) {
        bool present = sd_card_is_inserted();

        if (present) {
            stable_absent = 0;
            if (stable_present < SD_DEBOUNCE_SAMPLES) {
                stable_present++;
            }
        } else {
            stable_present = 0;
            if (stable_absent < SD_DEBOUNCE_SAMPLES) {
                stable_absent++;
            }
        }

        if (stable_present >= SD_DEBOUNCE_SAMPLES && !s_mounted) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            esp_err_t ret = sd_mount_locked();
            xSemaphoreGive(s_lock);
            if (ret == ESP_OK) {
                sd_card_log_event("boot: card mounted");
            }
        } else if (stable_absent >= SD_DEBOUNCE_SAMPLES && s_mounted) {
            ESP_LOGW(TAG, "card removed, unmounting");
            xSemaphoreTake(s_lock, portMAX_DELAY);
            sd_unmount_locked();
            xSemaphoreGive(s_lock);
        }

        vTaskDelay(pdMS_TO_TICKS(SD_MONITOR_POLL_MS));
    }
}

esp_err_t sd_card_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "create SD lock failed");
    }

    ESP_RETURN_ON_ERROR(sd_card_detect_init(), TAG, "SD detect init failed");

    BaseType_t ok = xTaskCreate(sd_monitor_task, "sd_monitor", SD_MONITOR_TASK_STACK, NULL, SD_MONITOR_TASK_PRIO, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "create SD monitor task failed");

    s_started = true;
    ESP_LOGI(TAG, "SD manager started (detect GPIO %d, CS GPIO %d)", CONFIG_APP_SD_DET_GPIO, CONFIG_APP_SD_CS_GPIO);
    return ESP_OK;
}

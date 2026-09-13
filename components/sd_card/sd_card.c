#include "sd_card.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> /* strcasecmp: short/long calib filename match */
#include <sys/stat.h>
#include <dirent.h>
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

/* Forward declarations for calibration backup functions from energy_meter_task */
esp_err_t calib_backup_pack_single(uint8_t *file_out, size_t cap, size_t *file_len);
esp_err_t calib_backup_pack(uint8_t *file_out, size_t cap, size_t *file_len);
esp_err_t calib_backup_unpack(const uint8_t *file_in, size_t file_len, bool apply, bool save_nvs);
esp_err_t calib_backup_json(char *json_out, size_t cap, size_t *json_len);

#define SD_MOUNT_POINT "/sdcard"
#define SD_EVENTS_DIR SD_MOUNT_POINT "/EVENTS"
#define SD_ENERGY_DIR SD_MOUNT_POINT "/ENERGY"
#define SD_CALIB_DIR SD_MOUNT_POINT "/CALIB"
#define SD_EVENTS_FILE SD_EVENTS_DIR "/EVENTS.LOG"
#define SD_ENERGY_FILE SD_ENERGY_DIR "/ENERGY.CSV"

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
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
#ifdef CONFIG_APP_SD_ENABLE_LFN
        .use_one_fat = false,  /* Use both FATs for reliability when LFN is enabled */
#endif
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

    esp_err_t ret_dir;
    ret_dir = ensure_dir(SD_EVENTS_DIR);
    if (ret_dir != ESP_OK) {
        ESP_LOGW(TAG, "ensure_dir %s failed", SD_EVENTS_DIR);
    }
    ret_dir = ensure_dir(SD_ENERGY_DIR);
    if (ret_dir != ESP_OK) {
        ESP_LOGW(TAG, "ensure_dir %s failed", SD_ENERGY_DIR);
    }
    ret_dir = ensure_dir(SD_CALIB_DIR);
    if (ret_dir != ESP_OK) {
        ESP_LOGW(TAG, "ensure_dir %s failed", SD_CALIB_DIR);
    }
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

    int result = fprintf(f, "%s\n", line);
    esp_err_t ret = ESP_FAIL;

    if (result > 0) {
        /* Ensure data is written to disk */
        if (fflush(f) == 0) {
            ret = ESP_OK;
        } else {
            ESP_LOGW(TAG, "flush %s failed (errno=%d)", path, errno);
        }
    } else {
        ESP_LOGW(TAG, "fprintf %s failed (errno=%d)", path, errno);
    }

    fclose(f);
    return ret;
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

esp_err_t sd_card_ensure_calib_dir(void)
{
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }
    return ensure_dir(SD_CALIB_DIR);
}

esp_err_t sd_card_calib_export_current(char *path_out, size_t path_cap)
{
    ESP_RETURN_ON_FALSE(path_out != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    if (!s_mounted || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Pack current calibration into the single-profile format */
    uint8_t buf[256];
    size_t len;
    esp_err_t ret = calib_backup_pack_single(buf, sizeof(buf), &len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "calib_backup_pack_single failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Parse wiring_mode from file header to determine filename prefix.
     * Single-profile header (new layout, no file_version):
     *   magic(4) header_len(2) wiring_mode(2) reserved(2) payload_len(4) crc32(4) */
    typedef struct __attribute__((packed)) {
        uint32_t magic;
        uint16_t header_len;
        uint16_t wiring_mode;
        uint16_t reserved;
        uint32_t payload_len;
        uint32_t crc32;
    } file_header_t;

    file_header_t header;
    memcpy(&header, buf, sizeof(header));

    const char *mode_str;
    if (header.wiring_mode == 1) {
        mode_str = "3W";
    } else if (header.wiring_mode == 0) {
        mode_str = "4W";
    } else {
        ESP_LOGE(TAG, "unknown wiring_mode %u in header", header.wiring_mode);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);

    /* Scan existing files to find max NN for this mode */
    DIR *dir = opendir(SD_CALIB_DIR);
    int max_num = 0;
    if (dir != NULL) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_type != DT_REG) {
                continue;
            }
            char file_mode[4];
            int num;
            bool matched = false;
            if (sscanf(entry->d_name, "c%3[^_]_%d.bin", file_mode, &num) == 2) {
                if (strcasecmp(file_mode, "3w") == 0) {
                    strlcpy(file_mode, "3W", sizeof(file_mode));
                    matched = true;
                } else if (strcasecmp(file_mode, "4w") == 0) {
                    strlcpy(file_mode, "4W", sizeof(file_mode));
                    matched = true;
                }
            } else if (sscanf(entry->d_name, "calib_%3[^_]_%d.bin", file_mode, &num) == 2) {
                matched = true;
            }
            if (matched && strcmp(file_mode, mode_str) == 0 && num > max_num) {
                max_num = num;
            }
        }
        closedir(dir);
    }

    int next_num = max_num + 1;
    /* 8.3 short names: FATFS_LFN_NONE is set, so the basename must stay
     * <= 8 chars ("c3w_01" = 6). Long names fail fopen with EINVAL. */
    char filename[32];
    snprintf(filename, sizeof(filename), "c%sw_%02d.bin",
             (strcmp(mode_str, "3W") == 0) ? "3" : "4", next_num);
    char full_path[64];
    snprintf(full_path, sizeof(full_path), "%s/%s", SD_CALIB_DIR, filename);

    /* Verify directory exists */
    struct stat dir_stat;
    if (stat(SD_CALIB_DIR, &dir_stat) != 0) {
        ESP_LOGE(TAG, "directory %s does not exist (errno=%d)", SD_CALIB_DIR, errno);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "attempting to create: '%s' (len=%d)", full_path, strlen(full_path));

    /* Write binary file */
    FILE *f = fopen(full_path, "wb");
    ret = ESP_FAIL;
    if (f != NULL) {
        size_t written = fwrite(buf, 1, len, f);
        if (written == len) {
            /* Flush to ensure data is written to SD card */
            if (fflush(f) == 0) {
                ret = ESP_OK;
                if (path_cap > 0) {
                    snprintf(path_out, path_cap, "%s", full_path);
                }
                ESP_LOGI(TAG, "exported calibration to %s (%zu bytes)", filename, len);

                /* Also create CSV metadata file with same prefix */
                char json_filename[32];
                snprintf(json_filename, sizeof(json_filename), "c%sw_%02d.csv",
                         (strcmp(mode_str, "3W") == 0) ? "3" : "4", next_num);
                char json_path[64];
                snprintf(json_path, sizeof(json_path), "%s/%s", SD_CALIB_DIR, json_filename);

                /* Use static buffer to avoid stack overflow when called rapidly */
                static char json_buf[512];
                size_t json_len;
                if (calib_backup_json(json_buf, sizeof(json_buf), &json_len) == ESP_OK) {
                    FILE *json_f = fopen(json_path, "w");
                    if (json_f != NULL) {
                        fwrite(json_buf, 1, json_len, json_f);
                        fflush(json_f);
                        fclose(json_f);
                        ESP_LOGI(TAG, "exported CSV metadata to %s (%zu bytes)", json_filename, json_len);
                    } else {
                        ESP_LOGW(TAG, "failed to create CSV file %s (errno=%d)", json_path, errno);
                    }
                } else {
                    ESP_LOGW(TAG, "failed to generate CSV metadata");
                }
            } else {
                ESP_LOGE(TAG, "flush %s failed (errno=%d): %s", full_path, errno, strerror(errno));
            }
        } else {
            ESP_LOGE(TAG, "write %s failed (expected=%zu, wrote=%zu, errno=%d): %s",
                     full_path, len, written, errno, strerror(errno));
        }
        fclose(f);
    } else {
        ESP_LOGE(TAG, "open %s failed (errno=%d): %s", full_path, errno, strerror(errno));
    }

    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t sd_card_calib_list(sd_calib_entry_t *out, size_t max, size_t *count)
{
    ESP_RETURN_ON_FALSE(out != NULL && count != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    if (!s_mounted || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    *count = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);

    DIR *dir = opendir(SD_CALIB_DIR);
    if (dir == NULL) {
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "opendir %s failed (errno=%d)", SD_CALIB_DIR, errno);
        return ESP_OK;  /* empty list, not an error */
    }

    /* Collect all matching files */
    typedef struct {
        char name[32];
        uint8_t mode;
        uint8_t num;
    } temp_entry_t;
    temp_entry_t temp[32];
    size_t temp_count = 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && temp_count < 32) {
        if (entry->d_type != DT_REG) {
            continue;
        }
        /* Accept both current short names (c3w_NN.bin / c4w_NN.bin) and legacy
         * long names (calib_3W_NN.bin / calib_4W_NN.bin) so old cards still list. */
        char mode_str[4];
        int num;
        bool matched = false;
        if (sscanf(entry->d_name, "c%3[^_]_%d.bin", mode_str, &num) == 2) {
            /* Short form: mode_str is "3w" or "4w" */
            if (strcasecmp(mode_str, "3w") == 0) {
                strlcpy(mode_str, "3W", sizeof(mode_str));
                matched = true;
            } else if (strcasecmp(mode_str, "4w") == 0) {
                strlcpy(mode_str, "4W", sizeof(mode_str));
                matched = true;
            }
        } else if (sscanf(entry->d_name, "calib_%3[^_]_%d.bin", mode_str, &num) == 2) {
            matched = true;
        }
        if (!matched) {
            continue;
        }
        /* Verify extension */
        const char *ext = strrchr(entry->d_name, '.');
        if (ext == NULL || strcmp(ext, ".bin") != 0) {
            continue;
        }
        uint8_t mode;
        if (strcmp(mode_str, "3W") == 0) {
            mode = 1;  /* ATM90E32AS_WIRING_3P3W */
        } else if (strcmp(mode_str, "4W") == 0) {
            mode = 0;  /* ATM90E32AS_WIRING_3P4W */
        } else {
            continue;  /* unknown mode, skip */
        }
        strncpy(temp[temp_count].name, entry->d_name, sizeof(temp[temp_count].name) - 1);
        temp[temp_count].name[sizeof(temp[temp_count].name) - 1] = '\0';
        temp[temp_count].mode = mode;
        temp[temp_count].num = num;
        temp_count++;
    }
    closedir(dir);

    /* Sort descending by (mode, num) - show newest files first, grouped by mode */
    for (size_t i = 0; i < temp_count; i++) {
        for (size_t j = i + 1; j < temp_count; j++) {
            bool swap = false;
            if (temp[j].mode < temp[i].mode) {
                swap = true;  /* 4W (0) before 3W (1) */
            } else if (temp[j].mode == temp[i].mode && temp[j].num > temp[i].num) {
                swap = true;  /* higher number first within same mode */
            }
            if (swap) {
                temp_entry_t tmp = temp[i];
                temp[i] = temp[j];
                temp[j] = tmp;
            }
        }
    }

    /* Copy to output */
    size_t n = temp_count < max ? temp_count : max;
    for (size_t i = 0; i < n; i++) {
        snprintf(out[i].filename, sizeof(out[i].filename), "%s", temp[i].name);
        out[i].mode = temp[i].mode;
        out[i].num = temp[i].num;
    }
    *count = n;

    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "listed %zu calibration files", n);
    return ESP_OK;
}

esp_err_t sd_card_calib_import(const char *filename)
{
    ESP_RETURN_ON_FALSE(filename != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    if (!s_mounted || s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char full_path[64];
    snprintf(full_path, sizeof(full_path), "%s/%s", SD_CALIB_DIR, filename);

    xSemaphoreTake(s_lock, portMAX_DELAY);

    FILE *f = fopen(full_path, "rb");
    if (f == NULL) {
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "open %s failed (errno=%d)", full_path, errno);
        return ESP_FAIL;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 512) {
        fclose(f);
        xSemaphoreGive(s_lock);
        ESP_LOGE(TAG, "invalid file size %ld", file_size);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t buf[512];
    size_t read_len = fread(buf, 1, file_size, f);
    fclose(f);
    xSemaphoreGive(s_lock);

    if (read_len != (size_t)file_size) {
        ESP_LOGE(TAG, "read %s failed (expected=%ld, got=%zu)", full_path, file_size, read_len);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "importing calibration from %s (%zu bytes)", filename, read_len);
    /* Apply to current mode + save_nvs (cross-mode flexible) */
    return calib_backup_unpack(buf, read_len, true, true);
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

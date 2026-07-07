#include "energy_meter_task.h"

#include "atm90e32as.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_card.h"
#include "sdkconfig.h"
#include "spi_bus_shared.h"

#ifndef CONFIG_APP_ATM90E32AS_DEFAULT_MODE_3P3W
#define CONFIG_APP_ATM90E32AS_DEFAULT_MODE_3P3W 0
#endif

#ifndef CONFIG_APP_ATM90E32AS_DEFAULT_LINE_FREQ_60HZ
#define CONFIG_APP_ATM90E32AS_DEFAULT_LINE_FREQ_60HZ 0
#endif

static const char *TAG = "energy_meter";
static atm90e32as_handle_t s_meter;
static SemaphoreHandle_t s_meter_mutex;
static SemaphoreHandle_t s_measurements_mutex;
static atm90e32as_calib_t s_current_calib;
static atm90e32as_measurements_t s_latest_measurements;
static bool s_measurements_valid;

/* Energy accumulators (Wh / varh) protected by s_measurements_mutex. */
static double s_active_import_wh;
static double s_active_export_wh;
static double s_reactive_import_varh;
static double s_reactive_export_varh;

/* Demand: moving average of total active power over a configurable window. */
static double s_demand_accum_w;
static uint32_t s_demand_samples;
static float s_demand_value_w;
static float s_demand_max_w;
static uint16_t s_demand_window_min = 15;

#define ENERGY_METER_CALIB_MAGIC 0x9032CA1BU
#define ENERGY_METER_CALIB_VERSION 1
#define ENERGY_METER_NVS_NAMESPACE "atm90e32as"
#define ENERGY_METER_NVS_CALIB_KEY "calib_v1"

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    atm90e32as_calib_t calib;
} energy_meter_calib_blob_t;

static esp_err_t energy_meter_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
        ret = nvs_flash_init();
    }
    return ret == ESP_ERR_INVALID_STATE ? ESP_OK : ret;
}

static esp_err_t energy_meter_load_calibration_from_nvs(atm90e32as_calib_t *calib)
{
    ESP_RETURN_ON_ERROR(energy_meter_nvs_init(), TAG, "init NVS failed");

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(ENERGY_METER_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }

    energy_meter_calib_blob_t blob;
    size_t size = sizeof(blob);
    ret = nvs_get_blob(nvs, ENERGY_METER_NVS_CALIB_KEY, &blob, &size);
    nvs_close(nvs);

    if (ret != ESP_OK) {
        return ret;
    }
    if (size != sizeof(blob) || blob.magic != ENERGY_METER_CALIB_MAGIC || blob.version != ENERGY_METER_CALIB_VERSION) {
        return ESP_ERR_INVALID_VERSION;
    }

    *calib = blob.calib;
    return ESP_OK;
}

static esp_err_t energy_meter_save_calibration_to_nvs(const atm90e32as_calib_t *calib)
{
    ESP_RETURN_ON_ERROR(energy_meter_nvs_init(), TAG, "init NVS failed");

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(ENERGY_METER_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open calibration NVS failed");

    energy_meter_calib_blob_t blob = {
        .magic = ENERGY_METER_CALIB_MAGIC,
        .version = ENERGY_METER_CALIB_VERSION,
        .reserved = 0,
        .calib = *calib,
    };

    esp_err_t ret = nvs_set_blob(nvs, ENERGY_METER_NVS_CALIB_KEY, &blob, sizeof(blob));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

static esp_err_t energy_meter_gpio_init(void)
{
    gpio_config_t input_config = {
        .pin_bit_mask = (1ULL << CONFIG_APP_ATM90E32AS_IRQ1_GPIO) |
                        (1ULL << CONFIG_APP_ATM90E32AS_IRQ2_GPIO) |
                        (1ULL << CONFIG_APP_ATM90E32AS_WARN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_config), TAG, "configure ATM90E32AS status GPIOs failed");

    gpio_config_t mode_sel_config = {
        .pin_bit_mask = 1ULL << CONFIG_APP_ATM90E32AS_MODE_SEL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&mode_sel_config), TAG, "configure mode select GPIO failed");
    /* Relay level is driven from the resolved wiring mode (NVS or default) in
     * energy_meter_init(), after calibration is loaded. Do not set it from the
     * compile-time Kconfig default here, or a saved 3P3W setup would boot the
     * relay into the wrong wiring. */

    return ESP_OK;
}

/* Drive the wiring-mode relay to match the given wiring mode. */
static void energy_meter_set_wiring_relay(atm90e32as_wiring_mode_t wiring_mode)
{
    gpio_set_level(CONFIG_APP_ATM90E32AS_MODE_SEL_GPIO,
                   wiring_mode == ATM90E32AS_WIRING_3P3W ? 1 : 0);
}

static esp_err_t energy_meter_init(void)
{
    ESP_RETURN_ON_ERROR(energy_meter_gpio_init(), TAG, "init energy meter GPIO failed");
    ESP_RETURN_ON_ERROR(spi_bus_shared_init(), TAG, "init shared SPI failed");
    ESP_RETURN_ON_ERROR(sd_card_detect_init(), TAG, "init SD detect failed");

    if (s_measurements_mutex == NULL) {
        s_measurements_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create measurements mutex failed");
    }

    if (s_meter_mutex == NULL) {
        s_meter_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_meter_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create meter mutex failed");
    }

    atm90e32as_calib_t calib;
    atm90e32as_get_default_calib(&calib);
    calib.wiring_mode = CONFIG_APP_ATM90E32AS_DEFAULT_MODE_3P3W ? ATM90E32AS_WIRING_3P3W : ATM90E32AS_WIRING_3P4W;
    calib.line_freq = CONFIG_APP_ATM90E32AS_DEFAULT_LINE_FREQ_60HZ ? ATM90E32AS_LINE_FREQ_60HZ : ATM90E32AS_LINE_FREQ_50HZ;

    esp_err_t calib_ret = energy_meter_load_calibration_from_nvs(&calib);
    if (calib_ret == ESP_OK) {
        ESP_LOGI(TAG, "loaded ATM90E32AS calibration from NVS");
    } else {
        ESP_LOGW(TAG, "using ATM90E32AS bring-up defaults; no saved calibration: %s", esp_err_to_name(calib_ret));
    }
    s_current_calib = calib;

    /* Switch the wiring-mode relay to the resolved mode right after MCU power-up,
     * so a saved 3P3W/3P4W setup takes effect on boot without any user action. */
    energy_meter_set_wiring_relay(calib.wiring_mode);

    atm90e32as_config_t config = {
        .cs_gpio = CONFIG_APP_ATM90E32AS_CS_GPIO,
        .spi_clock_hz = CONFIG_APP_ATM90E32AS_SPI_CLOCK_HZ,
        .calib = calib,
    };

    ESP_RETURN_ON_ERROR(atm90e32as_create(&config, &s_meter), TAG, "create ATM90E32AS failed");
    ESP_RETURN_ON_ERROR(atm90e32as_init(s_meter), TAG, "init ATM90E32AS failed");

    ESP_LOGW(TAG, "ATM90E32AS uses bring-up calibration defaults. Replace with measured board calibration before thesis measurements.");
    ESP_LOGI(TAG, "SD card inserted=%d", sd_card_is_inserted());
    return ESP_OK;
}

static void energy_meter_task(void *arg)
{
    atm90e32as_measurements_t measurements;
    atm90e32as_energy_counts_t energy_counts;

    const uint32_t demand_window_samples_base =
        (uint32_t)(60000U / CONFIG_APP_ENERGY_METER_POLL_PERIOD_MS);

    while (1) {
        xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
        esp_err_t ret = atm90e32as_read_measurements(s_meter, &measurements);
        esp_err_t energy_ret = ESP_FAIL;
        if (ret == ESP_OK) {
            energy_ret = atm90e32as_read_energy_counts(s_meter, &energy_counts);
        }
        xSemaphoreGive(s_meter_mutex);

        if (ret == ESP_OK) {
            xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
            s_latest_measurements = measurements;
            s_measurements_valid = true;

            /* Accumulate energy (read-to-clear counts -> Wh/varh). */
            if (energy_ret == ESP_OK) {
                s_active_import_wh += energy_counts.active_import * (double)ATM90E32AS_ENERGY_COUNT_TO_WH;
                s_active_export_wh += energy_counts.active_export * (double)ATM90E32AS_ENERGY_COUNT_TO_WH;
                s_reactive_import_varh += energy_counts.reactive_import * (double)ATM90E32AS_ENERGY_COUNT_TO_WH;
                s_reactive_export_varh += energy_counts.reactive_export * (double)ATM90E32AS_ENERGY_COUNT_TO_WH;
            }

            /* Demand: block moving average of total active power. */
            uint32_t window_samples = demand_window_samples_base * (s_demand_window_min ? s_demand_window_min : 1);
            if (window_samples == 0) {
                window_samples = 1;
            }
            s_demand_accum_w += measurements.total_active_power;
            s_demand_samples++;
            if (s_demand_samples >= window_samples) {
                s_demand_value_w = (float)(s_demand_accum_w / s_demand_samples);
                if (s_demand_value_w > s_demand_max_w) {
                    s_demand_max_w = s_demand_value_w;
                }
                s_demand_accum_w = 0.0;
                s_demand_samples = 0;
            }
            xSemaphoreGive(s_measurements_mutex);

#if CONFIG_APP_ENERGY_METER_LOG_EACH_SAMPLE
            ESP_LOGI(TAG,
                     "VA=%.2fV IA=%.3fA VB=%.2fV IB=%.3fA VC=%.2fV IC=%.3fA P=%.2fW F=%.2fHz PF=%.3f ST0=0x%04X ST1=0x%04X",
                     measurements.voltage[ATM90E32AS_PHASE_A],
                     measurements.current[ATM90E32AS_PHASE_A],
                     measurements.voltage[ATM90E32AS_PHASE_B],
                     measurements.current[ATM90E32AS_PHASE_B],
                     measurements.voltage[ATM90E32AS_PHASE_C],
                     measurements.current[ATM90E32AS_PHASE_C],
                     measurements.total_active_power,
                     measurements.frequency,
                     measurements.total_power_factor,
                     measurements.sys_status0,
                     measurements.sys_status1);
#endif
        } else {
            ESP_LOGE(TAG, "read ATM90E32AS measurements failed: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_ENERGY_METER_POLL_PERIOD_MS));
    }
}

esp_err_t energy_meter_get_energy(energy_meter_energy_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    out->active_import_kwh = (float)(s_active_import_wh / 1000.0);
    out->active_export_kwh = (float)(s_active_export_wh / 1000.0);
    out->reactive_import_kvarh = (float)(s_reactive_import_varh / 1000.0);
    out->reactive_export_kvarh = (float)(s_reactive_export_varh / 1000.0);
    xSemaphoreGive(s_measurements_mutex);

    return ESP_OK;
}

esp_err_t energy_meter_get_demand(energy_meter_demand_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    out->active_power_demand_w = s_demand_value_w;
    out->active_power_demand_max_w = s_demand_max_w;
    xSemaphoreGive(s_measurements_mutex);

    return ESP_OK;
}

esp_err_t energy_meter_reset_energy(void)
{
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    s_active_import_wh = 0.0;
    s_active_export_wh = 0.0;
    s_reactive_import_varh = 0.0;
    s_reactive_export_varh = 0.0;
    xSemaphoreGive(s_measurements_mutex);

    return ESP_OK;
}

esp_err_t energy_meter_reset_demand(void)
{
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    s_demand_accum_w = 0.0;
    s_demand_samples = 0;
    s_demand_value_w = 0.0f;
    s_demand_max_w = 0.0f;
    xSemaphoreGive(s_measurements_mutex);

    return ESP_OK;
}

esp_err_t energy_meter_set_demand_window_minutes(uint16_t minutes)
{
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");
    ESP_RETURN_ON_FALSE(minutes > 0, ESP_ERR_INVALID_ARG, TAG, "window must be > 0");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    s_demand_window_min = minutes;
    s_demand_accum_w = 0.0;
    s_demand_samples = 0;
    xSemaphoreGive(s_measurements_mutex);

    return ESP_OK;
}

esp_err_t energy_meter_get_latest(atm90e32as_measurements_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "measurements mutex not initialized");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    bool valid = s_measurements_valid;
    if (valid) {
        *out = s_latest_measurements;
    }
    xSemaphoreGive(s_measurements_mutex);

    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool energy_meter_has_latest(void)
{
    if (s_measurements_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    bool valid = s_measurements_valid;
    xSemaphoreGive(s_measurements_mutex);

    return valid;
}

esp_err_t energy_meter_read_register(uint16_t reg, uint16_t *value)
{
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "value is NULL");
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    esp_err_t ret = atm90e32as_read_register(s_meter, reg, value);
    xSemaphoreGive(s_meter_mutex);

    return ret;
}

esp_err_t energy_meter_write_register(uint16_t reg, uint16_t value)
{
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    esp_err_t ret = atm90e32as_write_register(s_meter, reg, value);
    xSemaphoreGive(s_meter_mutex);

    return ret;
}

esp_err_t energy_meter_get_calibration(atm90e32as_calib_t *calib)
{
    ESP_RETURN_ON_FALSE(calib != NULL, ESP_ERR_INVALID_ARG, TAG, "calib is NULL");
    ESP_RETURN_ON_FALSE(s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    *calib = s_current_calib;
    xSemaphoreGive(s_meter_mutex);

    return ESP_OK;
}

esp_err_t energy_meter_set_calibration(const atm90e32as_calib_t *calib)
{
    ESP_RETURN_ON_FALSE(calib != NULL, ESP_ERR_INVALID_ARG, TAG, "calib is NULL");
    ESP_RETURN_ON_FALSE(s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    s_current_calib = *calib;
    xSemaphoreGive(s_meter_mutex);

    return ESP_OK;
}

esp_err_t energy_meter_apply_calibration(void)
{
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    /* Drive the wiring-mode relay (3P4W vs 3P3W) to match the calibration
     * before the meter registers are updated, so the front-end wiring and the
     * chip's metering mode stay consistent. */
    energy_meter_set_wiring_relay(s_current_calib.wiring_mode);
    esp_err_t ret = atm90e32as_apply_calibration(s_meter, &s_current_calib);
    xSemaphoreGive(s_meter_mutex);

    return ret;
}

esp_err_t energy_meter_save_calibration(void)
{
    ESP_RETURN_ON_FALSE(s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_calib_t calib = s_current_calib;
    xSemaphoreGive(s_meter_mutex);

    return energy_meter_save_calibration_to_nvs(&calib);
}

esp_err_t energy_meter_load_calibration(bool apply)
{
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    atm90e32as_calib_t calib;
    ESP_RETURN_ON_ERROR(energy_meter_load_calibration_from_nvs(&calib), TAG, "load calibration from NVS failed");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    s_current_calib = calib;
    esp_err_t ret = ESP_OK;
    if (apply) {
        ret = atm90e32as_apply_calibration(s_meter, &s_current_calib);
    }
    xSemaphoreGive(s_meter_mutex);

    return ret;
}

esp_err_t energy_meter_reset_calibration_defaults(bool apply)
{
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_line_freq_t line_freq = s_current_calib.line_freq;
    atm90e32as_wiring_mode_t wiring_mode = s_current_calib.wiring_mode;
    atm90e32as_get_default_calib(&s_current_calib);
    s_current_calib.line_freq = line_freq;
    s_current_calib.wiring_mode = wiring_mode;
    esp_err_t ret = ESP_OK;
    if (apply) {
        ret = atm90e32as_apply_calibration(s_meter, &s_current_calib);
    }
    xSemaphoreGive(s_meter_mutex);

    return ret;
}

esp_err_t energy_meter_task_start(void)
{
    ESP_RETURN_ON_ERROR(energy_meter_init(), TAG, "init energy meter failed");

    BaseType_t ret = xTaskCreate(energy_meter_task,
                                 "energy_meter_task",
                                 CONFIG_APP_ENERGY_METER_TASK_STACK_SIZE,
                                 NULL,
                                 CONFIG_APP_ENERGY_METER_TASK_PRIORITY,
                                 NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_FAIL, TAG, "create energy meter task failed");

    return ESP_OK;
}

#include "ds1307.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define DS1307_I2C_TIMEOUT_MS 300
/* Same reasoning as the PCF857x drivers: a burst of WiFi/TLS work can lap a
 * single transfer window, so a lone timeout is retried rather than reported. */
#define DS1307_I2C_READ_RETRIES 2

/* Clock registers, read as one 7-byte burst from 0x00. */
#define DS1307_REG_SECONDS 0x00
#define DS1307_CLOCK_REGS  7

/* Bit 7 of the seconds register halts the oscillator when set. */
#define DS1307_BIT_CH 0x80

struct ds1307_dev_t {
    i2c_master_dev_handle_t dev_handle;
    SemaphoreHandle_t mutex;
};

static const char *TAG = "ds1307";

static uint8_t bcd_to_bin(uint8_t bcd)
{
    return (uint8_t)(((bcd >> 4) * 10U) + (bcd & 0x0FU));
}

static uint8_t bin_to_bcd(uint8_t bin)
{
    return (uint8_t)(((bin / 10U) << 4) | (bin % 10U));
}

/* Caller holds the mutex. Retries a bare transfer timeout; any other error is
 * returned as-is so the caller can tell "bus busy" from "device absent". */
static esp_err_t ds1307_read_regs_locked(ds1307_handle_t handle, uint8_t reg,
                                         uint8_t *buf, size_t len)
{
    esp_err_t ret = ESP_OK;
    for (int attempt = 0; attempt <= DS1307_I2C_READ_RETRIES; ++attempt) {
        ret = i2c_master_transmit_receive(handle->dev_handle, &reg, 1, buf, len,
                                          DS1307_I2C_TIMEOUT_MS);
        if (ret != ESP_ERR_TIMEOUT) {
            break;
        }
        if (attempt < DS1307_I2C_READ_RETRIES) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }
    }
    return ret;
}

esp_err_t ds1307_create(i2c_master_bus_handle_t bus_handle, uint8_t address,
                        ds1307_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "bus_handle is NULL");
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    ds1307_handle_t dev = calloc(1, sizeof(struct ds1307_dev_t));
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_NO_MEM, TAG, "no memory for device");

    dev->mutex = xSemaphoreCreateMutex();
    if (dev->mutex == NULL) {
        free(dev);
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = CONFIG_APP_I2C_CLK_SPEED_HZ,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_config, &dev->dev_handle);
    if (ret != ESP_OK) {
        vSemaphoreDelete(dev->mutex);
        free(dev);
        return ret;
    }

    /* Probe: a chip that is not on the bus has to fail create, not fail later
     * on the first read. One register is enough to tell. */
    uint8_t seconds = 0;
    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    ret = ds1307_read_regs_locked(dev, DS1307_REG_SECONDS, &seconds, 1);
    xSemaphoreGive(dev->mutex);

    if (ret != ESP_OK) {
        i2c_master_bus_rm_device(dev->dev_handle);
        vSemaphoreDelete(dev->mutex);
        free(dev);
        return ret;
    }

    *handle = dev;
    return ESP_OK;
}

esp_err_t ds1307_delete(ds1307_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    esp_err_t ret = i2c_master_bus_rm_device(handle->dev_handle);
    vSemaphoreDelete(handle->mutex);
    free(handle);
    return ret;
}

esp_err_t ds1307_is_running(ds1307_handle_t handle, bool *running)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(running != NULL, ESP_ERR_INVALID_ARG, TAG, "running is NULL");

    uint8_t seconds = 0;
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    esp_err_t ret = ds1307_read_regs_locked(handle, DS1307_REG_SECONDS, &seconds, 1);
    xSemaphoreGive(handle->mutex);

    if (ret == ESP_OK) {
        *running = (seconds & DS1307_BIT_CH) == 0;
    }
    return ret;
}

esp_err_t ds1307_get_time(ds1307_handle_t handle, struct tm *out)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    uint8_t regs[DS1307_CLOCK_REGS] = { 0 };
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    esp_err_t ret = ds1307_read_regs_locked(handle, DS1307_REG_SECONDS, regs, sizeof(regs));
    xSemaphoreGive(handle->mutex);
    if (ret != ESP_OK) {
        return ret;
    }

    if (regs[0] & DS1307_BIT_CH) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Bit 6 of the hours register selects 12-hour mode. The driver only ever
     * writes 24-hour mode, but a chip programmed by something else can come
     * back in 12-hour mode, so decode both rather than return a wrong hour. */
    uint8_t hour_reg = regs[2];
    uint8_t hour;
    if (hour_reg & 0x40U) {
        hour = bcd_to_bin((uint8_t)(hour_reg & 0x1FU)) % 12U;
        if (hour_reg & 0x20U) {
            hour = (uint8_t)(hour + 12U);
        }
    } else {
        hour = bcd_to_bin((uint8_t)(hour_reg & 0x3FU));
    }

    uint8_t second = bcd_to_bin((uint8_t)(regs[0] & 0x7FU));
    uint8_t minute = bcd_to_bin((uint8_t)(regs[1] & 0x7FU));
    uint8_t mday   = bcd_to_bin((uint8_t)(regs[4] & 0x3FU));
    uint8_t month  = bcd_to_bin((uint8_t)(regs[5] & 0x1FU));
    uint8_t year   = bcd_to_bin(regs[6]);

    /* An unpowered or corrupted chip reads back as out-of-range BCD far more
     * often than as a plausible date; reject it here so the caller never has
     * to guess whether a struct tm is real. */
    if (second > 59U || minute > 59U || hour > 23U ||
        mday < 1U || mday > 31U || month < 1U || month > 12U) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    memset(out, 0, sizeof(*out));
    out->tm_sec  = second;
    out->tm_min  = minute;
    out->tm_hour = hour;
    out->tm_mday = mday;
    out->tm_mon  = month - 1;
    out->tm_year = (DS1307_YEAR_MIN - 1900) + year;
    out->tm_isdst = -1;
    return ESP_OK;
}

esp_err_t ds1307_set_time(ds1307_handle_t handle, const struct tm *in)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "in is NULL");

    int year = in->tm_year + 1900;
    ESP_RETURN_ON_FALSE(year >= DS1307_YEAR_MIN && year <= DS1307_YEAR_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "year %d out of range", year);

    /* Normalize a copy so tm_wday is right even when the caller left it unset,
     * and so any out-of-range field is carried into the neighbouring one
     * instead of being written to the chip as-is. */
    struct tm norm = *in;
    norm.tm_isdst = -1;
    if (mktime(&norm) == (time_t)-1) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[1 + DS1307_CLOCK_REGS] = {
        DS1307_REG_SECONDS,
        bin_to_bcd((uint8_t)norm.tm_sec),        /* CH cleared: bit 7 stays 0 */
        bin_to_bcd((uint8_t)norm.tm_min),
        bin_to_bcd((uint8_t)norm.tm_hour),       /* bit 6 clear: 24-hour mode */
        (uint8_t)(norm.tm_wday + 1),             /* chip counts 1..7 */
        bin_to_bcd((uint8_t)norm.tm_mday),
        bin_to_bcd((uint8_t)(norm.tm_mon + 1)),
        bin_to_bcd((uint8_t)((norm.tm_year + 1900) - DS1307_YEAR_MIN)),
    };

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    esp_err_t ret = i2c_master_transmit(handle->dev_handle, payload, sizeof(payload),
                                        DS1307_I2C_TIMEOUT_MS);
    xSemaphoreGive(handle->mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set time failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

#include "pcf8575.h"

#include <stdlib.h>
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#define PCF8575_I2C_TIMEOUT_MS 100

struct pcf8575_dev_t {
    i2c_master_dev_handle_t dev_handle;
    SemaphoreHandle_t mutex;
    uint16_t output_latch;
    uint16_t input_mask;
};

static const char *TAG = "pcf8575";

static esp_err_t pcf8575_transmit_latch_locked(pcf8575_handle_t handle)
{
    uint16_t value = handle->output_latch | handle->input_mask;
    uint8_t tx[2] = {
        (uint8_t)(value & 0xFF),
        (uint8_t)((value >> 8) & 0xFF),
    };
    esp_err_t ret = i2c_master_transmit(handle->dev_handle, tx, sizeof(tx), PCF8575_I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
        handle->output_latch = value;
    }
    return ret;
}

esp_err_t pcf8575_create(i2c_master_bus_handle_t bus_handle, uint8_t address, pcf8575_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "bus_handle is NULL");
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    pcf8575_handle_t dev = calloc(1, sizeof(struct pcf8575_dev_t));
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

    dev->output_latch = 0xFFFF;
    dev->input_mask = 0xFFFF;

    xSemaphoreTake(dev->mutex, portMAX_DELAY);
    ret = pcf8575_transmit_latch_locked(dev);
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

esp_err_t pcf8575_delete(pcf8575_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    esp_err_t ret = i2c_master_bus_rm_device(handle->dev_handle);
    vSemaphoreDelete(handle->mutex);
    free(handle);
    return ret;
}

esp_err_t pcf8575_set_pin_mode(pcf8575_handle_t handle, uint8_t pin, pcf8575_pin_mode_t mode)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(pin < PCF8575_PIN_COUNT, ESP_ERR_INVALID_ARG, TAG, "invalid pin");

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    if (mode == PCF8575_PIN_MODE_INPUT) {
        handle->input_mask |= (1U << pin);
        handle->output_latch |= (1U << pin);
    } else {
        handle->input_mask &= ~(1U << pin);
    }
    esp_err_t ret = pcf8575_transmit_latch_locked(handle);
    xSemaphoreGive(handle->mutex);

    return ret;
}

esp_err_t pcf8575_set_direction_mask(pcf8575_handle_t handle, uint16_t input_mask)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    handle->input_mask = input_mask;
    handle->output_latch |= input_mask;
    esp_err_t ret = pcf8575_transmit_latch_locked(handle);
    xSemaphoreGive(handle->mutex);

    return ret;
}

esp_err_t pcf8575_get_latch(pcf8575_handle_t handle, uint16_t *value)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "value is NULL");

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    *value = handle->output_latch;
    xSemaphoreGive(handle->mutex);

    return ESP_OK;
}

esp_err_t pcf8575_write_port(pcf8575_handle_t handle, uint16_t value)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    handle->output_latch = value | handle->input_mask;
    esp_err_t ret = pcf8575_transmit_latch_locked(handle);
    xSemaphoreGive(handle->mutex);

    return ret;
}

esp_err_t pcf8575_read_port(pcf8575_handle_t handle, uint16_t *value)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "value is NULL");

    uint8_t rx[2] = {0};
    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    esp_err_t ret = i2c_master_receive(handle->dev_handle, rx, sizeof(rx), PCF8575_I2C_TIMEOUT_MS);
    xSemaphoreGive(handle->mutex);

    if (ret == ESP_OK) {
        *value = (uint16_t)rx[0] | ((uint16_t)rx[1] << 8);
    }
    return ret;
}

esp_err_t pcf8575_write_pin(pcf8575_handle_t handle, uint8_t pin, bool level)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(pin < PCF8575_PIN_COUNT, ESP_ERR_INVALID_ARG, TAG, "invalid pin");

    xSemaphoreTake(handle->mutex, portMAX_DELAY);
    bool is_input = (handle->input_mask & (1U << pin)) != 0;
    if (is_input && !level) {
        xSemaphoreGive(handle->mutex);
        return ESP_ERR_INVALID_STATE;
    }

    if (level) {
        handle->output_latch |= (1U << pin);
    } else {
        handle->output_latch &= ~(1U << pin);
    }

    esp_err_t ret = pcf8575_transmit_latch_locked(handle);
    xSemaphoreGive(handle->mutex);

    return ret;
}

esp_err_t pcf8575_read_pin(pcf8575_handle_t handle, uint8_t pin, bool *level)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(pin < PCF8575_PIN_COUNT, ESP_ERR_INVALID_ARG, TAG, "invalid pin");
    ESP_RETURN_ON_FALSE(level != NULL, ESP_ERR_INVALID_ARG, TAG, "level is NULL");

    uint16_t port = 0;
    esp_err_t ret = pcf8575_read_port(handle, &port);
    if (ret == ESP_OK) {
        *level = (port & (1U << pin)) != 0;
    }
    return ret;
}

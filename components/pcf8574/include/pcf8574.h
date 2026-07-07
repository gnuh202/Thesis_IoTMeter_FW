#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCF8574_PIN_COUNT 8

typedef struct pcf8574_dev_t *pcf8574_handle_t;

typedef enum {
    PCF8574_PIN_MODE_OUTPUT = 0,
    PCF8574_PIN_MODE_INPUT,
} pcf8574_pin_mode_t;

esp_err_t pcf8574_create(i2c_master_bus_handle_t bus_handle, uint8_t address, pcf8574_handle_t *handle);
esp_err_t pcf8574_delete(pcf8574_handle_t handle);
esp_err_t pcf8574_set_pin_mode(pcf8574_handle_t handle, uint8_t pin, pcf8574_pin_mode_t mode);
esp_err_t pcf8574_set_direction_mask(pcf8574_handle_t handle, uint8_t input_mask);
esp_err_t pcf8574_get_latch(pcf8574_handle_t handle, uint8_t *value);
esp_err_t pcf8574_write_port(pcf8574_handle_t handle, uint8_t value);
esp_err_t pcf8574_read_port(pcf8574_handle_t handle, uint8_t *value);
esp_err_t pcf8574_write_pin(pcf8574_handle_t handle, uint8_t pin, bool level);
esp_err_t pcf8574_read_pin(pcf8574_handle_t handle, uint8_t pin, bool *level);

#ifdef __cplusplus
}
#endif

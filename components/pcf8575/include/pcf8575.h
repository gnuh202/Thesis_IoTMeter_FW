#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PCF8575_PIN_COUNT 16

typedef struct pcf8575_dev_t *pcf8575_handle_t;

typedef enum {
    PCF8575_PIN_MODE_OUTPUT = 0,
    PCF8575_PIN_MODE_INPUT,
} pcf8575_pin_mode_t;

esp_err_t pcf8575_create(i2c_master_bus_handle_t bus_handle, uint8_t address, pcf8575_handle_t *handle);
esp_err_t pcf8575_delete(pcf8575_handle_t handle);
esp_err_t pcf8575_set_pin_mode(pcf8575_handle_t handle, uint8_t pin, pcf8575_pin_mode_t mode);
esp_err_t pcf8575_set_direction_mask(pcf8575_handle_t handle, uint16_t input_mask);
esp_err_t pcf8575_get_latch(pcf8575_handle_t handle, uint16_t *value);
esp_err_t pcf8575_write_port(pcf8575_handle_t handle, uint16_t value);
esp_err_t pcf8575_read_port(pcf8575_handle_t handle, uint16_t *value);
esp_err_t pcf8575_write_pin(pcf8575_handle_t handle, uint8_t pin, bool level);
esp_err_t pcf8575_read_pin(pcf8575_handle_t handle, uint8_t pin, bool *level);

#ifdef __cplusplus
}
#endif

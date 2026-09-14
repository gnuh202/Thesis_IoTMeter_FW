#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lcd2004_i2c_dev_t *lcd2004_i2c_handle_t;

esp_err_t lcd2004_i2c_create(i2c_master_bus_handle_t bus_handle, uint8_t address, lcd2004_i2c_handle_t *handle);
esp_err_t lcd2004_i2c_delete(lcd2004_i2c_handle_t handle);
esp_err_t lcd2004_i2c_init(lcd2004_i2c_handle_t handle);
esp_err_t lcd2004_i2c_clear(lcd2004_i2c_handle_t handle);
esp_err_t lcd2004_i2c_home(lcd2004_i2c_handle_t handle);
esp_err_t lcd2004_i2c_set_cursor(lcd2004_i2c_handle_t handle, uint8_t row, uint8_t col);
esp_err_t lcd2004_i2c_write_char(lcd2004_i2c_handle_t handle, char c);
esp_err_t lcd2004_i2c_write_str(lcd2004_i2c_handle_t handle, const char *text);
esp_err_t lcd2004_i2c_print_line(lcd2004_i2c_handle_t handle, uint8_t row, const char *text);
esp_err_t lcd2004_i2c_backlight(lcd2004_i2c_handle_t handle, bool on);
esp_err_t lcd2004_i2c_create_char(lcd2004_i2c_handle_t handle, uint8_t location, const uint8_t charmap[8]);

#ifdef __cplusplus
}
#endif

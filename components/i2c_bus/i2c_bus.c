#include "i2c_bus.h"

#include <stdbool.h>
#include "esp_check.h"
#include "sdkconfig.h"

#define I2C_BUS_PROBE_TIMEOUT_MS 50
#define I2C_BUS_SCAN_FIRST_ADDR 0x08
#define I2C_BUS_SCAN_LAST_ADDR 0x77

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t s_i2c_bus_handle = NULL;

esp_err_t i2c_bus_init(void)
{
    if (s_i2c_bus_handle != NULL) {
        return ESP_OK;
    }

    i2c_master_bus_config_t bus_config = {
        .i2c_port = CONFIG_APP_I2C_PORT_NUM,
        .sda_io_num = CONFIG_APP_I2C_SDA_IO,
        .scl_io_num = CONFIG_APP_I2C_SCL_IO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&bus_config, &s_i2c_bus_handle);
}

i2c_master_bus_handle_t i2c_bus_get_handle(void)
{
    return s_i2c_bus_handle;
}

esp_err_t i2c_bus_scan(uint8_t *addrs, size_t max, size_t *found)
{
    ESP_RETURN_ON_FALSE(found != NULL, ESP_ERR_INVALID_ARG, TAG, "found is NULL");
    ESP_RETURN_ON_FALSE(addrs != NULL || max == 0, ESP_ERR_INVALID_ARG, TAG, "addrs is NULL");
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "init I2C bus failed");

    *found = 0;
    for (uint8_t addr = I2C_BUS_SCAN_FIRST_ADDR; addr <= I2C_BUS_SCAN_LAST_ADDR; addr++) {
        /* Anything other than ESP_OK means "no device answered here", which is
         * the normal case for most addresses and never a scan failure. */
        if (i2c_master_probe(s_i2c_bus_handle, addr, I2C_BUS_PROBE_TIMEOUT_MS) != ESP_OK) {
            continue;
        }
        if (*found < max) {
            addrs[*found] = addr;
        }
        (*found)++;
    }

    return ESP_OK;
}

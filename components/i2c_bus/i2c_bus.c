#include "i2c_bus.h"

#include <stdbool.h>
#include "sdkconfig.h"

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

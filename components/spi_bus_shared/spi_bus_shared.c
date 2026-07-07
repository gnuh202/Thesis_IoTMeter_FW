#include "spi_bus_shared.h"

#include "esp_check.h"
#include "sdkconfig.h"

static const char *TAG = "spi_bus_shared";
static bool s_spi_bus_initialized;

spi_host_device_t spi_bus_shared_get_host(void)
{
    return (CONFIG_APP_SHARED_SPI_HOST == 1) ? SPI2_HOST : SPI3_HOST;
}

esp_err_t spi_bus_shared_init(void)
{
    if (s_spi_bus_initialized) {
        return ESP_OK;
    }

    spi_bus_config_t bus_config = {
        .mosi_io_num = CONFIG_APP_SHARED_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_APP_SHARED_SPI_MISO_GPIO,
        .sclk_io_num = CONFIG_APP_SHARED_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    esp_err_t ret = spi_bus_initialize(spi_bus_shared_get_host(), &bus_config, SPI_DMA_CH_AUTO);
    if (ret == ESP_ERR_INVALID_STATE) {
        s_spi_bus_initialized = true;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ret, TAG, "initialize shared SPI bus failed");
    s_spi_bus_initialized = true;
    return ESP_OK;
}

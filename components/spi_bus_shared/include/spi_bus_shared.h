#pragma once

#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t spi_bus_shared_init(void);
spi_host_device_t spi_bus_shared_get_host(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2c_bus_init(void);
i2c_master_bus_handle_t i2c_bus_get_handle(void);

/* Probe every 7-bit address in 0x08..0x77 and store the ones that ACK into
 * addrs (up to max entries). found always receives the number of devices seen,
 * even if it exceeds max. Initializes the bus if needed. Used by the IO expander
 * diagnostics to prove the PCF8574 is present at its configured address. */
esp_err_t i2c_bus_scan(uint8_t *addrs, size_t max, size_t *found);

#ifdef __cplusplus
}
#endif

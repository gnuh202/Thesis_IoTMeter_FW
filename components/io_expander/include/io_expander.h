#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t io_expander_start(void);
esp_err_t io_expander_w5500_reset_pulse(void);
esp_err_t io_expander_set_out0(bool level);
esp_err_t io_expander_set_out1(bool level);
esp_err_t io_expander_get_in0(bool *level);
esp_err_t io_expander_get_in1(bool *level);

/* Last-set output relay levels (OUT pins are write-only on the PCF8574, so these
 * return the cached value from the most recent io_expander_set_outN call). */
esp_err_t io_expander_get_out0(bool *level);
esp_err_t io_expander_get_out1(bool *level);

#ifdef __cplusplus
}
#endif

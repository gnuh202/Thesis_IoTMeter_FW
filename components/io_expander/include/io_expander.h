#pragma once

#include <stdbool.h>
#include <stdint.h>
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

/* Log the full PCF8574 picture: configured pin map, I2C scan, direction mask,
 * cached latch versus a real device readback, and current pin levels. Called
 * before the W5500 reset pulse and from the "ioexp diag" console command. */
esp_err_t io_expander_diag_dump(void);

/* Raw port access for bring-up and the console command. read_port samples the
 * physical pins; write_pin_raw drives one bit and optionally reports the port
 * byte read back afterwards. Neither updates the cached OUT levels, so prefer
 * io_expander_set_outN for normal relay control. */
esp_err_t io_expander_read_port(uint8_t *port);
esp_err_t io_expander_write_pin_raw(uint8_t pin, bool level, uint8_t *port_after);

/* Last-set output relay levels (OUT pins are write-only on the PCF8574, so these
 * return the cached value from the most recent io_expander_set_outN call). */
esp_err_t io_expander_get_out0(bool *level);
esp_err_t io_expander_get_out1(bool *level);

#ifdef __cplusplus
}
#endif

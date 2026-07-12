#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the global GPIO ISR service once for the whole firmware.
 *
 * Several drivers may need gpio_isr_handler_add(). Calling
 * gpio_install_isr_service() more than once is harmless functionally, but ESP-IDF
 * prints an error log before returning ESP_ERR_INVALID_STATE. This helper avoids
 * that noisy duplicate install path.
 */
esp_err_t gpio_isr_service_ensure_installed(int intr_alloc_flags);

#ifdef __cplusplus
}
#endif

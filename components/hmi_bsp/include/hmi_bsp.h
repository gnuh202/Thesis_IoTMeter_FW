#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HMI_BSP_BUTTON_RIGHT  (1U << 0)
#define HMI_BSP_BUTTON_BOTTOM (1U << 1)
#define HMI_BSP_BUTTON_CENTER (1U << 2)
#define HMI_BSP_BUTTON_LEFT   (1U << 3)
#define HMI_BSP_BUTTON_TOP    (1U << 4)

#define HMI_BSP_LED_COUNT 5

esp_err_t hmi_bsp_init(void);
esp_err_t hmi_bsp_lcd_print_line(uint8_t row, const char *text);

/* Turn the LCD backlight on/off. The display content is untouched — the
 * controller keeps its DDRAM, so switching back on restores the same screen. */
esp_err_t hmi_bsp_lcd_backlight(bool on);
esp_err_t hmi_bsp_read_buttons(uint8_t *pressed_mask);
esp_err_t hmi_bsp_set_leds(uint8_t led_mask);
esp_err_t hmi_bsp_set_led(uint8_t index, bool on);
esp_err_t hmi_bsp_buzzer_set(bool on);
esp_err_t hmi_bsp_buzzer_click(uint32_t ms);

#ifdef __cplusplus
}
#endif

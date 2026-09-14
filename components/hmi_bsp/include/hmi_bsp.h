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

/* Create a custom character in CGRAM (location 0-7). The charmap is 8 bytes
 * where each byte defines one row (5 pixels wide, bits 0-4). */
esp_err_t hmi_bsp_lcd_create_char(uint8_t location, const uint8_t charmap[8]);

/* Set LCD cursor position (row 0-3, col 0-19). */
esp_err_t hmi_bsp_lcd_set_cursor(uint8_t row, uint8_t col);

/* Write a single character at current cursor position. */
esp_err_t hmi_bsp_lcd_write_char(char c);

esp_err_t hmi_bsp_read_buttons(uint8_t *pressed_mask);
esp_err_t hmi_bsp_set_leds(uint8_t led_mask);
esp_err_t hmi_bsp_set_led(uint8_t index, bool on);
esp_err_t hmi_bsp_buzzer_set(bool on);
esp_err_t hmi_bsp_buzzer_click(uint32_t ms);

#ifdef __cplusplus
}
#endif

#include "hmi_bsp.h"

#include <stddef.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "lcd2004_i2c.h"
#include "pcf8575.h"
#include "sdkconfig.h"

#define HMI_PCF8575_PIN_BUZZER 0
#define HMI_PCF8575_PIN_SW_RIGHT 1
#define HMI_PCF8575_PIN_SW_BOTTOM 2
#define HMI_PCF8575_PIN_SW_CENTER 3
#define HMI_PCF8575_PIN_SW_LEFT 4
#define HMI_PCF8575_PIN_SW_TOP 5
#define HMI_PCF8575_PIN_LED_BASE 8

#define HMI_BUTTON_INPUT_MASK ((1U << HMI_PCF8575_PIN_SW_RIGHT) | \
                               (1U << HMI_PCF8575_PIN_SW_BOTTOM) | \
                               (1U << HMI_PCF8575_PIN_SW_CENTER) | \
                               (1U << HMI_PCF8575_PIN_SW_LEFT) | \
                               (1U << HMI_PCF8575_PIN_SW_TOP))

static const char *TAG = "hmi_bsp";

static pcf8575_handle_t s_io;
static lcd2004_i2c_handle_t s_lcd;
static bool s_initialized;
static uint8_t s_led_mask;
static bool s_buzzer_on;

static uint16_t hmi_bsp_make_output_latch(void)
{
    uint16_t latch = HMI_BUTTON_INPUT_MASK;

    /* Buzzer: for an active-low buzzer the PCF8575 pin must idle HIGH and be
     * driven LOW to sound; only set the bit when we want it silent. */
#if CONFIG_APP_HMI_BUZZER_ACTIVE_HIGH
    bool buzzer_level = s_buzzer_on;
#else
    bool buzzer_level = !s_buzzer_on;
#endif
    if (buzzer_level) {
        latch |= (1U << HMI_PCF8575_PIN_BUZZER);
    }

    for (uint8_t i = 0; i < HMI_BSP_LED_COUNT; ++i) {
        bool on = (s_led_mask & (1U << i)) != 0;
#if CONFIG_APP_HMI_LED_ACTIVE_HIGH
        bool level = on;
#else
        bool level = !on;
#endif
        if (level) {
            latch |= (1U << (HMI_PCF8575_PIN_LED_BASE + i));
        }
    }

    return latch;
}

static esp_err_t hmi_bsp_flush_outputs(void)
{
    return pcf8575_write_port(s_io, hmi_bsp_make_output_latch());
}

esp_err_t hmi_bsp_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "init I2C bus failed");
    ESP_RETURN_ON_ERROR(lcd2004_i2c_create(i2c_bus_get_handle(), CONFIG_APP_HMI_LCD2004_I2C_ADDR, &s_lcd), TAG, "create LCD failed");
    ESP_RETURN_ON_ERROR(lcd2004_i2c_init(s_lcd), TAG, "init LCD failed");

    ESP_RETURN_ON_ERROR(pcf8575_create(i2c_bus_get_handle(), CONFIG_APP_HMI_PCF8575_I2C_ADDR, &s_io), TAG, "create PCF8575 failed");
    ESP_RETURN_ON_ERROR(pcf8575_set_direction_mask(s_io, HMI_BUTTON_INPUT_MASK), TAG, "set PCF8575 direction failed");
    ESP_RETURN_ON_ERROR(hmi_bsp_flush_outputs(), TAG, "set initial HMI outputs failed");

    ESP_LOGI(TAG, "HMI initialized: LCD=0x%02X PCF8575=0x%02X", CONFIG_APP_HMI_LCD2004_I2C_ADDR, CONFIG_APP_HMI_PCF8575_I2C_ADDR);
    s_initialized = true;
    return ESP_OK;
}

esp_err_t hmi_bsp_lcd_print_line(uint8_t row, const char *text)
{
    ESP_RETURN_ON_ERROR(hmi_bsp_init(), TAG, "init HMI failed");
    return lcd2004_i2c_print_line(s_lcd, row, text);
}

esp_err_t hmi_bsp_lcd_backlight(bool on)
{
    ESP_RETURN_ON_ERROR(hmi_bsp_init(), TAG, "init HMI failed");
    return lcd2004_i2c_backlight(s_lcd, on);
}

esp_err_t hmi_bsp_read_buttons(uint8_t *pressed_mask)
{
    ESP_RETURN_ON_FALSE(pressed_mask != NULL, ESP_ERR_INVALID_ARG, TAG, "pressed_mask is NULL");
    ESP_RETURN_ON_ERROR(hmi_bsp_init(), TAG, "init HMI failed");

    uint16_t port = 0;
    ESP_RETURN_ON_ERROR(pcf8575_read_port(s_io, &port), TAG, "read PCF8575 failed");

    struct button_map_t {
        uint8_t pin;
        uint8_t mask;
    } buttons[] = {
        {HMI_PCF8575_PIN_SW_RIGHT, HMI_BSP_BUTTON_RIGHT},
        /* New PCB swaps the physical TOP/BOTTOM switch positions.
         * The UI still treats TOP as UP/increment and BOTTOM as DOWN/decrement,
         * so remap at the BSP layer instead of changing every menu. */
        {HMI_PCF8575_PIN_SW_BOTTOM, HMI_BSP_BUTTON_TOP},
        {HMI_PCF8575_PIN_SW_CENTER, HMI_BSP_BUTTON_CENTER},
        {HMI_PCF8575_PIN_SW_LEFT, HMI_BSP_BUTTON_LEFT},
        {HMI_PCF8575_PIN_SW_TOP, HMI_BSP_BUTTON_BOTTOM},
    };

    uint8_t mask = 0;
    for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); ++i) {
        bool level = (port & (1U << buttons[i].pin)) != 0;
#if CONFIG_APP_HMI_BUTTON_ACTIVE_LOW
        bool pressed = !level;
#else
        bool pressed = level;
#endif
        if (pressed) {
            mask |= buttons[i].mask;
        }
    }

    *pressed_mask = mask;
    return ESP_OK;
}

esp_err_t hmi_bsp_set_leds(uint8_t led_mask)
{
    ESP_RETURN_ON_ERROR(hmi_bsp_init(), TAG, "init HMI failed");
    s_led_mask = led_mask & ((1U << HMI_BSP_LED_COUNT) - 1U);
    return hmi_bsp_flush_outputs();
}

esp_err_t hmi_bsp_set_led(uint8_t index, bool on)
{
    ESP_RETURN_ON_FALSE(index < HMI_BSP_LED_COUNT, ESP_ERR_INVALID_ARG, TAG, "invalid LED index");
    uint8_t mask = s_led_mask;
    if (on) {
        mask |= (1U << index);
    } else {
        mask &= ~(1U << index);
    }
    return hmi_bsp_set_leds(mask);
}

esp_err_t hmi_bsp_buzzer_set(bool on)
{
    ESP_RETURN_ON_ERROR(hmi_bsp_init(), TAG, "init HMI failed");
    s_buzzer_on = on;
    return hmi_bsp_flush_outputs();
}

esp_err_t hmi_bsp_buzzer_click(uint32_t ms)
{
    if (ms == 0) {
        /* Click disabled: make sure the buzzer stays silent. */
        return hmi_bsp_buzzer_set(false);
    }

    ESP_RETURN_ON_ERROR(hmi_bsp_buzzer_set(true), TAG, "buzzer on failed");
    vTaskDelay(pdMS_TO_TICKS(ms));
    return hmi_bsp_buzzer_set(false);
}

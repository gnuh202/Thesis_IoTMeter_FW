#include "boot_manager.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hmi_bsp.h"

/*
 * Boot manager. Owns only the boot-time LCD UI + a module status table. Module
 * init ownership stays in each module; app_tasks_start() feeds results here.
 */

#define BOOT_SPLASH_MS 1500
#define BOOT_LCD_WIDTH 20
#define BOOT_LCD_ROWS 4

static const char *TAG = "BOOT";

static bool s_lcd_ok;
static boot_module_status_t s_modules[BOOT_MODULE_MAX];
static uint8_t s_module_count;
static uint8_t s_lcd_line;   /* next LCD row to write on the init screen */

/*
 * Custom character definitions for "PM" logo (3 rows tall).
 * Each character is 5x8 pixels. We use 8 custom characters (0-7).
 * 0xFF represents LCD's built-in full block character (not CGRAM).
 */
static const uint8_t CGRAM_LOGO[][8] = {
    /* Char 0x00 */
    {0b11000, 0b11000, 0b11100, 0b11100, 0b11110, 0b11110, 0b11111, 0b11111},
    /* Char 0x01 */
    {0b00000, 0b00000, 0b00000, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111},
    /* Char 0x02 */
    {0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b00000, 0b00000, 0b00000},
    /* Char 0x03 */
    {0b11000, 0b11100, 0b11110, 0b11111, 0b11111, 0b11111, 0b11111, 0b11111},
    /* Char 0x04 */
    {0b11111, 0b11111, 0b11111, 0b11111, 0b11111, 0b11110, 0b11100, 0b11000},
    /* Char 0x05 */
    {0b11111, 0b11110, 0b11110, 0b11100, 0b11100, 0b11000, 0b11000, 0b10000},
    /* Char 0x06 */
    {0b11111, 0b01111, 0b01111, 0b00111, 0b00111, 0b00011, 0b00011, 0b00001},
    /* Char 0x07 */
    {0b00011, 0b00011, 0b00111, 0b00111, 0b01111, 0b01111, 0b11111, 0b11111},
};

static void load_custom_chars(void)
{
    for (uint8_t i = 0; i < 8; i++) {
        hmi_bsp_lcd_create_char(i, CGRAM_LOGO[i]);
    }
}

static void show_splash(void)
{
    load_custom_chars();

    /* Row 0: Contains 0x00 byte, must use binary array not string literal */
    const uint8_t row0[20] = {
        ' ', ' ', ' ', ' ', ' ', 0xFF, 0x02, 0x02, 0x03, ' ',
        0xFF, 0x00, ' ', 0x07, 0xFF, ' ', ' ', ' ', ' ', ' '
    };

    /* Row 1: Safe to use string literal (no 0x00) */
    hmi_bsp_lcd_print_line(1, "     \xFF\x01\x01\x04 \xFF\x06\xFF\x05\xFF");

    /* Row 2: Safe to use string literal (no 0x00) */
    hmi_bsp_lcd_print_line(2, "     \xFF    \xFF   \xFF");

    /* Row 3: "Hung Nguyen" centered */
    hmi_bsp_lcd_print_line(3, "    Hung  Nguyen");

    /* Write row 0 manually since it contains NULL byte */
    hmi_bsp_lcd_set_cursor(0, 0);
    for (uint8_t i = 0; i < 20; i++) {
        hmi_bsp_lcd_write_char(row0[i]);
    }
}

esp_err_t boot_manager_begin(void)
{
    esp_err_t ret = hmi_bsp_init();
    s_lcd_ok = (ret == ESP_OK);
    if (!s_lcd_ok) {
        ESP_LOGE(TAG, "LCD/HMI init failed: %s; continuing headless", esp_err_to_name(ret));
        return ret;
    }

    show_splash();
    vTaskDelay(pdMS_TO_TICKS(BOOT_SPLASH_MS));

    /* Clear screen silently - no "Initializing..." message */
    hmi_bsp_lcd_print_line(0, "");
    hmi_bsp_lcd_print_line(1, "");
    hmi_bsp_lcd_print_line(2, "");
    hmi_bsp_lcd_print_line(3, "");
    s_lcd_line = 0;  /* Use all 4 rows for boot status if needed */

    ESP_LOGI(TAG, "Boot started");
    return ESP_OK;
}

void boot_manager_step(const char *label, esp_err_t result)
{
    if (label == NULL) {
        return;
    }

    bool ok = (result == ESP_OK);
    if (ok) {
        ESP_LOGI(TAG, "Init %s", label);
    } else {
        ESP_LOGE(TAG, "%s Failed: %s", label, esp_err_to_name(result));
    }

    if (s_module_count < BOOT_MODULE_MAX) {
        strlcpy(s_modules[s_module_count].name, label, sizeof(s_modules[s_module_count].name));
        s_modules[s_module_count].ok = ok;
        s_module_count++;
    }

    /* LCD stays blank during boot - no status messages shown */
}

void boot_manager_end(void)
{
    ESP_LOGI(TAG, "Boot complete");
    if (s_lcd_ok) {
        hmi_bsp_lcd_print_line(0, "Boot complete");
    }
}

bool boot_manager_engineering_mode(void)
{
    /* Cache the result at boot — it's constant for the session lifetime.
     * User and engineer modes are completely isolated: device enters one mode
     * at boot and must reset to switch modes. */
    static bool s_cached = false;
    static bool s_engineering_mode = false;

    if (s_cached) {
        return s_engineering_mode;
    }

    if (!s_lcd_ok) {
        s_engineering_mode = false;
        s_cached = true;
        return false;
    }

    uint8_t mask = 0;
    if (hmi_bsp_read_buttons(&mask) != ESP_OK) {
        s_engineering_mode = false;
        s_cached = true;
        return false;
    }
    /* LEFT + RIGHT held together at boot selects engineering mode. */
    s_engineering_mode = (mask & HMI_BSP_BUTTON_LEFT) && (mask & HMI_BSP_BUTTON_RIGHT);
    s_cached = true;
    return s_engineering_mode;
}

uint8_t boot_manager_get_status(const boot_module_status_t **out)
{
    if (out != NULL) {
        *out = s_modules;
    }
    return s_module_count;
}

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

/* Center text into a width-BOOT_LCD_WIDTH field for the splash. */
static void center_line(const char *text, char *out, size_t out_size)
{
    size_t len = strlen(text);
    if (len > BOOT_LCD_WIDTH) {
        len = BOOT_LCD_WIDTH;
    }
    size_t pad = (BOOT_LCD_WIDTH - len) / 2;
    memset(out, ' ', BOOT_LCD_WIDTH);
    memcpy(out + pad, text, len);
    if (out_size > BOOT_LCD_WIDTH) {
        out[BOOT_LCD_WIDTH] = '\0';
    } else {
        out[out_size - 1] = '\0';
    }
}

static void show_splash(void)
{
    char line[BOOT_LCD_WIDTH + 1];

    center_line("Power Meter", line, sizeof(line));
    hmi_bsp_lcd_print_line(0, line);
    center_line("IoT Energy Meter", line, sizeof(line));
    hmi_bsp_lcd_print_line(1, line);
    center_line("", line, sizeof(line));
    hmi_bsp_lcd_print_line(2, line);
    center_line("v1.0", line, sizeof(line));
    hmi_bsp_lcd_print_line(3, line);
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

    hmi_bsp_lcd_print_line(0, "Initializing...");
    hmi_bsp_lcd_print_line(1, "");
    hmi_bsp_lcd_print_line(2, "");
    hmi_bsp_lcd_print_line(3, "");
    s_lcd_line = 1;

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

    if (s_lcd_ok) {
        char line[BOOT_LCD_WIDTH + 1];
        snprintf(line, sizeof(line), "%-15s %s", label, ok ? "OK" : "ERR");
        hmi_bsp_lcd_print_line(s_lcd_line, line);
        /* Rows 1..3 scroll; keep row 0 as the "Initializing..." title. */
        s_lcd_line++;
        if (s_lcd_line >= BOOT_LCD_ROWS) {
            s_lcd_line = 1;
        }
    }
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
    if (!s_lcd_ok) {
        return false;
    }

    uint8_t mask = 0;
    if (hmi_bsp_read_buttons(&mask) != ESP_OK) {
        return false;
    }
    /* LEFT + RIGHT held together at boot selects engineering mode. */
    return (mask & HMI_BSP_BUTTON_LEFT) && (mask & HMI_BSP_BUTTON_RIGHT);
}

uint8_t boot_manager_get_status(const boot_module_status_t **out)
{
    if (out != NULL) {
        *out = s_modules;
    }
    return s_module_count;
}

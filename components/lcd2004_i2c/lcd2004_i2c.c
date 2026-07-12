#include "lcd2004_i2c.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pcf8574.h"

#define LCD_COLS 20
#define LCD_ROWS 4

#define LCD_BIT_RS 0x01
#define LCD_BIT_RW 0x02
#define LCD_BIT_EN 0x04
#define LCD_BIT_BL 0x08

#define LCD_CMD_CLEAR 0x01
#define LCD_CMD_HOME 0x02
#define LCD_CMD_ENTRY_MODE 0x06
#define LCD_CMD_DISPLAY_ON 0x0C
#define LCD_CMD_FUNCTION_SET 0x28

struct lcd2004_i2c_dev_t {
    pcf8574_handle_t io;
    uint8_t backlight;
};

static const char *TAG = "lcd2004_i2c";

static esp_err_t lcd_write_raw(lcd2004_i2c_handle_t handle, uint8_t value)
{
    return pcf8574_write_port(handle->io, value | handle->backlight);
}

static esp_err_t lcd_pulse_enable(lcd2004_i2c_handle_t handle, uint8_t value)
{
    ESP_RETURN_ON_ERROR(lcd_write_raw(handle, value | LCD_BIT_EN), TAG, "LCD EN high failed");
    esp_rom_delay_us(1);
    ESP_RETURN_ON_ERROR(lcd_write_raw(handle, value & ~LCD_BIT_EN), TAG, "LCD EN low failed");
    esp_rom_delay_us(50);
    return ESP_OK;
}

static esp_err_t lcd_write_nibble(lcd2004_i2c_handle_t handle, uint8_t nibble, bool rs)
{
    uint8_t value = (uint8_t)((nibble & 0x0F) << 4);
    if (rs) {
        value |= LCD_BIT_RS;
    }
    return lcd_pulse_enable(handle, value);
}

static esp_err_t lcd_write_byte(lcd2004_i2c_handle_t handle, uint8_t value, bool rs)
{
    ESP_RETURN_ON_ERROR(lcd_write_nibble(handle, value >> 4, rs), TAG, "write high nibble failed");
    ESP_RETURN_ON_ERROR(lcd_write_nibble(handle, value & 0x0F, rs), TAG, "write low nibble failed");
    return ESP_OK;
}

static esp_err_t lcd_command(lcd2004_i2c_handle_t handle, uint8_t cmd)
{
    ESP_RETURN_ON_ERROR(lcd_write_byte(handle, cmd, false), TAG, "write command failed");
    if (cmd == LCD_CMD_CLEAR || cmd == LCD_CMD_HOME) {
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    return ESP_OK;
}

esp_err_t lcd2004_i2c_create(i2c_master_bus_handle_t bus_handle, uint8_t address, lcd2004_i2c_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(bus_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "bus_handle is NULL");
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    lcd2004_i2c_handle_t dev = calloc(1, sizeof(struct lcd2004_i2c_dev_t));
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_NO_MEM, TAG, "no memory for LCD device");

    esp_err_t ret = pcf8574_create(bus_handle, address, &dev->io);
    if (ret != ESP_OK) {
        free(dev);
        return ret;
    }

    dev->backlight = LCD_BIT_BL;
    *handle = dev;
    return ESP_OK;
}

esp_err_t lcd2004_i2c_delete(lcd2004_i2c_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    esp_err_t ret = pcf8574_delete(handle->io);
    free(handle);
    return ret;
}

esp_err_t lcd2004_i2c_init(lcd2004_i2c_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    ESP_RETURN_ON_ERROR(pcf8574_set_direction_mask(handle->io, 0x00), TAG, "set LCD backpack outputs failed");
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_RETURN_ON_ERROR(lcd_write_nibble(handle, 0x03, false), TAG, "init 8-bit step 1 failed");
    vTaskDelay(pdMS_TO_TICKS(5));
    ESP_RETURN_ON_ERROR(lcd_write_nibble(handle, 0x03, false), TAG, "init 8-bit step 2 failed");
    esp_rom_delay_us(150);
    ESP_RETURN_ON_ERROR(lcd_write_nibble(handle, 0x03, false), TAG, "init 8-bit step 3 failed");
    esp_rom_delay_us(150);
    ESP_RETURN_ON_ERROR(lcd_write_nibble(handle, 0x02, false), TAG, "init 4-bit step failed");

    ESP_RETURN_ON_ERROR(lcd_command(handle, LCD_CMD_FUNCTION_SET), TAG, "function set failed");
    ESP_RETURN_ON_ERROR(lcd_command(handle, LCD_CMD_DISPLAY_ON), TAG, "display on failed");
    ESP_RETURN_ON_ERROR(lcd_command(handle, LCD_CMD_CLEAR), TAG, "clear failed");
    ESP_RETURN_ON_ERROR(lcd_command(handle, LCD_CMD_ENTRY_MODE), TAG, "entry mode failed");
    return ESP_OK;
}

esp_err_t lcd2004_i2c_clear(lcd2004_i2c_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    return lcd_command(handle, LCD_CMD_CLEAR);
}

esp_err_t lcd2004_i2c_home(lcd2004_i2c_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    return lcd_command(handle, LCD_CMD_HOME);
}

esp_err_t lcd2004_i2c_set_cursor(lcd2004_i2c_handle_t handle, uint8_t row, uint8_t col)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(row < LCD_ROWS, ESP_ERR_INVALID_ARG, TAG, "invalid row");
    ESP_RETURN_ON_FALSE(col < LCD_COLS, ESP_ERR_INVALID_ARG, TAG, "invalid col");

    static const uint8_t row_offsets[LCD_ROWS] = {0x00, 0x40, 0x14, 0x54};
    return lcd_command(handle, (uint8_t)(0x80 | (row_offsets[row] + col)));
}

esp_err_t lcd2004_i2c_write_char(lcd2004_i2c_handle_t handle, char c)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    return lcd_write_byte(handle, (uint8_t)c, true);
}

esp_err_t lcd2004_i2c_write_str(lcd2004_i2c_handle_t handle, const char *text)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(text != NULL, ESP_ERR_INVALID_ARG, TAG, "text is NULL");

    for (const char *p = text; *p != '\0'; ++p) {
        ESP_RETURN_ON_ERROR(lcd2004_i2c_write_char(handle, *p), TAG, "write char failed");
    }
    return ESP_OK;
}

esp_err_t lcd2004_i2c_print_line(lcd2004_i2c_handle_t handle, uint8_t row, const char *text)
{
    ESP_RETURN_ON_FALSE(text != NULL, ESP_ERR_INVALID_ARG, TAG, "text is NULL");
    ESP_RETURN_ON_ERROR(lcd2004_i2c_set_cursor(handle, row, 0), TAG, "set line cursor failed");

    char line[LCD_COLS + 1];
    memset(line, ' ', LCD_COLS);
    line[LCD_COLS] = '\0';
    size_t len = strlen(text);
    if (len > LCD_COLS) {
        len = LCD_COLS;
    }
    memcpy(line, text, len);
    return lcd2004_i2c_write_str(handle, line);
}

esp_err_t lcd2004_i2c_backlight(lcd2004_i2c_handle_t handle, bool on)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    handle->backlight = on ? LCD_BIT_BL : 0;
    return lcd_write_raw(handle, 0);
}

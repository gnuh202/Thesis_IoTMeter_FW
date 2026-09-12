#include "hmi_test_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hmi_bsp.h"
#include "home_screen.h"
#include "lcd_menu.h"
#include "sdkconfig.h"
#include "wifi_manager.h"

#define HMI_MENU_POLL_MS 20
#define HMI_MENU_LCD_REFRESH_MS 500
#define HMI_MENU_LCD_WIDTH 20
#define HMI_MENU_LCD_HEIGHT 4

/* Developer-only menu, reached by holding LEFT + RIGHT at boot (engineering
 * mode). It replaces the old HMI bring-up test menu (LED / button / LCD /
 * ext-meter checks), which is no longer needed now that those peripherals are
 * covered by the production home screen and console. What remains is the
 * functionality that must stay out of the end-user menu: calibration, plus AP
 * mode for field provisioning. */
static const char *TAG = "hmi_menu";
static bool s_started;

typedef struct {
    lcd_menu_t menu;
    uint32_t ap_mode_start_count;
} hmi_menu_app_t;

static esp_err_t menu_write_line(void *user_ctx, uint8_t row, const char *text)
{
    (void)user_ctx;
    return hmi_bsp_lcd_print_line(row, text);
}

/* Start SoftAP for field provisioning. Kept as an ACTION with an explicit
 * status screen: unlike the removed test items this changes device state, so a
 * one-shot confirmation of the result is worth the screen. */
static esp_err_t start_ap_mode(lcd_menu_t *menu, const lcd_menu_item_t *item, void *user_ctx)
{
    (void)menu;
    (void)item;
    hmi_menu_app_t *app = (hmi_menu_app_t *)user_ctx;
    app->ap_mode_start_count++;

    esp_err_t ret = wifi_manager_start_ap();

    char line[HMI_MENU_LCD_WIDTH + 1];
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(0, "== WIFI AP MODE =="), TAG, "write LCD failed");
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(1, ret == ESP_OK ? "AP start requested" : "AP start failed"), TAG, "write LCD failed");
    snprintf(line, sizeof(line), "Request count: %lu", (unsigned long)app->ap_mode_start_count);
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(2, line), TAG, "write LCD failed");
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(3, wifi_manager_ap_is_active() ? "AP is active" : "Check WiFi logs"), TAG, "write LCD failed");

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AP mode start failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "AP mode requested, count=%lu active=%d",
                 (unsigned long)app->ap_mode_start_count, wifi_manager_ap_is_active());
    }
    vTaskDelay(pdMS_TO_TICKS(1200));
    return ESP_OK;
}

static const lcd_menu_item_t s_root_items[] = {
    {.label = "Calibration", .type = LCD_MENU_ITEM_SUBMENU, .submenu = &home_screen_calibration_screen},
    {.label = "Start AP Mode", .type = LCD_MENU_ITEM_ACTION, .action = start_ap_mode},
};

static const lcd_menu_screen_t s_root_screen = {
    .title = "DEVELOPER",
    .items = s_root_items,
    .item_count = sizeof(s_root_items) / sizeof(s_root_items[0]),
};

static lcd_menu_key_t button_to_key(uint8_t button)
{
    if (button & HMI_BSP_BUTTON_TOP) {
        return LCD_MENU_KEY_UP;
    }
    if (button & HMI_BSP_BUTTON_BOTTOM) {
        return LCD_MENU_KEY_DOWN;
    }
    if (button & HMI_BSP_BUTTON_LEFT) {
        return LCD_MENU_KEY_LEFT;
    }
    if (button & HMI_BSP_BUTTON_RIGHT) {
        return LCD_MENU_KEY_RIGHT;
    }
    if (button & HMI_BSP_BUTTON_CENTER) {
        return LCD_MENU_KEY_OK;
    }
    return LCD_MENU_KEY_NONE;
}

static void hmi_menu_task(void *arg)
{
    (void)arg;

    hmi_menu_app_t app = {0};
    esp_err_t ret = hmi_bsp_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HMI init failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    /* The calibration callbacks live in home_screen.c and gate their button
     * feedback on the buzzer preference loaded by home_screen_load_buzzer_pref().
     * home_screen_task does not run in engineering mode, so load it here —
     * otherwise those clicks are silently disabled. */
    home_screen_load_buzzer_pref();

    hmi_bsp_buzzer_set(false);
    /* Clear whatever LED pattern the boot screen left behind. From here the LEDs
     * follow the production status pattern driven by update_leds() inside the
     * calibration modal helpers. */
    hmi_bsp_set_leds(0);

    lcd_menu_config_t menu_config = {
        .width = HMI_MENU_LCD_WIDTH,
        .height = HMI_MENU_LCD_HEIGHT,
        .pointer_char = '>',
        .wrap_cursor = false,
        .show_scroll_markers = true,
        /* Nothing in this menu toggles a BOOL item, so no edit is ever left
         * unsaved and the save-on-exit prompt would be unreachable. Each action
         * persists its own change (calibration writes to SD / NVS directly). */
        .ask_save_on_exit = false,
        .show_position_counter = true,
        .user_ctx = &app,
        .write_line = menu_write_line,
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(lcd_menu_init(&app.menu, &menu_config, &s_root_screen));
    ESP_ERROR_CHECK_WITHOUT_ABORT(lcd_menu_render(&app.menu));

    uint8_t last_buttons = 0;
    uint32_t refresh_elapsed_ms = HMI_MENU_LCD_REFRESH_MS;

    ESP_LOGI(TAG, "Developer menu started: UP/DOWN=move, CENTER=OK/enter, LEFT=Back, RIGHT=reserved");

    while (1) {
        uint8_t buttons = 0;
        ret = hmi_bsp_read_buttons(&buttons);
        if (ret == ESP_OK) {
            uint8_t press_edges = buttons & ~last_buttons;
            last_buttons = buttons;

            if (press_edges != 0) {
                lcd_menu_key_t key = button_to_key(press_edges);
                if (key != LCD_MENU_KEY_NONE) {
                    /* Same feedback path as production: gated by the user's
                     * buzzer preference, 50 ms click. */
                    home_screen_button_click();
                    ESP_ERROR_CHECK_WITHOUT_ABORT(lcd_menu_handle_key(&app.menu, key));
                    ESP_ERROR_CHECK_WITHOUT_ABORT(lcd_menu_render(&app.menu));
                    refresh_elapsed_ms = 0;
                }
            }
        } else {
            ESP_LOGW(TAG, "read buttons failed: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(HMI_MENU_POLL_MS));
        refresh_elapsed_ms += HMI_MENU_POLL_MS;
        if (refresh_elapsed_ms >= HMI_MENU_LCD_REFRESH_MS) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(lcd_menu_render(&app.menu));
            refresh_elapsed_ms = 0;
        }
    }
}

esp_err_t hmi_test_task_start(void)
{
#if CONFIG_APP_HMI_TEST_ENABLE
    if (s_started) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(hmi_menu_task,
                                "hmi_menu_task",
                                CONFIG_APP_HMI_TASK_STACK_SIZE,
                                NULL,
                                CONFIG_APP_HMI_TASK_PRIORITY,
                                NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "create HMI menu task failed");
    s_started = true;
#endif
    return ESP_OK;
}

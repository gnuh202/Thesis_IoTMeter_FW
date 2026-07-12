#include "hmi_test_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hmi_bsp.h"
#include "lcd_menu.h"
#include "sdkconfig.h"
#include "wifi_manager.h"

#define HMI_MENU_POLL_MS 20
#define HMI_MENU_LCD_REFRESH_MS 500
#define HMI_MENU_LCD_WIDTH 20
#define HMI_MENU_LCD_HEIGHT 4
#define HMI_MENU_LED_COUNT HMI_BSP_LED_COUNT

static const char *TAG = "hmi_menu";
static bool s_started;

typedef struct {
    lcd_menu_t menu;
    bool led_on[HMI_MENU_LED_COUNT];
    uint8_t saved_led_mask;
    uint8_t current_buttons;
    uint8_t last_pressed;
    uint32_t press_count;
    uint32_t ap_mode_start_count;
} hmi_menu_app_t;

static esp_err_t menu_write_line(void *user_ctx, uint8_t row, const char *text)
{
    (void)user_ctx;
    return hmi_bsp_lcd_print_line(row, text);
}

static uint8_t led_bool_to_mask(const hmi_menu_app_t *app)
{
    uint8_t mask = 0;
    for (uint8_t i = 0; i < HMI_MENU_LED_COUNT; i++) {
        if (app->led_on[i]) {
            mask |= (1U << i);
        }
    }
    return mask;
}

static void led_mask_to_bool(hmi_menu_app_t *app, uint8_t mask)
{
    for (uint8_t i = 0; i < HMI_MENU_LED_COUNT; i++) {
        app->led_on[i] = (mask & (1U << i)) != 0;
    }
}

static esp_err_t apply_leds(hmi_menu_app_t *app)
{
    return hmi_bsp_set_leds(led_bool_to_mask(app));
}

static esp_err_t on_bool_changed(lcd_menu_t *menu, const lcd_menu_item_t *item, bool value, void *user_ctx)
{
    (void)menu;
    hmi_menu_app_t *app = (hmi_menu_app_t *)user_ctx;
    ESP_LOGI(TAG, "%s = %s", item->label, value ? "On" : "Off");
    return apply_leds(app);
}

static esp_err_t on_save(lcd_menu_t *menu, void *user_ctx)
{
    hmi_menu_app_t *app = (hmi_menu_app_t *)user_ctx;
    app->saved_led_mask = led_bool_to_mask(app);
    lcd_menu_set_dirty(menu, false);
    ESP_LOGI(TAG, "HMI settings saved, LED mask=0x%02X", app->saved_led_mask);
    return ESP_OK;
}

static esp_err_t on_discard(lcd_menu_t *menu, void *user_ctx)
{
    hmi_menu_app_t *app = (hmi_menu_app_t *)user_ctx;
    led_mask_to_bool(app, app->saved_led_mask);
    ESP_RETURN_ON_ERROR(apply_leds(app), TAG, "restore LED state failed");
    lcd_menu_set_dirty(menu, false);
    ESP_LOGI(TAG, "HMI settings discarded, LED mask=0x%02X", app->saved_led_mask);
    return ESP_OK;
}

static const char *button_name(uint8_t mask)
{
    if (mask & HMI_BSP_BUTTON_RIGHT) {
        return "RIGHT";
    }
    if (mask & HMI_BSP_BUTTON_BOTTOM) {
        return "DOWN";
    }
    if (mask & HMI_BSP_BUTTON_CENTER) {
        return "CENTER";
    }
    if (mask & HMI_BSP_BUTTON_LEFT) {
        return "LEFT";
    }
    if (mask & HMI_BSP_BUTTON_TOP) {
        return "UP";
    }
    return "NONE";
}

static esp_err_t show_button_test(lcd_menu_t *menu, const lcd_menu_item_t *item, void *user_ctx)
{
    (void)menu;
    (void)item;
    hmi_menu_app_t *app = (hmi_menu_app_t *)user_ctx;
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(0, "== BUTTON TEST ===="), TAG, "write LCD failed");

    char line[21];
    snprintf(line, sizeof(line), "Now : %-12s", button_name(app->current_buttons));
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(1, line), TAG, "write LCD failed");
    snprintf(line, sizeof(line), "Last: %-12s", button_name(app->last_pressed));
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(2, line), TAG, "write LCD failed");
    snprintf(line, sizeof(line), "Mask:%02X Count:%lu", app->current_buttons, (unsigned long)app->press_count);
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(3, line), TAG, "write LCD failed");
    ESP_LOGI(TAG, "Button test: now=%s last=%s count=%lu", button_name(app->current_buttons), button_name(app->last_pressed), (unsigned long)app->press_count);
    vTaskDelay(pdMS_TO_TICKS(1200));
    return ESP_OK;
}

static esp_err_t show_lcd_info(lcd_menu_t *menu, const lcd_menu_item_t *item, void *user_ctx)
{
    (void)menu;
    (void)item;
    hmi_menu_app_t *app = (hmi_menu_app_t *)user_ctx;

    char line[21];
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(0, "== LCD INFO ======="), TAG, "write LCD failed");
    snprintf(line, sizeof(line), "%u cols x %u rows", HMI_MENU_LCD_WIDTH, HMI_MENU_LCD_HEIGHT);
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(1, line), TAG, "write LCD failed");
    snprintf(line, sizeof(line), "LED mask: 0x%02X", led_bool_to_mask(app));
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(2, line), TAG, "write LCD failed");
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(3, "Auto back soon..."), TAG, "write LCD failed");
    vTaskDelay(pdMS_TO_TICKS(1200));
    return ESP_OK;
}

static esp_err_t start_ap_mode(lcd_menu_t *menu, const lcd_menu_item_t *item, void *user_ctx)
{
    (void)menu;
    (void)item;
    hmi_menu_app_t *app = (hmi_menu_app_t *)user_ctx;
    app->ap_mode_start_count++;

    esp_err_t ret = wifi_manager_start_ap();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AP mode start failed: %s", esp_err_to_name(ret));
    }

    char line[21];
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(0, "== WIFI AP MODE =="), TAG, "write LCD failed");
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(1, ret == ESP_OK ? "AP start requested" : "AP start failed"), TAG, "write LCD failed");
    snprintf(line, sizeof(line), "Request count: %lu", (unsigned long)app->ap_mode_start_count);
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(2, line), TAG, "write LCD failed");
    ESP_RETURN_ON_ERROR(hmi_bsp_lcd_print_line(3, wifi_manager_ap_is_active() ? "AP is active" : "Check WiFi logs"), TAG, "write LCD failed");
    ESP_LOGI(TAG, "AP mode action requested, count=%lu, ret=%s, active=%d", (unsigned long)app->ap_mode_start_count, esp_err_to_name(ret), wifi_manager_ap_is_active());
    vTaskDelay(pdMS_TO_TICKS(1200));
    return ESP_OK;
}

static lcd_menu_screen_t s_led_screen;
static lcd_menu_screen_t s_root_screen;

static lcd_menu_item_t s_led_items[] = {
    {.label = "LED 1", .type = LCD_MENU_ITEM_BOOL},
    {.label = "LED 2", .type = LCD_MENU_ITEM_BOOL},
    {.label = "LED 3", .type = LCD_MENU_ITEM_BOOL},
    {.label = "LED 4", .type = LCD_MENU_ITEM_BOOL},
    {.label = "LED 5", .type = LCD_MENU_ITEM_BOOL},
    {.label = "Save", .type = LCD_MENU_ITEM_SAVE},
};

static const lcd_menu_item_t s_root_items[] = {
    {.label = "LED Control", .type = LCD_MENU_ITEM_SUBMENU, .submenu = &s_led_screen},
    {.label = "Start AP Mode", .type = LCD_MENU_ITEM_ACTION, .action = start_ap_mode},
    {.label = "Button Test", .type = LCD_MENU_ITEM_ACTION, .action = show_button_test},
    {.label = "LCD Info", .type = LCD_MENU_ITEM_ACTION, .action = show_lcd_info},
};

static lcd_menu_screen_t s_led_screen = {
    .title = "LED CONTROL",
    .items = s_led_items,
    .item_count = sizeof(s_led_items) / sizeof(s_led_items[0]),
};

static lcd_menu_screen_t s_root_screen = {
    .title = "HMI MAIN MENU",
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

static void bind_led_items(hmi_menu_app_t *app)
{
    for (uint8_t i = 0; i < HMI_MENU_LED_COUNT; i++) {
        s_led_items[i].bool_value = &app->led_on[i];
    }
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

    bind_led_items(&app);
    app.saved_led_mask = 0;
    led_mask_to_bool(&app, app.saved_led_mask);

    hmi_bsp_buzzer_set(false);
    ESP_ERROR_CHECK_WITHOUT_ABORT(apply_leds(&app));

    lcd_menu_config_t menu_config = {
        .width = HMI_MENU_LCD_WIDTH,
        .height = HMI_MENU_LCD_HEIGHT,
        .pointer_char = '>',
        .wrap_cursor = false,
        .show_scroll_markers = true,
        .ask_save_on_exit = true,
        .save_prompt_title = "Save changes?",
        .user_ctx = &app,
        .write_line = menu_write_line,
        .bool_changed = on_bool_changed,
        .save = on_save,
        .discard = on_discard,
    };

    ESP_ERROR_CHECK_WITHOUT_ABORT(lcd_menu_init(&app.menu, &menu_config, &s_root_screen));
    ESP_ERROR_CHECK_WITHOUT_ABORT(lcd_menu_render(&app.menu));

    uint8_t last_buttons = 0;
    uint32_t refresh_elapsed_ms = HMI_MENU_LCD_REFRESH_MS;

    ESP_LOGI(TAG, "Reusable LCD menu started: UP/DOWN=move, RIGHT=enter submenu, CENTER=OK/activate, LEFT=Back");

    while (1) {
        ret = hmi_bsp_read_buttons(&app.current_buttons);
        if (ret == ESP_OK) {
            uint8_t press_edges = app.current_buttons & ~last_buttons;
            if (press_edges != 0) {
                app.last_pressed = press_edges;
                app.press_count++;

                lcd_menu_key_t key = button_to_key(press_edges);
                if (key != LCD_MENU_KEY_NONE) {
                    ESP_LOGI(TAG, "Button: %s key=%d", button_name(press_edges), key);
                    ESP_ERROR_CHECK_WITHOUT_ABORT(lcd_menu_handle_key(&app.menu, key));
                    ESP_ERROR_CHECK_WITHOUT_ABORT(lcd_menu_render(&app.menu));
                    refresh_elapsed_ms = 0;
                }
            }
            last_buttons = app.current_buttons;
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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LCD_MENU_MAX_DEPTH
#define LCD_MENU_MAX_DEPTH 8
#endif

#ifndef LCD_MENU_MAX_WIDTH
#define LCD_MENU_MAX_WIDTH 32
#endif

typedef struct lcd_menu_t lcd_menu_t;
typedef struct lcd_menu_item_t lcd_menu_item_t;
typedef struct lcd_menu_screen_t lcd_menu_screen_t;

typedef enum {
    LCD_MENU_KEY_NONE = 0,
    LCD_MENU_KEY_UP,     /* move selection up / scroll up */
    LCD_MENU_KEY_DOWN,   /* move selection down / scroll down */
    LCD_MENU_KEY_LEFT,   /* Back: leave submenu, or trigger save prompt at root */
    LCD_MENU_KEY_RIGHT,  /* Drill-in only: enter a submenu; ignored on other items */
    LCD_MENU_KEY_OK,     /* Activate: enter submenu, toggle bool, run action, confirm save */
    LCD_MENU_KEY_BACK = LCD_MENU_KEY_LEFT,
} lcd_menu_key_t;

typedef enum {
    LCD_MENU_ITEM_SUBMENU = 0,
    LCD_MENU_ITEM_ACTION,
    LCD_MENU_ITEM_BOOL,
    LCD_MENU_ITEM_BACK,
    LCD_MENU_ITEM_SAVE,
} lcd_menu_item_type_t;

typedef esp_err_t (*lcd_menu_write_line_cb_t)(void *user_ctx, uint8_t row, const char *text);
typedef esp_err_t (*lcd_menu_action_cb_t)(lcd_menu_t *menu, const lcd_menu_item_t *item, void *user_ctx);
typedef bool (*lcd_menu_bool_get_cb_t)(lcd_menu_t *menu, const lcd_menu_item_t *item, void *user_ctx);
typedef esp_err_t (*lcd_menu_bool_set_cb_t)(lcd_menu_t *menu, const lcd_menu_item_t *item, bool value, void *user_ctx);
typedef esp_err_t (*lcd_menu_bool_changed_cb_t)(lcd_menu_t *menu, const lcd_menu_item_t *item, bool value, void *user_ctx);
typedef esp_err_t (*lcd_menu_save_cb_t)(lcd_menu_t *menu, void *user_ctx);
typedef esp_err_t (*lcd_menu_discard_cb_t)(lcd_menu_t *menu, void *user_ctx);

struct lcd_menu_item_t {
    const char *label;
    lcd_menu_item_type_t type;
    const lcd_menu_screen_t *submenu;
    bool *bool_value;
    lcd_menu_bool_get_cb_t bool_get;
    lcd_menu_bool_set_cb_t bool_set;
    lcd_menu_action_cb_t action;
    void *user_data;
};

struct lcd_menu_screen_t {
    const char *title;
    const lcd_menu_item_t *items;
    uint8_t item_count;
};

typedef struct {
    uint8_t width;
    uint8_t height;
    char pointer_char;
    const char *back_item_text;
    const char *save_prompt_title;
    bool wrap_cursor;
    bool show_scroll_markers;
    bool ask_save_on_exit;
    void *user_ctx;
    lcd_menu_write_line_cb_t write_line;
    lcd_menu_bool_changed_cb_t bool_changed;
    lcd_menu_save_cb_t save;
    lcd_menu_discard_cb_t discard;
} lcd_menu_config_t;

esp_err_t lcd_menu_init(lcd_menu_t *menu, const lcd_menu_config_t *config, const lcd_menu_screen_t *root);
esp_err_t lcd_menu_render(lcd_menu_t *menu);
esp_err_t lcd_menu_handle_key(lcd_menu_t *menu, lcd_menu_key_t key);
void lcd_menu_set_dirty(lcd_menu_t *menu, bool dirty);
bool lcd_menu_is_dirty(const lcd_menu_t *menu);
void *lcd_menu_get_user_ctx(const lcd_menu_t *menu);
const lcd_menu_screen_t *lcd_menu_get_current_screen(const lcd_menu_t *menu);
uint8_t lcd_menu_get_selected_index(const lcd_menu_t *menu);
esp_err_t lcd_menu_back(lcd_menu_t *menu);

struct lcd_menu_t {
    lcd_menu_config_t config;
    const lcd_menu_screen_t *root;
    const lcd_menu_screen_t *stack[LCD_MENU_MAX_DEPTH];
    uint8_t selected[LCD_MENU_MAX_DEPTH];
    uint8_t top[LCD_MENU_MAX_DEPTH];
    uint8_t depth;
    bool dirty;
    bool in_save_prompt;
    uint8_t save_selected;
};

#ifdef __cplusplus
}
#endif

#include "lcd_menu.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"

static const char *TAG = "lcd_menu";

static uint8_t menu_width(const lcd_menu_t *menu)
{
    return menu->config.width == 0 ? 20 : menu->config.width;
}

static uint8_t menu_height(const lcd_menu_t *menu)
{
    return menu->config.height < 2 ? 2 : menu->config.height;
}

static char menu_pointer(const lcd_menu_t *menu)
{
    return menu->config.pointer_char == '\0' ? '>' : menu->config.pointer_char;
}

static const char *save_prompt_title(const lcd_menu_t *menu)
{
    return menu->config.save_prompt_title != NULL ? menu->config.save_prompt_title : "Save changes?";
}

static void make_blank_line(const lcd_menu_t *menu, char *line, size_t line_size)
{
    uint8_t width = menu_width(menu);
    if (line_size == 0) {
        return;
    }
    if (width >= line_size) {
        width = line_size - 1;
    }
    memset(line, ' ', width);
    line[width] = '\0';
}

static esp_err_t write_padded_line(lcd_menu_t *menu, uint8_t row, const char *text)
{
    ESP_RETURN_ON_FALSE(menu->config.write_line != NULL, ESP_ERR_INVALID_STATE, TAG, "write_line callback is NULL");

    char line[LCD_MENU_MAX_WIDTH + 1];
    make_blank_line(menu, line, sizeof(line));

    uint8_t width = menu_width(menu);
    size_t copy_len = text != NULL ? strlen(text) : 0;
    if (copy_len > width) {
        copy_len = width;
    }
    memcpy(line, text != NULL ? text : "", copy_len);
    return menu->config.write_line(menu->config.user_ctx, row, line);
}

static esp_err_t write_centered_line(lcd_menu_t *menu, uint8_t row, const char *text)
{
    char line[LCD_MENU_MAX_WIDTH + 1];
    make_blank_line(menu, line, sizeof(line));

    uint8_t width = menu_width(menu);
    size_t text_len = text != NULL ? strlen(text) : 0;
    if (text_len > width) {
        text_len = width;
    }

    uint8_t pad = (width > text_len) ? (uint8_t)((width - text_len) / 2) : 0;
    memcpy(&line[pad], text != NULL ? text : "", text_len);
    return menu->config.write_line(menu->config.user_ctx, row, line);
}

static const lcd_menu_screen_t *current_screen(const lcd_menu_t *menu)
{
    if (menu->depth >= LCD_MENU_MAX_DEPTH) {
        return NULL;
    }
    return menu->stack[menu->depth];
}

static uint8_t current_selected(const lcd_menu_t *menu)
{
    return menu->selected[menu->depth];
}

static uint8_t current_top(const lcd_menu_t *menu)
{
    return menu->top[menu->depth];
}

static uint8_t visible_rows(const lcd_menu_t *menu)
{
    return menu_height(menu) - 1;
}

static void normalize_cursor(lcd_menu_t *menu)
{
    const lcd_menu_screen_t *screen = current_screen(menu);
    if (screen == NULL || screen->item_count == 0) {
        menu->selected[menu->depth] = 0;
        menu->top[menu->depth] = 0;
        return;
    }

    if (menu->selected[menu->depth] >= screen->item_count) {
        menu->selected[menu->depth] = screen->item_count - 1;
    }

    uint8_t rows = visible_rows(menu);
    if (menu->top[menu->depth] > menu->selected[menu->depth]) {
        menu->top[menu->depth] = menu->selected[menu->depth];
    }
    if (menu->selected[menu->depth] >= menu->top[menu->depth] + rows) {
        menu->top[menu->depth] = menu->selected[menu->depth] - rows + 1;
    }
}

static void move_up(lcd_menu_t *menu, uint8_t item_count)
{
    if (item_count == 0) {
        return;
    }

    uint8_t *selected = &menu->selected[menu->depth];
    if (*selected > 0) {
        (*selected)--;
    } else if (menu->config.wrap_cursor) {
        *selected = item_count - 1;
    }
    normalize_cursor(menu);
}

static void move_down(lcd_menu_t *menu, uint8_t item_count)
{
    if (item_count == 0) {
        return;
    }

    uint8_t *selected = &menu->selected[menu->depth];
    if (*selected + 1 < item_count) {
        (*selected)++;
    } else if (menu->config.wrap_cursor) {
        *selected = 0;
    }
    normalize_cursor(menu);
}

static bool get_bool_value(lcd_menu_t *menu, const lcd_menu_item_t *item)
{
    if (item == NULL) {
        return false;
    }
    if (item->bool_get != NULL) {
        return item->bool_get(menu, item, menu->config.user_ctx);
    }
    return item->bool_value != NULL ? *item->bool_value : false;
}

static esp_err_t set_bool_value(lcd_menu_t *menu, const lcd_menu_item_t *item, bool value)
{
    ESP_RETURN_ON_FALSE(item != NULL, ESP_ERR_INVALID_ARG, TAG, "bool item is NULL");
    if (item->bool_set != NULL) {
        return item->bool_set(menu, item, value, menu->config.user_ctx);
    }
    ESP_RETURN_ON_FALSE(item->bool_value != NULL, ESP_ERR_INVALID_ARG, TAG, "bool setter/value is NULL");
    *item->bool_value = value;
    return ESP_OK;
}

static esp_err_t render_item_line(lcd_menu_t *menu, uint8_t row, const lcd_menu_item_t *item, bool selected, bool has_above, bool has_below)
{
    char line[LCD_MENU_MAX_WIDTH + 1];
    make_blank_line(menu, line, sizeof(line));

    uint8_t width = menu_width(menu);
    line[0] = selected ? menu_pointer(menu) : ' ';
    if (width > 1) {
        line[1] = ' ';
    }

    char label[LCD_MENU_MAX_WIDTH + 1];
    if (item == NULL) {
        label[0] = '\0';
    } else if (item->type == LCD_MENU_ITEM_BOOL) {
        snprintf(label, sizeof(label), "%s: %s", item->label != NULL ? item->label : "", get_bool_value(menu, item) ? "On" : "Off");
    } else {
        snprintf(label, sizeof(label), "%s", item->label != NULL ? item->label : "");
    }

    size_t copy_len = strlen(label);
    uint8_t label_start = width > 2 ? 2 : width;
    if (copy_len > width - label_start) {
        copy_len = width - label_start;
    }
    if (label_start < width) {
        memcpy(&line[label_start], label, copy_len);
    }

    if (menu->config.show_scroll_markers && width > 0) {
        if (has_above && row == 1) {
            line[width - 1] = '|';
        } else if (has_below && row == menu_height(menu) - 1) {
            line[width - 1] = '|';
        }
    }

    return menu->config.write_line(menu->config.user_ctx, row, line);
}

static esp_err_t render_save_prompt(lcd_menu_t *menu)
{
    ESP_RETURN_ON_ERROR(write_centered_line(menu, 0, save_prompt_title(menu)), TAG, "write save title failed");
    ESP_RETURN_ON_ERROR(render_item_line(menu, 1, &(lcd_menu_item_t){.label = "Yes"}, menu->save_selected == 0, false, false), TAG, "write save yes failed");
    if (menu_height(menu) > 2) {
        ESP_RETURN_ON_ERROR(render_item_line(menu, 2, &(lcd_menu_item_t){.label = "No"}, menu->save_selected == 1, false, false), TAG, "write save no failed");
    }
    for (uint8_t row = 3; row < menu_height(menu); row++) {
        ESP_RETURN_ON_ERROR(write_padded_line(menu, row, row == menu_height(menu) - 1 ? "LEFT: Cancel" : ""), TAG, "clear save row failed");
    }
    return ESP_OK;
}

esp_err_t lcd_menu_init(lcd_menu_t *menu, const lcd_menu_config_t *config, const lcd_menu_screen_t *root)
{
    ESP_RETURN_ON_FALSE(menu != NULL, ESP_ERR_INVALID_ARG, TAG, "menu is NULL");
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(root != NULL, ESP_ERR_INVALID_ARG, TAG, "root is NULL");
    ESP_RETURN_ON_FALSE(config->write_line != NULL, ESP_ERR_INVALID_ARG, TAG, "write_line callback is NULL");
    ESP_RETURN_ON_FALSE(config->width <= LCD_MENU_MAX_WIDTH, ESP_ERR_INVALID_ARG, TAG, "width too large");

    memset(menu, 0, sizeof(*menu));
    menu->config = *config;
    menu->root = root;
    menu->stack[0] = root;
    menu->depth = 0;
    return ESP_OK;
}

esp_err_t lcd_menu_render(lcd_menu_t *menu)
{
    ESP_RETURN_ON_FALSE(menu != NULL, ESP_ERR_INVALID_ARG, TAG, "menu is NULL");

    if (menu->in_save_prompt) {
        return render_save_prompt(menu);
    }

    const lcd_menu_screen_t *screen = current_screen(menu);
    ESP_RETURN_ON_FALSE(screen != NULL, ESP_ERR_INVALID_STATE, TAG, "screen is NULL");

    normalize_cursor(menu);
    ESP_RETURN_ON_ERROR(write_centered_line(menu, 0, screen->title != NULL ? screen->title : "Menu"), TAG, "write title failed");

    uint8_t rows = visible_rows(menu);
    uint8_t top = current_top(menu);
    uint8_t selected = current_selected(menu);
    for (uint8_t row = 0; row < rows; row++) {
        uint8_t item_index = top + row;
        if (item_index < screen->item_count) {
            bool has_above = top > 0;
            bool has_below = (top + rows) < screen->item_count;
            ESP_RETURN_ON_ERROR(render_item_line(menu, row + 1, &screen->items[item_index], item_index == selected, has_above, has_below), TAG, "write item failed");
        } else {
            ESP_RETURN_ON_ERROR(write_padded_line(menu, row + 1, ""), TAG, "clear item row failed");
        }
    }

    return ESP_OK;
}

static esp_err_t push_screen(lcd_menu_t *menu, const lcd_menu_screen_t *screen)
{
    ESP_RETURN_ON_FALSE(screen != NULL, ESP_ERR_INVALID_ARG, TAG, "submenu is NULL");
    ESP_RETURN_ON_FALSE(menu->depth + 1 < LCD_MENU_MAX_DEPTH, ESP_ERR_NO_MEM, TAG, "menu stack full");

    menu->depth++;
    menu->stack[menu->depth] = screen;
    menu->selected[menu->depth] = 0;
    menu->top[menu->depth] = 0;
    return ESP_OK;
}

static esp_err_t request_exit_or_back(lcd_menu_t *menu)
{
    if (menu->depth > 0) {
        menu->depth--;
        return ESP_OK;
    }

    if (menu->dirty && menu->config.ask_save_on_exit) {
        menu->in_save_prompt = true;
        menu->save_selected = 0;
    }
    return ESP_OK;
}

esp_err_t lcd_menu_back(lcd_menu_t *menu)
{
    ESP_RETURN_ON_FALSE(menu != NULL, ESP_ERR_INVALID_ARG, TAG, "menu is NULL");
    return request_exit_or_back(menu);
}

static esp_err_t handle_save_prompt_key(lcd_menu_t *menu, lcd_menu_key_t key)
{
    if (key == LCD_MENU_KEY_UP || key == LCD_MENU_KEY_DOWN) {
        menu->save_selected = menu->save_selected == 0 ? 1 : 0;
        return ESP_OK;
    }
    if (key == LCD_MENU_KEY_LEFT) {
        menu->in_save_prompt = false;
        return ESP_OK;
    }
    if (key == LCD_MENU_KEY_OK) {
        esp_err_t ret = ESP_OK;
        if (menu->save_selected == 0) {
            if (menu->config.save != NULL) {
                ret = menu->config.save(menu, menu->config.user_ctx);
            }
        } else if (menu->config.discard != NULL) {
            ret = menu->config.discard(menu, menu->config.user_ctx);
        }
        if (ret == ESP_OK) {
            menu->dirty = false;
            menu->in_save_prompt = false;
        }
        return ret;
    }
    return ESP_OK;
}

esp_err_t lcd_menu_handle_key(lcd_menu_t *menu, lcd_menu_key_t key)
{
    ESP_RETURN_ON_FALSE(menu != NULL, ESP_ERR_INVALID_ARG, TAG, "menu is NULL");

    if (menu->in_save_prompt) {
        return handle_save_prompt_key(menu, key);
    }

    const lcd_menu_screen_t *screen = current_screen(menu);
    ESP_RETURN_ON_FALSE(screen != NULL, ESP_ERR_INVALID_STATE, TAG, "screen is NULL");

    switch (key) {
    case LCD_MENU_KEY_UP:
        move_up(menu, screen->item_count);
        break;
    case LCD_MENU_KEY_DOWN:
        move_down(menu, screen->item_count);
        break;
    case LCD_MENU_KEY_LEFT:
        ESP_RETURN_ON_ERROR(request_exit_or_back(menu), TAG, "back failed");
        break;
    case LCD_MENU_KEY_RIGHT: {
        /* RIGHT is drill-in only: enter a submenu if the selected item is one,
         * otherwise do nothing. Toggling/activating is reserved for OK so the
         * two keys never overlap. */
        if (screen->item_count == 0) {
            break;
        }
        normalize_cursor(menu);
        const lcd_menu_item_t *item = &screen->items[current_selected(menu)];
        if (item->type == LCD_MENU_ITEM_SUBMENU) {
            ESP_RETURN_ON_ERROR(push_screen(menu, item->submenu), TAG, "push submenu failed");
        }
        break;
    }
    case LCD_MENU_KEY_OK: {
        if (screen->item_count == 0) {
            break;
        }
        normalize_cursor(menu);
        const lcd_menu_item_t *item = &screen->items[current_selected(menu)];
        switch (item->type) {
        case LCD_MENU_ITEM_SUBMENU:
            ESP_RETURN_ON_ERROR(push_screen(menu, item->submenu), TAG, "push submenu failed");
            break;
        case LCD_MENU_ITEM_ACTION:
            if (item->action != NULL) {
                ESP_RETURN_ON_ERROR(item->action(menu, item, menu->config.user_ctx), TAG, "action failed");
            }
            break;
        case LCD_MENU_ITEM_BOOL: {
            bool new_value = !get_bool_value(menu, item);
            ESP_RETURN_ON_ERROR(set_bool_value(menu, item, new_value), TAG, "set bool failed");
            menu->dirty = true;
            if (menu->config.bool_changed != NULL) {
                ESP_RETURN_ON_ERROR(menu->config.bool_changed(menu, item, new_value, menu->config.user_ctx), TAG, "bool callback failed");
            }
            break;
        }
        case LCD_MENU_ITEM_BACK:
            ESP_RETURN_ON_ERROR(request_exit_or_back(menu), TAG, "back item failed");
            break;
        case LCD_MENU_ITEM_SAVE:
            if (menu->config.save != NULL) {
                ESP_RETURN_ON_ERROR(menu->config.save(menu, menu->config.user_ctx), TAG, "save failed");
            }
            menu->dirty = false;
            break;
        default:
            break;
        }
        break;
    }
    default:
        break;
    }

    return ESP_OK;
}

void lcd_menu_set_dirty(lcd_menu_t *menu, bool dirty)
{
    if (menu != NULL) {
        menu->dirty = dirty;
    }
}

bool lcd_menu_is_dirty(const lcd_menu_t *menu)
{
    return menu != NULL && menu->dirty;
}

void *lcd_menu_get_user_ctx(const lcd_menu_t *menu)
{
    return menu != NULL ? menu->config.user_ctx : NULL;
}

const lcd_menu_screen_t *lcd_menu_get_current_screen(const lcd_menu_t *menu)
{
    return menu != NULL ? current_screen(menu) : NULL;
}

uint8_t lcd_menu_get_selected_index(const lcd_menu_t *menu)
{
    return menu != NULL ? current_selected(menu) : 0;
}

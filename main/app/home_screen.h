#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Calibration sub-menu screen, defined in home_screen.c. Developer-only: it is
 * not linked into the production SETTINGS tree. The engineering-mode menu
 * (hmi_test_task.c) links it as a submenu. Its action callbacks live in
 * home_screen.c and use the same 20x4 LCD + modal helpers.
 */
struct lcd_menu_screen_t;
extern const struct lcd_menu_screen_t home_screen_calibration_screen;

/*
 * Button-feedback buzzer helpers shared with the engineering-mode menu
 * (hmi_test_task.c). The calibration callbacks defined in home_screen.c call
 * button_click() internally, which is gated by the buzzer preference loaded
 * from the config snapshot. In engineering mode home_screen_task never runs, so
 * the menu task must call home_screen_load_buzzer_pref() once at startup (to
 * populate that preference) and home_screen_button_click() for its own button
 * presses, so feedback is identical to production instead of silently disabled.
 */
void home_screen_load_buzzer_pref(void);
void home_screen_button_click(void);

/*
 * Home Screen: the default HMI shown after a normal boot. It owns the LCD once
 * the boot sequence finishes, displaying a home view (device identity + live
 * status) and a module OK/ERROR page fed from boot_manager's status table.
 *
 * This is a new layer added in front of the existing menu (hmi_test_task),
 * which is now reached only in engineering mode. app_tasks_start() creates
 * exactly one of the two: home_screen (normal boot) or hmi_test_task
 * (engineering mode).
 */
esp_err_t home_screen_start(void);

/*
 * Ask the Home Screen task to re-read the LCD settings (backlight, auto-sleep
 * timeout) from the Configuration Manager snapshot and apply them.
 *
 * Asynchronous by design: only the Home Screen task may drive the LCD, so this
 * raises a request that the task picks up on its next poll (20 ms) instead of
 * touching the display from the caller's context. Returns
 * ESP_ERR_INVALID_STATE when the task is not running (engineering mode) — the
 * settings are then applied when it next starts.
 */
esp_err_t home_screen_apply_config(void);

#ifdef __cplusplus
}
#endif

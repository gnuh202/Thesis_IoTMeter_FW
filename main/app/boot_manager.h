#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Boot manager: owns the boot-time UI only (splash logo + "Initializing..."
 * progress on the LCD) and records a per-module OK/ERROR table that the Home
 * Screen can display later. It does NOT own module initialization — each module
 * still self-inits in its own start function. app_tasks_start() calls a module's
 * start function and hands the result to boot_manager_step() to log + display.
 *
 * Only the LCD + PCF8575 (buttons/LEDs) are brought up here, through the
 * idempotent hmi_bsp_init(), so the boot screen and the engineering-mode key
 * combo work. All other peripherals keep their existing init ownership.
 */

#define BOOT_MODULE_MAX 16
#define BOOT_MODULE_NAME_LEN 20

typedef struct {
    char name[BOOT_MODULE_NAME_LEN];
    bool ok;
} boot_module_status_t;

/*
 * Bring up the display (idempotent hmi_bsp_init), show the splash logo for
 * ~1.5 s, then switch to the "Initializing..." screen. LCD failure is non-fatal:
 * it is logged and boot continues headless.
 */
esp_err_t boot_manager_begin(void);

/*
 * Report one module's init result: logs [I][BOOT] <label> or [E][BOOT] <label>
 * Failed, appends a rolling line on the LCD, and records the status for the
 * Home Screen. Never aborts.
 */
void boot_manager_step(const char *label, esp_err_t result);

/* Mark the end of the boot sequence (final "Boot complete" log/line). */
void boot_manager_end(void);

/*
 * True if the engineering-mode key combo (LEFT + RIGHT) is held at boot. Read
 * once, after boot_manager_begin() has brought up the buttons.
 */
bool boot_manager_engineering_mode(void);

/*
 * Access the recorded module status table (for the Home Screen). Returns the
 * number of entries and points *out at the internal array.
 */
uint8_t boot_manager_get_status(const boot_module_status_t **out);

#ifdef __cplusplus
}
#endif

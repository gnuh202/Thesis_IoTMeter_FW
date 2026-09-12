#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Central system status registry.
 *
 * A pure in-RAM table: one state per module. It talks to no hardware and
 * includes no other module (no reverse dependency) — each module reports its
 * own state changes via system_status_set(); consumers (LCD, web, MQTT, logger)
 * read via system_status_get(). No task, no polling: it only stores state.
 */

typedef enum {
    SYS_MODULE_ATM90 = 0,
    SYS_MODULE_RS485_MASTER,
    SYS_MODULE_RS485_SLAVE,
    SYS_MODULE_ETHERNET,
    SYS_MODULE_WIFI,
    SYS_MODULE_MQTT,
    SYS_MODULE_SD_CARD,
    SYS_MODULE_DIGITAL_INPUT,
    SYS_MODULE_DIGITAL_OUTPUT,
    SYS_MODULE_COUNT,
} system_module_t;

typedef enum {
    SYS_STATUS_UNKNOWN = 0,   /* no state reported yet */
    SYS_STATUS_INIT,          /* bringing up */
    SYS_STATUS_READY,         /* up and working */
    SYS_STATUS_WARNING,       /* degraded but usable */
    SYS_STATUS_ERROR,         /* init/operation failed */
    SYS_STATUS_OFFLINE,       /* was up, link/peer lost */
} system_status_state_t;

/* Create the internal mutex and reset all modules to UNKNOWN. Safe to call more
 * than once. Call once early in boot before the first set/get. */
esp_err_t system_status_init(void);

/* Set one module's state (thread-safe). Out-of-range module is ignored. */
esp_err_t system_status_set(system_module_t module, system_status_state_t state);

/* Get one module's state (thread-safe). Returns SYS_STATUS_UNKNOWN for an
 * out-of-range module or before init. */
system_status_state_t system_status_get(system_module_t module);

/* Human-readable names for logs/UI (static strings, never NULL). */
const char *system_status_module_name(system_module_t module);
const char *system_status_state_name(system_status_state_t state);

#if CONFIG_APP_STATUS_DEBUG
/* DEBUG ONLY (temporary, gated by CONFIG_APP_STATUS_DEBUG): print the whole
 * status table. Removed once Feature 04 is confirmed PASS. */
void system_status_dump(void);
#endif

#ifdef __cplusplus
}
#endif

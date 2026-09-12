#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configuration Apply Engine — framework (Feature 11).
 *
 * One uniform place to turn "the configuration snapshot changed" into "the
 * affected modules pick the change up". data_point_write() updates only the
 * Configuration Manager RAM snapshot; nothing acts on it. This module is the
 * missing step between the two:
 *
 *   Modbus / MQTT / Web / LCD
 *     -> data_point_write()      (RAM snapshot only)
 *     -> config_apply(flags)     (this module)
 *     -> the module concerned
 *
 * Scope:
 *   - Feature 11 defined the flag space and log-only domain handlers.
 *   - Feature 14 makes CONFIG_APPLY_MQTT a real runtime Apply: the MQTT manager
 *     destroys its old client and creates a fresh one from the active RAM
 *     profile. Other domain handlers remain log-only.
 *   - Apply never writes or reloads NVS.
 *   - Nothing calls config_apply() automatically; it runs in the caller's
 *     context and adds no task or timer.
 */

/* One bit per applicable domain, so a caller can apply several at once:
 *   config_apply(CONFIG_APPLY_WIFI | CONFIG_APPLY_MQTT);
 * Unknown bits are rejected with ESP_ERR_INVALID_ARG.
 *
 * Modbus is split into SLAVE and MASTER because the two are independent
 * domains: editing the slave address / baud only needs to rebuild the slave
 * RTU stack, not the master. CONFIG_APPLY_MODBUS is kept as the OR of both so
 * the legacy `apply modbus` console command still touches both. */
typedef enum {
    CONFIG_APPLY_NONE             = 0,
    CONFIG_APPLY_ETHERNET         = 1u << 0,  /* DHCP / static IP / gateway / netmask / DNS */
    CONFIG_APPLY_WIFI             = 1u << 1,  /* SSID / password */
    CONFIG_APPLY_MQTT             = 1u << 2,  /* enable / broker / port / credentials / period */
    CONFIG_APPLY_MODBUS_SLAVE     = 1u << 3,  /* slave id / baud / parity (rebuild slave stack only) */
    CONFIG_APPLY_LCD              = 1u << 4,  /* backlight / sleep timeout */
    CONFIG_APPLY_MEASUREMENT      = 1u << 5,  /* reserved no-op: PGA/freq = console+Kconfig only */
    CONFIG_APPLY_ALARM            = 1u << 6,  /* alarm enables / thresholds / delays */
    CONFIG_APPLY_MODBUS_MASTER    = 1u << 7,  /* master slots / baud / parity / period (rebuild master only) */

    /* Legacy alias: CONFIG_APPLY_MODBUS used to mean "both" before the split.
     * Kept so existing callers (console `apply modbus`, all-domain apply)
     * still rebuild both stacks. New code should pick the specific bit. */
    CONFIG_APPLY_MODBUS           = CONFIG_APPLY_MODBUS_SLAVE | CONFIG_APPLY_MODBUS_MASTER,

    CONFIG_APPLY_ALL = CONFIG_APPLY_ETHERNET |
                       CONFIG_APPLY_WIFI |
                       CONFIG_APPLY_MQTT |
                       CONFIG_APPLY_MODBUS_SLAVE |
                       CONFIG_APPLY_LCD |
                       CONFIG_APPLY_MEASUREMENT |
                       CONFIG_APPLY_ALARM |
                       CONFIG_APPLY_MODBUS_MASTER,
} config_apply_flags_t;

/*
 * Apply the current configuration snapshot to the selected domains.
 *   flags — bitwise OR of CONFIG_APPLY_* bits.
 * Handlers run in a fixed order (Ethernet, WiFi, MQTT, Modbus, LCD) so the
 * outcome does not depend on the order of the bits.
 *
 * Returns:
 *   ESP_OK                on success,
 *   ESP_ERR_INVALID_ARG   if flags is CONFIG_APPLY_NONE or contains unknown bits,
 *   the first handler error otherwise — remaining handlers still run, so one
 *   failing domain cannot silently skip the others.
 */
esp_err_t config_apply(config_apply_flags_t flags);

/* Shorthand for config_apply(CONFIG_APPLY_ALL). */
esp_err_t config_apply_all(void);

/* Human-readable name of a single flag bit, for logs (static string, never
 * NULL; "unknown" for anything that is not exactly one known bit). */
const char *config_apply_flag_name(config_apply_flags_t flag);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Web configuration portal.
 *
 * A small HTTP server that runs ONLY while the config-portal SoftAP is up
 * (network_manager_start_config_portal). It lets an operator edit the
 * network / MQTT / system settings in NVS from a browser, then reboot to
 * apply. It is intentionally not served on the ETH/STA data path.
 *
 * Lifecycle is driven by network_manager: start() when the AP comes up,
 * stop() when it goes down. Both are idempotent.
 *
 * Design reference: docs/ESP32_WebPortal_Design.md
 */

/* Start the portal HTTP server. No-op if already running. */
esp_err_t web_portal_start(void);

/* Stop the portal HTTP server and release its resources. No-op if not running. */
esp_err_t web_portal_stop(void);

/* True if the portal HTTP server is currently running. */
bool web_portal_is_running(void);

#ifdef __cplusplus
}
#endif

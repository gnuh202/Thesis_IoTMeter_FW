#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MQTT manager: telemetry + remote-control client for the power meter.
 *
 * Owns the esp-mqtt client, its lifecycle, and reconnection. Reads the active
 * broker profile from config_store (NVS) and connects once the network has an
 * IP. TLS is per-profile (plain / IDF cert bundle / custom CA). The client ID
 * is generated at runtime from the device name plus a MAC suffix.
 *
 * Design: docs/ESP32_MQTT_Design.md
 *
 * Interface handoff: the manager watches network_manager's active interface and
 * proactively restarts the client on an ETH<->STA switch, rather than relying
 * solely on esp-mqtt's own reconnect (which does not know the interface moved).
 *
 * Independence: MQTT is an auxiliary channel. If the broker is down, the network
 * is lost, or TLS fails, the metering and Modbus RTU paths keep running. Nothing
 * here may block or crash the measurement core.
 *
 * Status: SKELETON (step 3). Connects, sets LWT, publishes online/offline
 * status, and subscribes to nothing yet. Telemetry publishing (step 4) and relay
 * command handling (step 5) are added later.
 */

/* Start the MQTT manager task. Reads the active profile; if MQTT is disabled or
 * the profile has no URI, the task idles until reconfigured. Idempotent. */
esp_err_t mqtt_manager_start(void);

/* True if the client currently has a live broker session. */
bool mqtt_manager_is_connected(void);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MQTT manager: telemetry + remote-control client for the power meter.
 *
 * Owns the esp-mqtt client, its lifecycle, runtime recreation and reconnection.
 * Reads the device's single broker from Configuration Manager. TLS is selected
 * by that broker's tls_mode and its certificate paths; PEM files are loaded by
 * the runtime rather than stored in configuration RAM. The client ID and topic
 * bases may come from the broker config, with the existing device-derived
 * defaults retained when those fields are empty.
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
 * Runtime state: connects, publishes the existing telemetry/status topics,
 * subscribes to relay commands, and can destroy/recreate its client on Apply.
 */

/* Start the MQTT manager task. Reads the broker config; if MQTT is disabled or
 * no broker host is set, the task remains idle and ready for a later Apply.
 * Idempotent. */
esp_err_t mqtt_manager_start(void);

/* Apply the current MQTT config at runtime.
 *
 * The existing manager task synchronously stops and destroys its current
 * esp-mqtt client, reads the broker config again from Configuration Manager,
 * then creates and starts a fresh client. Nothing is saved to or reloaded from
 * NVS. Returns only after that lifecycle attempt has completed. */
esp_err_t mqtt_manager_apply(void);

/* True if the client currently has a live broker session. */
bool mqtt_manager_is_connected(void);

#ifdef __cplusplus
}
#endif

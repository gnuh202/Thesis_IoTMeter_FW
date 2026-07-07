#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * WiFi manager: owns the STA + SoftAP bring-up and the WiFi/IP event handlers.
 * It publishes STA got-IP state through an event group so consumers (the meter
 * upload task now, network_manager later) can wait on it without re-registering
 * handlers.
 *
 * The device boots STA-only (no AP). The SoftAP is on-demand: brought up only
 * for the config portal or as an auto-AP recovery fallback, then torn down. The
 * AP is never a data path — it only serves configuration. Infrastructure init
 * (NVS / netif / event loop) is owned by network_manager_infra_init(), which
 * must run before this manager.
 */

#define WIFI_MANAGER_STA_GOT_IP_BIT BIT0

/* Bring up WiFi in STA-only mode (STA left idle until sta_connect(); no AP).
 * Registers WiFi/IP event handlers and begins driving the event group. Safe to
 * call more than once (subsequent calls are no-ops). */
esp_err_t wifi_manager_start(void);

/*
 * Bring the SoftAP up / down on demand (for the config portal or auto-AP
 * recovery). Switches the WiFi mode between STA and APSTA without disturbing the
 * STA link. start_ap() is a no-op if the AP is already up; stop_ap() if down.
 */
esp_err_t wifi_manager_start_ap(void);
esp_err_t wifi_manager_stop_ap(void);

/* True if the SoftAP is currently up. */
bool wifi_manager_ap_is_active(void);

/* Event group carrying WIFI_MANAGER_STA_GOT_IP_BIT (set on STA got-IP, cleared
 * on STA disconnect). NULL until wifi_manager_start() has run. */
EventGroupHandle_t wifi_manager_event_group(void);

/* True if the WiFi STA currently holds a valid IP. */
bool wifi_manager_sta_has_ip(void);

/*
 * Set the STA credentials at runtime (does not connect). Copied into the WiFi
 * driver's STA config; takes effect on the next wifi_manager_sta_connect().
 * Passing an empty SSID clears the stored credentials.
 */
esp_err_t wifi_manager_sta_set_credentials(const char *ssid, const char *password);

/*
 * Bring the STA up / down on demand. connect() enables auto-(re)connect and
 * starts associating; disconnect() disables auto-reconnect and drops the link.
 * Used by network_manager for ETH>STA failover (STA is brought up only when
 * Ethernet is unavailable). Both are no-ops if the state is already as asked.
 */
esp_err_t wifi_manager_sta_connect(void);
esp_err_t wifi_manager_sta_disconnect(void);

/* True if the STA has been asked to stay connected (auto-reconnect enabled). */
bool wifi_manager_sta_is_enabled(void);

/* Consecutive STA connect failures since the STA was last enabled or last got
 * an IP. Reset to 0 on connect request and on got-IP. network_manager reads
 * this to decide when to give up on WiFi and enter auto-AP recovery. */
uint32_t wifi_manager_sta_fail_count(void);

/* The STA netif handle, or NULL before wifi_manager_start() has run. Used by
 * network_manager to set the default netif on interface switches. */
esp_netif_t *wifi_manager_sta_netif(void);

#ifdef __cplusplus
}
#endif

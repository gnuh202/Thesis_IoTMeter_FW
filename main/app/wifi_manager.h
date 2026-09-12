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
 * Bring the SoftAP up / down on demand for the user-launched config portal.
 * AP and STA are mutually exclusive: start_ap() disconnects the STA side first;
 * stop_ap() reconnects it when credentials are available, so the data path
 * returns automatically when the portal closes. Both are no-ops if the state
 * is already as asked.
 */
esp_err_t wifi_manager_start_ap(void);
esp_err_t wifi_manager_stop_ap(void);

/* True if the SoftAP is currently up. */
bool wifi_manager_ap_is_active(void);

/* Number of stations currently associated with the SoftAP (0 if AP is down). */
uint8_t wifi_manager_ap_sta_count(void);

/* Copy current SoftAP SSID into out (NUL-terminated). Uses runtime config when
 * available, else Kconfig default. out_len must be > 0. */
esp_err_t wifi_manager_ap_get_ssid(char *out, size_t out_len);

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
 * True if an STA SSID is currently stored in the WiFi driver. Read live rather
 * than latched, so credentials applied at runtime (config_apply) are seen by the
 * failover logic without a reboot.
 */
bool wifi_manager_sta_has_credentials(void);

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
 * an IP. Reset to 0 on connect request and on got-IP. */
uint32_t wifi_manager_sta_fail_count(void);

/* The STA netif handle, or NULL before wifi_manager_start() has run. Used by
 * network_manager to set the default netif on interface switches. */
esp_netif_t *wifi_manager_sta_netif(void);

#ifdef __cplusplus
}
#endif

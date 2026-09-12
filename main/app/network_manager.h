#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * NetworkManager: owns the network state machine and the shared network
 * infrastructure (netif init, default event loop, default-netif selection).
 *
 * Design: docs/ESP32_Network_Manager_Design.md
 *
 * Status: SKELETON. The state machine and public API are defined here, but the
 * transitions are not yet wired to the real Ethernet/WiFi drivers. The existing
 * network_comm_task / wifi_meter_server remain in charge until each driver is
 * migrated behind this manager (phase A, one piece at a time). Nothing calls
 * network_manager_start() yet.
 */

typedef enum {
    NETWORK_STATE_INIT = 0,        /* pre-start */
    NETWORK_STATE_LOAD_CONFIG,     /* reading NetworkConfig from NVS */
    NETWORK_STATE_CHECK_ETH,       /* eth started, waiting for link */
    NETWORK_STATE_ETH_GET_IP,      /* link up, waiting for DHCP/static IP */
    NETWORK_STATE_ETH_ACTIVE,      /* Ethernet is the data-path interface */
    NETWORK_STATE_WIFI_CONNECTING, /* STA associating / waiting for IP */
    NETWORK_STATE_WIFI_ACTIVE,     /* WiFi STA is the data-path interface */
    NETWORK_STATE_AP_MODE,         /* SoftAP up (auto-AP fallback) */
    NETWORK_STATE_RESTART_MANAGER, /* tear down + re-enter INIT with new config */
    NETWORK_STATE_ERROR,           /* unrecoverable without retry/restart */
} network_state_t;

/* Which data-path interface currently carries traffic (AP is not a data-path). */
typedef enum {
    NETWORK_IFACE_NONE = 0,
    NETWORK_IFACE_ETH,
    NETWORK_IFACE_WIFI_STA,
} network_iface_t;

typedef struct {
    network_state_t state;
    network_iface_t active_iface;
    bool has_ip;
    bool ap_active;
    char ip[16];               /* dotted IPv4 of the active interface, or "" */
} network_status_t;

const char *network_state_str(network_state_t state);

/*
 * Initialize shared network infrastructure exactly once: NVS flash, the netif
 * layer, and the default event loop. This is the single owner of these inits;
 * ethernet_driver and wifi_manager rely on it having run first and must not
 * init these themselves. Idempotent — safe to call more than once.
 *
 * Call this early in app startup, before any network driver and before any
 * consumer that needs NVS.
 */
esp_err_t network_manager_infra_init(void);

/*
 * Start the manager: init infrastructure, load config, launch the state-machine
 * task. Idempotent. NOTE: not yet called from app_tasks during phase-A bring-up.
 */
esp_err_t network_manager_start(void);

/* Snapshot the current network status (thread-safe). */
esp_err_t network_manager_get_status(network_status_t *out);

/*
 * Request the config portal SoftAP (on-demand). Drops the STA so AP/STA never
 * coexist; the STA returns when the portal closes (auto-stop idle timer or
 * explicit stop). Ethernet is unaffected.
 */
esp_err_t network_manager_start_config_portal(void);
esp_err_t network_manager_stop_config_portal(void);

/*
 * Re-read config from NVS and restart the data-path without rebooting the
 * device. Used after the web portal saves new settings.
 */
esp_err_t network_manager_restart(void);

#ifdef __cplusplus
}
#endif

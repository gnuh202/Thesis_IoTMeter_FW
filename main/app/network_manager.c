#include "network_manager.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "config_manager.h"
#include "config_store.h"
#include "ethernet_driver.h"
#include "web_portal.h"
#include "wifi_manager.h"

/*
 * NetworkManager skeleton.
 *
 * This compiles and runs a state-machine task, but the transitions are not yet
 * connected to the real Ethernet/WiFi drivers. During phase-A migration the
 * existing network_comm_task / wifi_meter_server stay in charge; each driver is
 * moved behind this manager one piece at a time, with a build/flash check
 * between steps. Until then, nothing calls network_manager_start().
 *
 * Design reference: docs/ESP32_Network_Manager_Design.md
 */

static const char *TAG = "net_mgr";

static SemaphoreHandle_t s_status_mutex;
static network_status_t s_status;
static bool s_started;

const char *network_state_str(network_state_t state)
{
    switch (state) {
    case NETWORK_STATE_INIT: return "INIT";
    case NETWORK_STATE_LOAD_CONFIG: return "LOAD_CONFIG";
    case NETWORK_STATE_CHECK_ETH: return "CHECK_ETH";
    case NETWORK_STATE_ETH_GET_IP: return "ETH_GET_IP";
    case NETWORK_STATE_ETH_ACTIVE: return "ETH_ACTIVE";
    case NETWORK_STATE_WIFI_CONNECTING: return "WIFI_CONNECTING";
    case NETWORK_STATE_WIFI_ACTIVE: return "WIFI_ACTIVE";
    case NETWORK_STATE_AP_MODE: return "AP_MODE";
    case NETWORK_STATE_RESTART_MANAGER: return "RESTART_MANAGER";
    case NETWORK_STATE_ERROR: return "ERROR";
    default: return "?";
    }
}

static void set_state(network_state_t state)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    network_state_t prev = s_status.state;
    s_status.state = state;
    xSemaphoreGive(s_status_mutex);
    if (prev != state) {
        ESP_LOGI(TAG, "state %s -> %s", network_state_str(prev), network_state_str(state));
    }
}

esp_err_t network_manager_get_status(network_status_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    /* Not started yet is a normal boot-time race (a consumer may poll before
     * network_manager_start() runs), not an error — return quietly. */
    if (s_status_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_status_mutex);
    return ESP_OK;
}

bool network_manager_is_config_mode(void)
{
    if (s_status_mutex == NULL) {
        return false;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    bool active = s_status.ap_active;
    xSemaphoreGive(s_status_mutex);
    return active;
}

#ifndef CONFIG_APP_NET_ETH_DOWN_DEBOUNCE_MS
#define CONFIG_APP_NET_ETH_DOWN_DEBOUNCE_MS 3000
#endif
#ifndef CONFIG_APP_NET_STA_RETRY_MAX
#define CONFIG_APP_NET_STA_RETRY_MAX 5
#endif
#ifndef CONFIG_APP_NET_AP_RECOVERY_GRACE_MS
#define CONFIG_APP_NET_AP_RECOVERY_GRACE_MS 60000
#endif
#ifndef CONFIG_APP_NET_AP_IDLE_TIMEOUT_MS
#define CONFIG_APP_NET_AP_IDLE_TIMEOUT_MS 60000
#endif

#define NET_POLL_PERIOD_MS 500

/* Publish the active interface + IP into the shared status snapshot. */
static void publish_iface(network_iface_t iface, bool has_ip, const char *ip)
{
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.active_iface = iface;
    s_status.has_ip = has_ip;
    if (ip != NULL) {
        strlcpy(s_status.ip, ip, sizeof(s_status.ip));
    } else {
        s_status.ip[0] = '\0';
    }
    xSemaphoreGive(s_status_mutex);
}

static void set_ap_active(bool active)
{
    if (s_status_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.ap_active = active;
    xSemaphoreGive(s_status_mutex);
}

/* Start/stop SoftAP and the HTTP portal as one lifecycle unit. */
static esp_err_t stop_ap_with_portal(void);

static esp_err_t start_ap_with_portal(void)
{
    esp_err_t ret = wifi_manager_start_ap();
    if (ret != ESP_OK) {
        return ret;
    }

    ret = web_portal_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "web portal start failed: %s", esp_err_to_name(ret));
        wifi_manager_stop_ap();
        return ret;
    }

    set_ap_active(true);
    return ESP_OK;
}

static esp_err_t stop_ap_with_portal(void)
{
    esp_err_t web_ret = web_portal_stop();
    if (web_ret != ESP_OK) {
        ESP_LOGW(TAG, "web portal stop failed: %s", esp_err_to_name(web_ret));
    }

    esp_err_t ap_ret = wifi_manager_stop_ap();
    if (ap_ret == ESP_OK) {
        set_ap_active(false);
    }
    return ap_ret != ESP_OK ? ap_ret : web_ret;
}

/* Make the given netif the default route for outbound traffic. */
static void switch_default_netif(esp_netif_t *netif, const char *label)
{
    if (netif == NULL) {
        ESP_LOGW(TAG, "cannot switch default netif to %s: handle is NULL", label);
        return;
    }
    esp_err_t ret = esp_netif_set_default_netif(netif);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "default netif -> %s", label);
    } else {
        ESP_LOGW(TAG, "set default netif %s failed: %s", label, esp_err_to_name(ret));
    }
}

/*
 * Failover orchestrator (option B: STA is brought up only when Ethernet is
 * unavailable). Ethernet is preferred; on a debounced ETH loss the STA is
 * connected and becomes the default route; when ETH returns the STA is dropped
 * and ETH becomes default again.
 *
 * The data path is whichever interface is set as the default netif. Only one is
 * default at a time, satisfying the "single active data-path" requirement.
 */
static void network_manager_task(void *arg)
{
    set_state(NETWORK_STATE_LOAD_CONFIG);

    /* config_manager_t is ~1.2 KB — heap, not this task's stack. */
    config_manager_t *cfg = malloc(sizeof(config_manager_t));
    if (cfg == NULL) {
        ESP_LOGE(TAG, "out of memory reading network config; STA credentials not primed");
    } else {
        esp_err_t ret = config_manager_get(cfg);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "config_manager not ready (%s); STA credentials not primed",
                     esp_err_to_name(ret));
        } else if (strlen(cfg->wifi_ssid) > 0) {
            esp_err_t cred_ret = wifi_manager_sta_set_credentials(cfg->wifi_ssid, cfg->wifi_pass);
            if (cred_ret == ESP_OK) {
                ESP_LOGI(TAG, "STA credentials primed from config (ssid=\"%s\")", cfg->wifi_ssid);
            } else {
                ESP_LOGW(TAG, "prime STA credentials failed: %s", esp_err_to_name(cred_ret));
            }
        } else {
            ESP_LOGI(TAG, "no STA SSID stored; WiFi failover unavailable until credentials are set");
        }
        free(cfg);
    }

    int64_t eth_down_since_us = 0;   /* 0 = ETH not currently in a down window */
    bool sta_requested = false;      /* have we asked the STA to come up? */
    network_iface_t default_iface = NETWORK_IFACE_NONE; /* last netif we made default */
    int64_t ap_idle_since_us = 0;    /* 0 = AP idle timer not running */
    bool offline_no_sta_logged = false; /* throttle the "staying offline" line */

    set_state(NETWORK_STATE_CHECK_ETH);

    while (1) {
        bool eth_ip = ethernet_driver_has_ip();
        bool sta_ip = wifi_manager_sta_has_ip();
        bool ap_active = wifi_manager_ap_is_active();

        /* The config portal tears the STA down (start_ap). If that happened
         * while we owned a failover request, forget it — otherwise the task
         * waits forever on an STA that will never associate and never re-runs
         * the failover after the portal closes. */
        if (sta_requested && !wifi_manager_sta_is_enabled()) {
            ESP_LOGI(TAG, "STA torn down externally; failover request cleared");
            sta_requested = false;
        }

        /* AP and STA are mutually exclusive. While the config portal is up, skip
         * the failover logic so the task does not race to re-connect the STA.
         * Ethernet is unaffected and still reported as the data path. */
        if (eth_ip) {
            /* Ethernet is the preferred data path. Drop STA if we had raised it. */
            eth_down_since_us = 0;
            offline_no_sta_logged = false;
            if (sta_requested) {
                ESP_LOGI(TAG, "Ethernet restored; dropping WiFi STA failover");
                wifi_manager_sta_disconnect();
                sta_requested = false;
            }
            /* Only switch + log on an actual interface change, not every poll. */
            if (default_iface != NETWORK_IFACE_ETH) {
                switch_default_netif(ethernet_driver_netif(), "ETH");
                default_iface = NETWORK_IFACE_ETH;
            }
            publish_iface(NETWORK_IFACE_ETH, true, NULL);
            set_state(NETWORK_STATE_ETH_ACTIVE);
        } else if (!sta_requested) {
            /* Ethernet has no IP yet. Two cases must be told apart:
             *   - link UP but no IP: DHCP is still in progress (e.g. at boot, or
             *     right after a cable reconnect). This is NOT a failure — wait,
             *     do not run the failover debounce.
             *   - link DOWN: the cable is unplugged / negotiation lost. Only then
             *     does the debounce window run before failing over to WiFi.
             * Tracking link state (not just "no IP") prevents a spurious failover
             * during the initial DHCP wait. */
            set_state(NETWORK_STATE_CHECK_ETH);
            publish_iface(NETWORK_IFACE_NONE, false, NULL);

            if (!ap_active && ethernet_driver_link_is_up()) {
                /* Link is up, just waiting for DHCP — reset the down window and
                 * the one-shot log so a fresh ETH drop can log again. */
                eth_down_since_us = 0;
                offline_no_sta_logged = false;
            } else if (!ap_active) {
                int64_t now = esp_timer_get_time();
                if (eth_down_since_us == 0) {
                    eth_down_since_us = now;
                }
                int64_t down_ms = (now - eth_down_since_us) / 1000;

                if (down_ms >= CONFIG_APP_NET_ETH_DOWN_DEBOUNCE_MS) {
                    /* No auto-AP fallback. With Ethernet down and no STA
                     * credentials, the device is simply offline — the operator
                     * raises the portal manually from the LCD menu. Log once;
                     * repeat only if the link or credential state changes. */
                    if (wifi_manager_sta_has_credentials()) {
                        ESP_LOGW(TAG, "Ethernet link down %lld ms; failing over to WiFi STA", (long long)down_ms);
                        if (wifi_manager_sta_connect() == ESP_OK) {
                            sta_requested = true;
                            set_state(NETWORK_STATE_WIFI_CONNECTING);
                        } else {
                            ESP_LOGE(TAG, "STA connect request failed; will retry");
                        }
                        offline_no_sta_logged = false;
                    } else if (!offline_no_sta_logged) {
                        ESP_LOGI(TAG, "Ethernet down and no STA credentials; staying offline (raise portal from LCD)");
                        offline_no_sta_logged = true;
                    }
                }
            }
        } else {
            /* STA failover in progress or active. */
            if (sta_ip) {
                /* Only switch + log on an actual interface change, not every poll. */
                if (default_iface != NETWORK_IFACE_WIFI_STA) {
                    switch_default_netif(wifi_manager_sta_netif(), "WiFi STA");
                    default_iface = NETWORK_IFACE_WIFI_STA;
                }
                esp_netif_ip_info_t ip_info;
                char ip_str[16] = "";
                if (wifi_manager_sta_netif() != NULL &&
                    esp_netif_get_ip_info(wifi_manager_sta_netif(), &ip_info) == ESP_OK) {
                    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
                }
                publish_iface(NETWORK_IFACE_WIFI_STA, true, ip_str);
                set_state(NETWORK_STATE_WIFI_ACTIVE);
            } else {
                /* STA is trying but has no IP. No auto-AP fallback — the operator
                 * raises the portal manually from the LCD menu if needed. */
                set_state(NETWORK_STATE_WIFI_CONNECTING);
                publish_iface(NETWORK_IFACE_NONE, false, NULL);
            }
        }

        /*
         * Config-portal AP idle timer. The user launches the SoftAP from the LCD
         * menu; if no station associates within the Kconfig window, stop the AP
         * automatically and reconnect the STA so the device returns to normal
         * operation. Any associated station resets the timer.
         */
        if (wifi_manager_ap_is_active()) {
            if (wifi_manager_ap_sta_count() > 0) {
                ap_idle_since_us = 0;
            } else if (ap_idle_since_us == 0) {
                ap_idle_since_us = esp_timer_get_time();
                ESP_LOGI(TAG, "config portal AP up, no station yet; idle timeout %d ms",
                         CONFIG_APP_NET_AP_IDLE_TIMEOUT_MS);
            } else if ((esp_timer_get_time() - ap_idle_since_us) / 1000 >= CONFIG_APP_NET_AP_IDLE_TIMEOUT_MS) {
                ESP_LOGI(TAG, "config portal AP idle for %d ms; stopping",
                         CONFIG_APP_NET_AP_IDLE_TIMEOUT_MS);
                stop_ap_with_portal();
                ap_idle_since_us = 0;
            }
        } else {
            ap_idle_since_us = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(NET_POLL_PERIOD_MS));
    }
}

esp_err_t network_manager_infra_init(void)
{
    static bool s_infra_done;
    if (s_infra_done) {
        return ESP_OK;
    }

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
        ret = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "init NVS failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "init netif failed");

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "create event loop failed");
    }

    s_infra_done = true;
    return ESP_OK;
}

esp_err_t network_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    if (s_status_mutex == NULL) {
        s_status_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_status_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create status mutex failed");
    }

    memset(&s_status, 0, sizeof(s_status));
    s_status.state = NETWORK_STATE_INIT;
    s_status.active_iface = NETWORK_IFACE_NONE;

    ESP_RETURN_ON_ERROR(config_store_init(), TAG, "config store init failed");

    BaseType_t ok = xTaskCreate(network_manager_task, "net_mgr", 4096, NULL,
                                CONFIG_APP_NETWORK_COMM_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "create net_mgr task failed");

    s_started = true;
    return ESP_OK;
}

/*
 * Config-portal and restart entry points are used by console/web/LCD layers.
 * Config portal owns SoftAP + web_portal as one lifecycle unit.
 */
esp_err_t network_manager_start_config_portal(void)
{
    /* Bring up the SoftAP on demand. AP and STA are mutually exclusive, so the
     * STA is dropped for the duration of the portal and reconnected when the
     * portal closes (idle auto-stop or explicit stop). Ethernet is unaffected. */
    return start_ap_with_portal();
}

esp_err_t network_manager_stop_config_portal(void)
{
    return stop_ap_with_portal();
}

esp_err_t network_manager_restart(void)
{
    ESP_LOGW(TAG, "restart: not implemented in skeleton");
    return ESP_ERR_NOT_SUPPORTED;
}

#include "wifi_manager.h"

#include <string.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "network_manager.h"
#include "sdkconfig.h"

/*
 * WiFi manager. Extracted verbatim from wifi_meter_server.c during phase-A
 * network refactor; behavior is unchanged (APSTA, AP always on). Shared
 * infrastructure init (NVS / netif / event loop) is now owned by
 * network_manager_infra_init(), which must run before this manager. AP
 * on-demand is a separate later step.
 */

static const char *TAG = "wifi_manager";

static bool s_started;
static EventGroupHandle_t s_event_group;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static bool s_ap_active;

/*
 * Whether the STA should stay connected. When false, the event handler does not
 * (re)connect, so the STA stays down. network_manager toggles this for ETH>STA
 * failover. Defaults to true in wifi_manager_start() to preserve the original
 * boot behavior until the orchestrator (step 5b-2) takes over.
 */
static volatile bool s_sta_enabled;

/*
 * Consecutive STA connect failures since the STA was last enabled or last got an
 * IP. network_manager reads this to decide when to give up on WiFi and fall back
 * to auto-AP recovery. Reset on connect request and on got-IP.
 */
static volatile uint32_t s_sta_fail_count;

static void wifi_manager_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_sta_enabled) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_event_group, WIFI_MANAGER_STA_GOT_IP_BIT);
        if (s_sta_enabled) {
            s_sta_fail_count++;
            ESP_LOGW(TAG, "WiFi STA disconnected (fail %u), reconnecting", (unsigned)s_sta_fail_count);
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi STA got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_fail_count = 0;
        xEventGroupSetBits(s_event_group, WIFI_MANAGER_STA_GOT_IP_BIT);
    }
}

esp_err_t wifi_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    if (s_event_group == NULL) {
        s_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create WiFi event group failed");
    }

    ESP_RETURN_ON_ERROR(network_manager_infra_init(), TAG, "init network infra failed");

    /* Both netifs are created up front (they are just netif objects); the SoftAP
     * does not broadcast until the mode includes AP, which happens on demand in
     * wifi_manager_start_ap(). */
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_config), TAG, "init WiFi failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_manager_event_handler, NULL), TAG, "register WiFi event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_manager_event_handler, NULL), TAG, "register IP event failed");

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, CONFIG_APP_WIFI_METER_STA_SSID, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, CONFIG_APP_WIFI_METER_STA_PASSWORD, sizeof(sta_config.sta.password));

    /* Boot STA-only: the AP is on-demand (config portal / auto-AP recovery),
     * not a permanent data path. The orchestrator connects the STA only when
     * Ethernet is unavailable. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set WiFi STA mode failed");
    if (strlen(CONFIG_APP_WIFI_METER_STA_SSID) > 0) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "set WiFi STA config failed");
    }

    /* STA stays down until the orchestrator enables it on ETH failover. */
    s_sta_enabled = false;

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start WiFi failed");

    s_started = true;
    ESP_LOGI(TAG, "WiFi manager started (STA-only; AP on-demand)");
    return ESP_OK;
}

EventGroupHandle_t wifi_manager_event_group(void)
{
    return s_event_group;
}

bool wifi_manager_sta_has_ip(void)
{
    if (s_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_event_group) & WIFI_MANAGER_STA_GOT_IP_BIT) != 0;
}

esp_err_t wifi_manager_sta_set_credentials(const char *ssid, const char *password)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "wifi manager not started");
    ESP_RETURN_ON_FALSE(ssid != NULL, ESP_ERR_INVALID_ARG, TAG, "ssid is NULL");

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    if (password != NULL) {
        strlcpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password));
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "set STA config failed");
    ESP_LOGI(TAG, "STA credentials set (ssid=\"%s\")", ssid);
    return ESP_OK;
}

esp_err_t wifi_manager_sta_connect(void)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "wifi manager not started");

    s_sta_enabled = true;
    s_sta_fail_count = 0;   /* fresh failover attempt; reset the retry counter */
    esp_err_t ret = esp_wifi_connect();
    /* ESP_ERR_WIFI_CONN means a connect is already in progress: not an error. */
    if (ret == ESP_ERR_WIFI_CONN) {
        return ESP_OK;
    }
    return ret;
}

esp_err_t wifi_manager_sta_disconnect(void)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "wifi manager not started");

    s_sta_enabled = false;
    xEventGroupClearBits(s_event_group, WIFI_MANAGER_STA_GOT_IP_BIT);
    esp_err_t ret = esp_wifi_disconnect();
    /* Not connected yet is fine — the caller just wants the STA down. */
    if (ret == ESP_ERR_WIFI_NOT_STARTED || ret == ESP_ERR_WIFI_NOT_CONNECT) {
        return ESP_OK;
    }
    return ret;
}

bool wifi_manager_sta_is_enabled(void)
{
    return s_sta_enabled;
}

uint32_t wifi_manager_sta_fail_count(void)
{
    return s_sta_fail_count;
}

esp_netif_t *wifi_manager_sta_netif(void)
{
    return s_sta_netif;
}

/*
 * Bring the SoftAP up on demand for the config portal / auto-AP recovery.
 * Switches the WiFi mode to APSTA so the AP coexists with any STA link, then
 * applies the AP config. No-op if the AP is already active.
 */
esp_err_t wifi_manager_start_ap(void)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "wifi manager not started");
    if (s_ap_active) {
        return ESP_OK;
    }

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, CONFIG_APP_WIFI_METER_AP_SSID, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, CONFIG_APP_WIFI_METER_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(CONFIG_APP_WIFI_METER_AP_SSID);
    ap_config.ap.channel = CONFIG_APP_WIFI_METER_AP_CHANNEL;
    ap_config.ap.max_connection = CONFIG_APP_WIFI_METER_AP_MAX_CONN;
    ap_config.ap.authmode = strlen(CONFIG_APP_WIFI_METER_AP_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "set AP config failed");

    s_ap_active = true;
    ESP_LOGI(TAG, "SoftAP started. SSID=%s", CONFIG_APP_WIFI_METER_AP_SSID);
    return ESP_OK;
}

/*
 * Take the SoftAP down and return to STA-only mode. The STA link (if any) is
 * left untouched. No-op if the AP is not active.
 */
esp_err_t wifi_manager_stop_ap(void)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "wifi manager not started");
    if (!s_ap_active) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");

    s_ap_active = false;
    ESP_LOGI(TAG, "SoftAP stopped");
    return ESP_OK;
}

bool wifi_manager_ap_is_active(void)
{
    return s_ap_active;
}

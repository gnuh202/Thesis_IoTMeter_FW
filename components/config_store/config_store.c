#include "config_store.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

/*
 * Defaults fall back to the existing Kconfig values where they exist, so a
 * fresh device behaves the same as the current build until the user saves
 * something through the config portal.
 */
#ifndef CONFIG_APP_WIFI_METER_STA_SSID
#define CONFIG_APP_WIFI_METER_STA_SSID ""
#endif
#ifndef CONFIG_APP_WIFI_METER_STA_PASSWORD
#define CONFIG_APP_WIFI_METER_STA_PASSWORD ""
#endif
#ifndef CONFIG_APP_WIFI_METER_AP_PASSWORD
#define CONFIG_APP_WIFI_METER_AP_PASSWORD "12345678"
#endif

#define CONFIG_STORE_MAGIC 0x4E455443U /* "NETC" */

#define NET_VERSION 1
#define MQTT_VERSION 2
#define SYS_VERSION 1

#define NET_NAMESPACE "netcfg"
#define NET_KEY "net_v1"
#define MQTT_NAMESPACE "mqttcfg"
#define MQTT_KEY "mqtt_v2"
#define SYS_NAMESPACE "syscfg"
#define SYS_KEY "sys_v1"

static const char *TAG = "config_store";

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    config_network_t data;
} net_blob_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    config_mqtt_t data;
} mqtt_blob_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    config_system_t data;
} sys_blob_t;

esp_err_t config_store_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
        ret = nvs_flash_init();
    }
    return ret == ESP_ERR_INVALID_STATE ? ESP_OK : ret;
}

void config_store_default_network(config_network_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->mode = CONFIG_NETWORK_MODE_AUTO;
    strlcpy(out->wifi_ssid, CONFIG_APP_WIFI_METER_STA_SSID, sizeof(out->wifi_ssid));
    strlcpy(out->wifi_pass, CONFIG_APP_WIFI_METER_STA_PASSWORD, sizeof(out->wifi_pass));
    out->eth_dhcp = true;
    strlcpy(out->ap_pass, CONFIG_APP_WIFI_METER_AP_PASSWORD, sizeof(out->ap_pass));
}

void config_store_default_mqtt(config_mqtt_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->active = 0;
    out->keepalive_s = 60;
    out->publish_period_ms = 5000;
    out->enabled = false;

    /* Profile 0: a plain local Mosquitto broker, the development default.
     * URI is left empty so the getter reports "not configured" until the user
     * fills it in via the console. Profiles 1 and 2 start blank. */
    strlcpy(out->profiles[0].name, "Mosquitto local", sizeof(out->profiles[0].name));
    out->profiles[0].port = 1883;
    out->profiles[0].tls_enable = false;
    out->profiles[0].use_custom_ca = false;
}

void config_store_default_system(config_system_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    strlcpy(out->device_name, "Power Meter", sizeof(out->device_name));
    strlcpy(out->hostname, "power-meter", sizeof(out->hostname));
}

/*
 * Generic blob read: validates size + magic + version. Returns:
 *   ESP_OK             valid blob copied out
 *   ESP_ERR_NVS_NOT_FOUND  no/invalid blob (caller should use defaults)
 *   other              real NVS fault
 */
static esp_err_t read_blob(const char *ns, const char *key, void *blob, size_t blob_size,
                           uint32_t magic, uint16_t version)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(ns, NVS_READONLY, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "open %s failed", ns);

    size_t size = blob_size;
    ret = nvs_get_blob(nvs, key, blob, &size);
    nvs_close(nvs);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "read %s/%s failed", ns, key);

    if (size != blob_size) {
        ESP_LOGW(TAG, "%s/%s size mismatch (%u vs %u), using defaults", ns, key,
                 (unsigned)size, (unsigned)blob_size);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    /* magic + version are the first 6 bytes of every blob struct. */
    uint32_t stored_magic;
    uint16_t stored_version;
    memcpy(&stored_magic, blob, sizeof(stored_magic));
    memcpy(&stored_version, (uint8_t *)blob + sizeof(stored_magic), sizeof(stored_version));
    if (stored_magic != magic || stored_version != version) {
        ESP_LOGW(TAG, "%s/%s magic/version mismatch, using defaults", ns, key);
        return ESP_ERR_NVS_NOT_FOUND;
    }

    return ESP_OK;
}

static esp_err_t write_blob(const char *ns, const char *key, const void *blob, size_t blob_size)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(ns, NVS_READWRITE, &nvs), TAG, "open %s rw failed", ns);
    esp_err_t ret = nvs_set_blob(nvs, key, blob, blob_size);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

esp_err_t config_store_get_network(config_network_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    net_blob_t blob;
    esp_err_t ret = read_blob(NET_NAMESPACE, NET_KEY, &blob, sizeof(blob), CONFIG_STORE_MAGIC, NET_VERSION);
    if (ret == ESP_OK) {
        *out = blob.data;
        return ESP_OK;
    }
    config_store_default_network(out);
    return ret;
}

esp_err_t config_store_get_mqtt(config_mqtt_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    /* mqtt_blob_t is ~7 KB (3 profiles x 2 KB CA each); keep it off the stack. */
    mqtt_blob_t *blob = malloc(sizeof(*blob));
    ESP_RETURN_ON_FALSE(blob != NULL, ESP_ERR_NO_MEM, TAG, "no mem for mqtt blob");

    esp_err_t ret = read_blob(MQTT_NAMESPACE, MQTT_KEY, blob, sizeof(*blob), CONFIG_STORE_MAGIC, MQTT_VERSION);
    if (ret == ESP_OK) {
        *out = blob->data;
    } else {
        config_store_default_mqtt(out);
    }
    free(blob);
    return ret;
}

esp_err_t config_store_get_system(config_system_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    sys_blob_t blob;
    esp_err_t ret = read_blob(SYS_NAMESPACE, SYS_KEY, &blob, sizeof(blob), CONFIG_STORE_MAGIC, SYS_VERSION);
    if (ret == ESP_OK) {
        *out = blob.data;
        return ESP_OK;
    }
    config_store_default_system(out);
    return ret;
}

esp_err_t config_store_set_network(const config_network_t *in)
{
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "in is NULL");
    net_blob_t blob = {.magic = CONFIG_STORE_MAGIC, .version = NET_VERSION, .reserved = 0, .data = *in};
    return write_blob(NET_NAMESPACE, NET_KEY, &blob, sizeof(blob));
}

esp_err_t config_store_set_mqtt(const config_mqtt_t *in)
{
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "in is NULL");

    /* ~7 KB blob: allocate on the heap rather than the caller's stack. */
    mqtt_blob_t *blob = malloc(sizeof(*blob));
    ESP_RETURN_ON_FALSE(blob != NULL, ESP_ERR_NO_MEM, TAG, "no mem for mqtt blob");

    blob->magic = CONFIG_STORE_MAGIC;
    blob->version = MQTT_VERSION;
    blob->reserved = 0;
    blob->data = *in;
    esp_err_t ret = write_blob(MQTT_NAMESPACE, MQTT_KEY, blob, sizeof(*blob));
    free(blob);
    return ret;
}

esp_err_t config_store_set_system(const config_system_t *in)
{
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "in is NULL");
    sys_blob_t blob = {.magic = CONFIG_STORE_MAGIC, .version = SYS_VERSION, .reserved = 0, .data = *in};
    return write_blob(SYS_NAMESPACE, SYS_KEY, &blob, sizeof(blob));
}

esp_err_t config_store_factory_reset(void)
{
    esp_err_t final_ret = ESP_OK;
    const char *namespaces[] = {NET_NAMESPACE, MQTT_NAMESPACE, SYS_NAMESPACE};

    for (int i = 0; i < 3; i++) {
        nvs_handle_t nvs;
        esp_err_t ret = nvs_open(namespaces[i], NVS_READWRITE, &nvs);
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            continue;
        }
        if (ret != ESP_OK) {
            final_ret = ret;
            continue;
        }
        ret = nvs_erase_all(nvs);
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
        if (ret != ESP_OK) {
            final_ret = ret;
        }
    }

    return final_ret;
}

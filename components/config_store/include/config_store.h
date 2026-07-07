#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Persistent configuration store for the network stack.
 *
 * Three independent NVS blobs (network / MQTT / system), each guarded by a
 * magic + version header. On a missing or mismatched blob, the getter fills
 * the struct with compile-time defaults instead of failing, so the caller can
 * always proceed. This mirrors the ATM90E32AS calibration store pattern.
 *
 * Strings are fixed-size, always NUL-terminated. Sizes are generous enough for
 * real SSIDs, broker URIs, and passwords without dynamic allocation.
 */

#define CONFIG_STORE_SSID_LEN 33      /* 32 chars + NUL (802.11 max) */
#define CONFIG_STORE_PASS_LEN 65      /* 64 chars + NUL (WPA2 max)   */
#define CONFIG_STORE_IP_LEN 16        /* "255.255.255.255" + NUL     */
#define CONFIG_STORE_URI_LEN 128
#define CONFIG_STORE_NAME_LEN 33
#define CONFIG_STORE_CLIENT_ID_LEN 64
#define CONFIG_STORE_MQTT_PROFILE_COUNT 3
#define CONFIG_STORE_MQTT_NAME_LEN 32   /* profile label */
#define CONFIG_STORE_MQTT_CA_LEN 2048   /* custom CA PEM (fits typical single CA) */

typedef enum {
    CONFIG_NETWORK_MODE_AUTO = 0,   /* ETH first, fall back to STA, then auto-AP */
    CONFIG_NETWORK_MODE_ETH_ONLY,
    CONFIG_NETWORK_MODE_WIFI_ONLY,
} config_network_mode_t;

typedef struct {
    config_network_mode_t mode;
    char wifi_ssid[CONFIG_STORE_SSID_LEN];
    char wifi_pass[CONFIG_STORE_PASS_LEN];
    bool eth_dhcp;                          /* true = DHCP, false = static */
    char static_ip[CONFIG_STORE_IP_LEN];
    char gateway[CONFIG_STORE_IP_LEN];
    char netmask[CONFIG_STORE_IP_LEN];
    char dns[CONFIG_STORE_IP_LEN];
    char ap_pass[CONFIG_STORE_PASS_LEN];    /* SoftAP password for config portal */
} config_network_t;

/*
 * One MQTT broker profile. The device stores up to
 * CONFIG_STORE_MQTT_PROFILE_COUNT of these; exactly one is active at a time.
 * The MQTT client ID is NOT stored here — it is generated at runtime from the
 * device name plus a MAC suffix so it is unique on the broker.
 */
typedef struct {
    char name[CONFIG_STORE_MQTT_NAME_LEN];  /* label, e.g. "Mosquitto local" */
    char uri[CONFIG_STORE_URI_LEN];         /* host only, no scheme, e.g. "192.168.1.10" */
    uint16_t port;                          /* 1883 (plain) / 8883 (TLS) */
    char username[CONFIG_STORE_NAME_LEN];
    char password[CONFIG_STORE_PASS_LEN];
    bool tls_enable;                        /* true -> mqtts, verify broker */
    bool use_custom_ca;                     /* true -> ca_cert below; false -> IDF cert bundle */
    char ca_cert[CONFIG_STORE_MQTT_CA_LEN]; /* custom CA PEM, empty if unused */
} mqtt_profile_t;

typedef struct {
    uint8_t active;                                          /* active profile index 0..COUNT-1 */
    mqtt_profile_t profiles[CONFIG_STORE_MQTT_PROFILE_COUNT];
    uint16_t keepalive_s;                                   /* shared across profiles */
    uint32_t publish_period_ms;                             /* telemetry publish period */
    bool enabled;                                           /* MQTT client on/off */
} config_mqtt_t;

typedef struct {
    char device_name[CONFIG_STORE_NAME_LEN];
    char hostname[CONFIG_STORE_NAME_LEN];
} config_system_t;

/* Initialize NVS (with erase/retry fallback). Safe to call more than once. */
esp_err_t config_store_init(void);

/*
 * Getters never fail on missing data: if no valid blob is stored, the struct is
 * filled with defaults and ESP_ERR_NVS_NOT_FOUND is returned so the caller can
 * log "using defaults". Any other error is a real NVS fault.
 */
esp_err_t config_store_get_network(config_network_t *out);
esp_err_t config_store_get_mqtt(config_mqtt_t *out);
esp_err_t config_store_get_system(config_system_t *out);

esp_err_t config_store_set_network(const config_network_t *in);
esp_err_t config_store_set_mqtt(const config_mqtt_t *in);
esp_err_t config_store_set_system(const config_system_t *in);

/* Fill a struct with compile-time defaults without touching NVS. */
void config_store_default_network(config_network_t *out);
void config_store_default_mqtt(config_mqtt_t *out);
void config_store_default_system(config_system_t *out);

/* Erase all three blobs (factory reset for network configuration). */
esp_err_t config_store_factory_reset(void);

#ifdef __cplusplus
}
#endif

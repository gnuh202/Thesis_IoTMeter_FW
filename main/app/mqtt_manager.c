#include "mqtt_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "config_store.h"
#include "energy_meter_task.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "io_expander.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "sdkconfig.h"

/*
 * MQTT manager skeleton (step 3).
 *
 * Connects to the active broker profile once the network has an IP, sets an LWT,
 * and publishes online/offline status. Telemetry publishing and relay command
 * handling arrive in later steps. Runs entirely in its own task so a stalled or
 * failing broker never blocks the metering core.
 *
 * Design reference: docs/ESP32_MQTT_Design.md
 */

#ifndef CONFIG_APP_MQTT_TASK_STACK_SIZE
#define CONFIG_APP_MQTT_TASK_STACK_SIZE 6144
#endif
#ifndef CONFIG_APP_MQTT_TASK_PRIORITY
#define CONFIG_APP_MQTT_TASK_PRIORITY 5
#endif

#define MQTT_TOPIC_MAX 96
#define MQTT_URI_MAX 160

static const char *TAG = "mqtt_mgr";

static esp_mqtt_client_handle_t s_client;
static bool s_started;
static volatile bool s_connected;

/* device_id used in topics (sanitized device name) and the generated client id. */
static char s_device_id[CONFIG_STORE_NAME_LEN];
static char s_client_id[CONFIG_STORE_NAME_LEN + 8];
static char s_topic_status[MQTT_TOPIC_MAX];
static char s_topic_telemetry[MQTT_TOPIC_MAX];
static char s_topic_energy[MQTT_TOPIC_MAX];
static char s_topic_io[MQTT_TOPIC_MAX];
static char s_topic_heartbeat[MQTT_TOPIC_MAX];
static char s_topic_cmd_out0[MQTT_TOPIC_MAX];   /* subscribed: relay out0 control */
static char s_topic_cmd_out1[MQTT_TOPIC_MAX];   /* subscribed: relay out1 control */
static char s_active_broker[CONFIG_STORE_MQTT_NAME_LEN];  /* name of connected profile, for heartbeat */

/* Defined below; the event handler echoes io state after a relay command. */
static void publish_io(void);

/*
 * Sanitize a device name into something safe for an MQTT topic level: drop '/',
 * '+', '#', whitespace and control chars, collapsing them to '_'. Empty or
 * all-invalid input falls back to "PowerMeter".
 */
static void sanitize_device_id(const char *name, char *out, size_t out_len)
{
    size_t j = 0;
    for (size_t i = 0; name[i] != '\0' && j + 1 < out_len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c <= ' ' || c == '/' || c == '+' || c == '#') {
            out[j++] = '_';
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    if (j == 0) {
        strlcpy(out, "PowerMeter", out_len);
    }
}

/* Build device id + unique client id (device id + last 3 MAC bytes). */
static void build_identity(void)
{
    config_system_t sys;
    config_store_get_system(&sys);
    sanitize_device_id(sys.device_name, s_device_id, sizeof(s_device_id));

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_client_id, sizeof(s_client_id), "%s-%02X%02X%02X",
             s_device_id, mac[3], mac[4], mac[5]);

    snprintf(s_topic_status, sizeof(s_topic_status), "pm/%s/status", s_device_id);
    snprintf(s_topic_telemetry, sizeof(s_topic_telemetry), "pm/%s/telemetry", s_device_id);
    snprintf(s_topic_energy, sizeof(s_topic_energy), "pm/%s/energy", s_device_id);
    snprintf(s_topic_io, sizeof(s_topic_io), "pm/%s/io", s_device_id);
    snprintf(s_topic_heartbeat, sizeof(s_topic_heartbeat), "pm/%s/heartbeat", s_device_id);
    snprintf(s_topic_cmd_out0, sizeof(s_topic_cmd_out0), "pm/%s/cmd/out0", s_device_id);
    snprintf(s_topic_cmd_out1, sizeof(s_topic_cmd_out1), "pm/%s/cmd/out1", s_device_id);
}

/*
 * Parse a relay command payload and drive the output. The only accepted form is
 * a JSON object {"state":"on"|"off"}; anything else (bad JSON, missing/wrong
 * type, unrecognized value, extra keys are ignored) is rejected without touching
 * the relay. On a valid command the physical output is set and the io topic is
 * re-published so subscribers see the confirmed state.
 */
static void handle_relay_command(int out_index, const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (root == NULL) {
        ESP_LOGW(TAG, "cmd/out%d: payload is not valid JSON; ignored", out_index);
        return;
    }

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsString(state) || state->valuestring == NULL) {
        ESP_LOGW(TAG, "cmd/out%d: missing/invalid \"state\" string; ignored", out_index);
        cJSON_Delete(root);
        return;
    }

    bool level;
    if (strcmp(state->valuestring, "on") == 0) {
        level = true;
    } else if (strcmp(state->valuestring, "off") == 0) {
        level = false;
    } else {
        ESP_LOGW(TAG, "cmd/out%d: state must be \"on\" or \"off\"; ignored", out_index);
        cJSON_Delete(root);
        return;
    }

    esp_err_t ret = (out_index == 0) ? io_expander_set_out0(level)
                                     : io_expander_set_out1(level);
    cJSON_Delete(root);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "cmd/out%d: set relay failed: %s", out_index, esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "cmd/out%d -> %s", out_index, level ? "on" : "off");
    publish_io();  /* echo confirmed state */
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "connected to broker");
        /* Announce presence; retained so late subscribers see it. Matches the
         * LWT topic so offline/online toggle on the same retained topic. Use the
         * handle from the event, not s_client: the CONNECTED event can fire from
         * the esp-mqtt task before esp_mqtt_client_start() returns and assigns
         * s_client, so the global may still be NULL here. */
        esp_mqtt_client_publish(event->client, s_topic_status, "online", 0, 1, 1);
        /* Subscribe to the relay command topics (QoS1). */
        esp_mqtt_client_subscribe(event->client, s_topic_cmd_out0, 1);
        esp_mqtt_client_subscribe(event->client, s_topic_cmd_out1, 1);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected from broker");
        break;
    case MQTT_EVENT_DATA:
        /* Route relay commands. Topic is not NUL-terminated: compare by length. */
        if (event->topic_len == (int)strlen(s_topic_cmd_out0) &&
            strncmp(event->topic, s_topic_cmd_out0, event->topic_len) == 0) {
            handle_relay_command(0, event->data, event->data_len);
        } else if (event->topic_len == (int)strlen(s_topic_cmd_out1) &&
                   strncmp(event->topic, s_topic_cmd_out1, event->topic_len) == 0) {
            handle_relay_command(1, event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle != NULL &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGW(TAG, "transport error: esp_tls_last_err=0x%x sock_errno=%d",
                     event->error_handle->esp_tls_last_esp_err,
                     event->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        break;
    }
}

/*
 * Build the esp-mqtt config from a profile and start the client. Returns
 * ESP_ERR_INVALID_STATE (not a fault) if the profile has no URI to connect to.
 */
static esp_err_t start_client_for_profile(const mqtt_profile_t *p, uint16_t keepalive_s)
{
    if (strlen(p->uri) == 0) {
        ESP_LOGW(TAG, "active profile has no URI; MQTT idle until configured");
        return ESP_ERR_INVALID_STATE;
    }

    char uri[MQTT_URI_MAX];
    const char *scheme = p->tls_enable ? "mqtts" : "mqtt";
    snprintf(uri, sizeof(uri), "%s://%s:%u", scheme, p->uri, (unsigned)p->port);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = s_client_id,
        .session.keepalive = keepalive_s,
        .session.last_will = {
            .topic = s_topic_status,
            .msg = "offline",
            .msg_len = 0,
            .qos = 1,
            .retain = 1,
        },
    };

    if (strlen(p->username) > 0) {
        cfg.credentials.username = p->username;
        cfg.credentials.authentication.password = p->password;
    }

    if (p->tls_enable) {
        if (p->use_custom_ca && strlen(p->ca_cert) > 0) {
            cfg.broker.verification.certificate = p->ca_cert;
        } else {
            cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
        }
    }

    s_client = esp_mqtt_client_init(&cfg);
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_FAIL, TAG, "mqtt client init failed");

    ESP_RETURN_ON_ERROR(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                       mqtt_event_handler, NULL),
                        TAG, "register mqtt event failed");
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_client), TAG, "mqtt client start failed");

    ESP_LOGI(TAG, "client started: %s (client_id=%s)", uri, s_client_id);
    return ESP_OK;
}

/* Publish a cJSON object to a topic, then free it. Takes ownership of root. */
static void publish_json(const char *topic, cJSON *root, int qos, int retain)
{
    if (root == NULL) {
        return;
    }
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (payload == NULL) {
        ESP_LOGW(TAG, "json print failed for %s", topic);
        return;
    }
    esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
    cJSON_free(payload);
}

/* pm/<id>/telemetry — instantaneous measurements (QoS0, no retain). */
static void publish_telemetry(void)
{
    atm90e32as_measurements_t m;
    if (energy_meter_get_latest(&m) != ESP_OK) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }

    cJSON *v = cJSON_AddArrayToObject(root, "v");
    cJSON *i = cJSON_AddArrayToObject(root, "i");
    cJSON *pf = cJSON_AddArrayToObject(root, "pf");
    for (int ph = 0; ph < ATM90E32AS_PHASE_COUNT; ph++) {
        cJSON_AddItemToArray(v, cJSON_CreateNumber(m.voltage[ph]));
        cJSON_AddItemToArray(i, cJSON_CreateNumber(m.current[ph]));
        cJSON_AddItemToArray(pf, cJSON_CreateNumber(m.power_factor[ph]));
    }
    cJSON_AddNumberToObject(root, "in", m.current_neutral);
    cJSON_AddNumberToObject(root, "p", m.total_active_power);
    cJSON_AddNumberToObject(root, "q", m.total_reactive_power);
    cJSON_AddNumberToObject(root, "s", m.total_apparent_power);
    cJSON_AddNumberToObject(root, "pf_total", m.total_power_factor);
    cJSON_AddNumberToObject(root, "freq", m.frequency);
    cJSON_AddNumberToObject(root, "temp", m.temperature);

    publish_json(s_topic_telemetry, root, 0, 0);
}

/* pm/<id>/energy — accumulated energy + demand (QoS1). */
static void publish_energy(void)
{
    energy_meter_energy_t e;
    energy_meter_demand_t d;
    bool have_e = energy_meter_get_energy(&e) == ESP_OK;
    bool have_d = energy_meter_get_demand(&d) == ESP_OK;
    if (!have_e && !have_d) {
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    if (have_e) {
        cJSON_AddNumberToObject(root, "imp_kwh", e.active_import_kwh);
        cJSON_AddNumberToObject(root, "exp_kwh", e.active_export_kwh);
        cJSON_AddNumberToObject(root, "imp_kvarh", e.reactive_import_kvarh);
        cJSON_AddNumberToObject(root, "exp_kvarh", e.reactive_export_kvarh);
    }
    if (have_d) {
        cJSON_AddNumberToObject(root, "dmd_w", d.active_power_demand_w);
        cJSON_AddNumberToObject(root, "dmd_max_w", d.active_power_demand_max_w);
    }

    publish_json(s_topic_energy, root, 1, 0);
}

/* pm/<id>/io — relay outputs + digital inputs (QoS1, retained). */
static void publish_io(void)
{
    bool in0 = false, in1 = false, out0 = false, out1 = false;
    io_expander_get_in0(&in0);
    io_expander_get_in1(&in1);
    io_expander_get_out0(&out0);
    io_expander_get_out1(&out1);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddBoolToObject(root, "in0", in0);
    cJSON_AddBoolToObject(root, "in1", in1);
    cJSON_AddBoolToObject(root, "out0", out0);
    cJSON_AddBoolToObject(root, "out1", out1);

    publish_json(s_topic_io, root, 1, 1);
}

/* pm/<id>/heartbeat — liveness + debug/monitoring fields (QoS0). */
static void publish_heartbeat(void)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }

    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(root, "heap", (double)esp_get_free_heap_size());

    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(root, "fw_version", app != NULL ? app->version : "?");
    cJSON_AddStringToObject(root, "active_broker", s_active_broker);

    network_status_t st;
    if (network_manager_get_status(&st) == ESP_OK) {
        const char *iface = st.active_iface == NETWORK_IFACE_ETH ? "eth"
                          : st.active_iface == NETWORK_IFACE_WIFI_STA ? "wifi" : "none";
        cJSON_AddStringToObject(root, "iface", iface);
        cJSON_AddStringToObject(root, "ip", st.ip);
    }

    publish_json(s_topic_heartbeat, root, 0, 0);
}

static void mqtt_manager_task(void *arg)
{
    /* config_mqtt_t is ~7 KB (3 profiles x 2 KB CA); keep it off the task stack. */
    config_mqtt_t *mqtt = malloc(sizeof(*mqtt));
    if (mqtt == NULL) {
        ESP_LOGE(TAG, "no mem for mqtt config; task idle");
        vTaskDelete(NULL);
        return;
    }
    config_store_get_mqtt(mqtt);

    if (!mqtt->enabled) {
        ESP_LOGI(TAG, "MQTT disabled in config; task idle");
        free(mqtt);
        vTaskDelete(NULL);
        return;
    }
    if (mqtt->active >= CONFIG_STORE_MQTT_PROFILE_COUNT) {
        ESP_LOGW(TAG, "active profile index %u out of range; task idle", (unsigned)mqtt->active);
        free(mqtt);
        vTaskDelete(NULL);
        return;
    }

    build_identity();

    /* Wait for a usable data path before connecting. */
    while (1) {
        network_status_t st;
        if (network_manager_get_status(&st) == ESP_OK && st.has_ip) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    strlcpy(s_active_broker, mqtt->profiles[mqtt->active].name, sizeof(s_active_broker));
    uint32_t publish_period_ms = mqtt->publish_period_ms;

    esp_err_t ret = start_client_for_profile(&mqtt->profiles[mqtt->active], mqtt->keepalive_s);
    free(mqtt);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "MQTT client not started (%s); task idle", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    /*
     * Main loop: publish telemetry on the configured period, and watch for an
     * interface handoff. On an ETH<->STA switch the old TCP socket is dead;
     * esp-mqtt will eventually reconnect, but reconnecting it immediately forces
     * a fresh TCP/TLS handshake on the new default netif without waiting for its
     * timeout. Publishes only happen while connected.
     */
    network_iface_t last_iface = NETWORK_IFACE_NONE;
    {
        network_status_t st;
        if (network_manager_get_status(&st) == ESP_OK) {
            last_iface = st.active_iface;
        }
    }

    int64_t last_publish_us = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));

        network_status_t st;
        if (network_manager_get_status(&st) == ESP_OK && st.active_iface != last_iface) {
            ESP_LOGI(TAG, "active interface changed; reconnecting MQTT on new path");
            if (st.has_ip) {
                esp_mqtt_client_reconnect(s_client);
            }
            last_iface = st.active_iface;
        }

        if (!s_connected) {
            continue;
        }

        int64_t now = esp_timer_get_time();
        if (last_publish_us == 0 || (now - last_publish_us) / 1000 >= publish_period_ms) {
            last_publish_us = now;
            publish_telemetry();
            publish_energy();
            publish_io();
            publish_heartbeat();
        }
    }
}

esp_err_t mqtt_manager_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(mqtt_manager_task, "mqtt_mgr",
                                CONFIG_APP_MQTT_TASK_STACK_SIZE, NULL,
                                CONFIG_APP_MQTT_TASK_PRIORITY, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "create mqtt task failed");

    s_started = true;
    return ESP_OK;
}

bool mqtt_manager_is_connected(void)
{
    return s_connected;
}

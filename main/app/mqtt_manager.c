#include "mqtt_manager.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "cert_store.h"
#include "config_manager.h"
#include "energy_meter_task.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "io_expander.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "sdkconfig.h"
#include "system_status.h"

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

/* Upper bound for one PEM file loaded from the filesystem (Feature 13). A
 * typical CA or client cert is 1-2 KB; a 4 KB RSA key PEM is ~3.2 KB. Anything
 * larger is refused rather than heaped, so a wrong path (e.g. a log file) can
 * never exhaust the heap. */
#define MQTT_PEM_MAX 8192

static const char *TAG = "mqtt_mgr";

static esp_mqtt_client_handle_t s_client;
static TaskHandle_t s_manager_task;
static QueueHandle_t s_apply_queue;
static bool s_started;
static volatile bool s_connected;
static uint32_t s_publish_period_ms = 5000;

typedef struct {
    SemaphoreHandle_t done;
    esp_err_t result;
} mqtt_apply_request_t;

/* device_id used in topics (sanitized device name) and the configured/generated client id. */
static char s_device_id[CONFIG_MANAGER_DEVICE_NAME_LEN];
static char s_client_id[CONFIG_MANAGER_MQTT_CLIENT_ID_LEN];
static char s_topic_status[MQTT_TOPIC_MAX];
static char s_topic_telemetry[MQTT_TOPIC_MAX];
static char s_topic_energy[MQTT_TOPIC_MAX];
static char s_topic_io[MQTT_TOPIC_MAX];
static char s_topic_heartbeat[MQTT_TOPIC_MAX];
static char s_topic_cmd_out0[MQTT_TOPIC_MAX];   /* subscribed: relay out0 control */
static char s_topic_cmd_out1[MQTT_TOPIC_MAX];   /* subscribed: relay out1 control */
static char s_active_broker[CONFIG_MANAGER_MQTT_NAME_LEN];  /* name of connected profile, for heartbeat */

/*
 * PEM buffers loaded from the filesystem (Feature 13). esp-mqtt keeps the
 * pointers handed to it in esp_mqtt_client_config_t and re-reads them on every
 * reconnect handshake, so they remain allocated for the lifetime of the client
 * that references them. Feature 14 frees them only after that client has been
 * stopped and destroyed, then loads a fresh set for the replacement client.
 */
static char *s_tls_ca_pem;
static char *s_tls_cert_pem;
static char *s_tls_key_pem;

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

/* Build device identity, client ID and every runtime topic from the active
 * profile. Empty profile fields preserve the runtime defaults that existed
 * before MQTT Apply. */
static void build_identity(const config_mqtt_profile_t *profile)
{
    /* config_manager_t is ~2.2 KB since Feature 12 (mqtt_profiles[3]); heap it
     * rather than putting it on the caller's stack. */
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        sanitize_device_id("", s_device_id, sizeof(s_device_id));
    } else {
        if (config_manager_get(cfg) == ESP_OK) {
            sanitize_device_id(cfg->device_name, s_device_id, sizeof(s_device_id));
        } else {
            sanitize_device_id("", s_device_id, sizeof(s_device_id));
        }
        free(cfg);
    }

    if (profile->client_id[0] != '\0') {
        strlcpy(s_client_id, profile->client_id, sizeof(s_client_id));
    } else {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(s_client_id, sizeof(s_client_id), "%s-%02X%02X%02X",
                 s_device_id, mac[3], mac[4], mac[5]);
    }

    const char *publish_base = profile->publish_topic[0] != '\0'
                               ? profile->publish_topic : NULL;
    if (publish_base != NULL) {
        snprintf(s_topic_status, sizeof(s_topic_status), "%s/status", publish_base);
        snprintf(s_topic_telemetry, sizeof(s_topic_telemetry), "%s/telemetry", publish_base);
        snprintf(s_topic_energy, sizeof(s_topic_energy), "%s/energy", publish_base);
        snprintf(s_topic_io, sizeof(s_topic_io), "%s/io", publish_base);
        snprintf(s_topic_heartbeat, sizeof(s_topic_heartbeat), "%s/heartbeat", publish_base);
    } else {
        snprintf(s_topic_status, sizeof(s_topic_status), "pm/%s/status", s_device_id);
        snprintf(s_topic_telemetry, sizeof(s_topic_telemetry), "pm/%s/telemetry", s_device_id);
        snprintf(s_topic_energy, sizeof(s_topic_energy), "pm/%s/energy", s_device_id);
        snprintf(s_topic_io, sizeof(s_topic_io), "pm/%s/io", s_device_id);
        snprintf(s_topic_heartbeat, sizeof(s_topic_heartbeat), "pm/%s/heartbeat", s_device_id);
    }

    const char *subscribe_base = profile->subscribe_topic[0] != '\0'
                                 ? profile->subscribe_topic : NULL;
    if (subscribe_base != NULL) {
        snprintf(s_topic_cmd_out0, sizeof(s_topic_cmd_out0), "%s/out0", subscribe_base);
        snprintf(s_topic_cmd_out1, sizeof(s_topic_cmd_out1), "%s/out1", subscribe_base);
    } else {
        snprintf(s_topic_cmd_out0, sizeof(s_topic_cmd_out0), "pm/%s/cmd/out0", s_device_id);
        snprintf(s_topic_cmd_out1, sizeof(s_topic_cmd_out1), "pm/%s/cmd/out1", s_device_id);
    }
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
        system_status_set(SYS_MODULE_MQTT, SYS_STATUS_READY);
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
        system_status_set(SYS_MODULE_MQTT, SYS_STATUS_OFFLINE);
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

/* Human-readable tls_mode, for logs. */
static const char *tls_mode_str(mqtt_tls_mode_t mode)
{
    switch (mode) {
    case MQTT_TLS_DISABLE:  return "DISABLE";
    case MQTT_TLS_CA_ONLY:  return "CA_ONLY";
    case MQTT_TLS_MUTUAL:   return "MUTUAL";
    case MQTT_TLS_INSECURE: return "INSECURE";
    default:                return "?";
    }
}

/*
 * Load one PEM file from the filesystem into a fresh heap buffer (Feature 13).
 * The Configuration Manager only supplies paths — PEM content is never held in
 * the RAM config — so the runtime reads the file itself here.
 *
 * label is used only for logging (e.g. "ca"). On success *out owns a
 * NUL-terminated buffer the caller must keep alive as long as esp-mqtt uses it.
 * Every failure path logs the concrete reason and returns an error; nothing
 * here asserts or aborts.
 */
static esp_err_t load_pem_file(const char *label, const char *path, char **out)
{
    *out = NULL;

    if (path == NULL || path[0] == '\0') {
        ESP_LOGE(TAG, "TLS %s: path is empty in the active profile", label);
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        ESP_LOGE(TAG, "TLS %s: cannot stat \"%s\" (errno=%d); is the filesystem mounted?",
                 label, path, errno);
        return ESP_ERR_NOT_FOUND;
    }
    if (st.st_size <= 0) {
        ESP_LOGE(TAG, "TLS %s: \"%s\" is empty", label, path);
        return ESP_ERR_INVALID_SIZE;
    }
    if (st.st_size > MQTT_PEM_MAX) {
        ESP_LOGE(TAG, "TLS %s: \"%s\" is %ld bytes, over the %d byte limit",
                 label, path, (long)st.st_size, MQTT_PEM_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "TLS %s: cannot open \"%s\" (errno=%d)", label, path, errno);
        return ESP_ERR_NOT_FOUND;
    }

    size_t len = (size_t)st.st_size;
    char *buf = malloc(len + 1);          /* +1 for the NUL esp-tls expects */
    if (buf == NULL) {
        fclose(f);
        ESP_LOGE(TAG, "TLS %s: no memory for %u bytes", label, (unsigned)len);
        return ESP_ERR_NO_MEM;
    }

    size_t got = fread(buf, 1, len, f);
    fclose(f);
    if (got != len) {
        ESP_LOGE(TAG, "TLS %s: short read on \"%s\" (%u of %u bytes)",
                 label, path, (unsigned)got, (unsigned)len);
        free(buf);
        return ESP_FAIL;
    }
    buf[len] = '\0';

    /* Cheap sanity check: esp-tls wants PEM, and a DER/binary or wrong file
     * would otherwise fail deep inside the handshake with an opaque code. */
    if (strstr(buf, "-----BEGIN") == NULL) {
        ESP_LOGE(TAG, "TLS %s: \"%s\" has no PEM header (-----BEGIN); DER is not supported",
                 label, path);
        free(buf);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "TLS %s: loaded %s (%u bytes)", label, path, (unsigned)len);
    *out = buf;
    return ESP_OK;
}

/* Release any PEM buffers loaded for a start-up attempt that then failed. */
static void free_tls_pems(void)
{
    free(s_tls_ca_pem);
    free(s_tls_cert_pem);
    free(s_tls_key_pem);
    s_tls_ca_pem = NULL;
    s_tls_cert_pem = NULL;
    s_tls_key_pem = NULL;
}

/*
 * Apply the profile's tls_mode to the esp-mqtt config (Feature 13).
 *
 *   DISABLE  — nothing to do; the caller already chose the mqtt:// scheme.
 *   CA_ONLY  — verify the broker against the PEM at ca_path.
 *   MUTUAL   — CA_ONLY plus a client cert/key from cert_path/key_path.
 *   INSECURE — TLS with server verification switched off. Debug only.
 *
 * Returns an error if a required file cannot be loaded, so the caller can
 * refuse to connect rather than silently falling back to a weaker mode.
 */
static esp_err_t apply_tls_config(const config_mqtt_profile_t *p, esp_mqtt_client_config_t *cfg)
{
    switch (p->tls_mode) {
    case MQTT_TLS_DISABLE:
        return ESP_OK;

    case MQTT_TLS_CA_ONLY:
        /* No explicit CA: fall back to the ESP-IDF certificate bundle compiled
         * into the image. That covers the public CAs used by hosted brokers
         * without requiring any file on /flash. This is still full server
         * verification, so it is not a silent downgrade — only a different trust
         * anchor set, and the log says which one is in use. */
        if (p->ca_path[0] == '\0') {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
            ESP_LOGI(TAG, "TLS CA_ONLY: no ca_path set, verifying against the "
                          "built-in certificate bundle");
            cfg->broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
            return ESP_OK;
#else
            /* No profile index here to name the exact ca<N>.pem, so point at the
             * portal section that writes it rather than guessing a path. */
            ESP_LOGE(TAG, "TLS CA_ONLY: ca_path is empty and this build has no "
                          "certificate bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE); "
                          "upload a CA for this profile (portal Certs section, under "
                          "%s) or enable the bundle. Refusing to connect.",
                     CERT_STORE_MOUNT_POINT);
            return ESP_ERR_INVALID_STATE;
#endif
        }
        ESP_RETURN_ON_ERROR(load_pem_file("ca", p->ca_path, &s_tls_ca_pem),
                            TAG, "TLS CA_ONLY: CA not usable; refusing to connect");
        cfg->broker.verification.certificate = s_tls_ca_pem;
        return ESP_OK;

    case MQTT_TLS_MUTUAL:
        /* All three must load; a partial set would either fail the handshake
         * or silently degrade to server-only auth. */
        ESP_RETURN_ON_ERROR(load_pem_file("ca", p->ca_path, &s_tls_ca_pem),
                            TAG, "TLS MUTUAL: CA not usable; refusing to connect");
        ESP_RETURN_ON_ERROR(load_pem_file("cert", p->cert_path, &s_tls_cert_pem),
                            TAG, "TLS MUTUAL: client cert not usable; refusing to connect");
        ESP_RETURN_ON_ERROR(load_pem_file("key", p->key_path, &s_tls_key_pem),
                            TAG, "TLS MUTUAL: client key not usable; refusing to connect");
        cfg->broker.verification.certificate = s_tls_ca_pem;
        cfg->credentials.authentication.certificate = s_tls_cert_pem;
        cfg->credentials.authentication.key = s_tls_key_pem;
        return ESP_OK;

    case MQTT_TLS_INSECURE:
#if CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
        ESP_LOGW(TAG, "INSECURE TLS MODE: server certificate verification is DISABLED "
                      "for broker \"%s\" — debug use only, never ship this", p->broker);
        /*
         * Deliberately leave every verification field unset. esp-tls treats
         * "no verification option supplied" as the trigger for its
         * MBEDTLS_SSL_VERIFY_NONE fallback, and that fallback only exists when
         * CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY is enabled (see
         * set_client_config() in esp-tls/esp_tls_mbedtls.c). Setting a CA or the
         * cert bundle here would re-enable verification and defeat the mode.
         * skip_cert_common_name_check additionally drops the CN/hostname match.
         */
        cfg->broker.verification.skip_cert_common_name_check = true;
        return ESP_OK;
#else
        /*
         * Without that Kconfig option esp-tls refuses the handshake with
         * ESP_ERR_MBEDTLS_SSL_SETUP_FAILED ("No server verification option set").
         * Fail here instead, with a reason the log actually explains — and
         * never silently fall back to a verifying mode the operator did not ask
         * for, nor to a connection they think is unverified when it is not.
         */
        ESP_LOGE(TAG, "tls_mode=INSECURE requested for broker \"%s\", but this build has "
                      "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY disabled; esp-tls cannot skip "
                      "verification. Refusing to connect.", p->broker);
        ESP_LOGE(TAG, "To use INSECURE (debug only), enable ESP_TLS_INSECURE and "
                      "ESP_TLS_SKIP_SERVER_CERT_VERIFY in menuconfig; otherwise use CA_ONLY.");
        return ESP_ERR_NOT_SUPPORTED;
#endif

    default:
        ESP_LOGE(TAG, "unknown tls_mode %d in active profile; refusing to connect",
                 (int)p->tls_mode);
        return ESP_ERR_INVALID_ARG;
    }
}

/*
 * Build the esp-mqtt config from a profile and start the client. Returns
 * ESP_ERR_INVALID_STATE (not a fault) if the profile has no broker to connect to,
 * or an error if the profile's TLS material cannot be loaded.
 *
 * Feature 13 wires tls_mode to real behaviour: see apply_tls_config(). Only the
 * construction of esp_mqtt_client_config_t changed — the client lifecycle,
 * event registration and esp-mqtt's own reconnect logic are untouched.
 */
static esp_err_t start_client_for_profile(const config_mqtt_profile_t *p)
{
    if (strlen(p->broker) == 0) {
        ESP_LOGW(TAG, "active profile has no broker; MQTT idle until configured");
        return ESP_ERR_INVALID_STATE;
    }

    bool tls = (p->tls_mode != MQTT_TLS_DISABLE);
    char uri[MQTT_URI_MAX];
    const char *scheme = tls ? "mqtts" : "mqtt";
    snprintf(uri, sizeof(uri), "%s://%s:%u", scheme, p->broker, (unsigned)p->port);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.client_id = s_client_id,
        .session.keepalive = p->keepalive_s,
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

    esp_err_t tls_ret = apply_tls_config(p, &cfg);
    if (tls_ret != ESP_OK) {
        ESP_LOGE(TAG, "TLS setup failed for mode %s (%s); MQTT will not connect",
                 tls_mode_str(p->tls_mode), esp_err_to_name(tls_ret));
        free_tls_pems();
        return tls_ret;
    }

    /* From here on the PEM buffers are referenced by the client config, so every
     * bail-out has to release them (esp-mqtt neither copies nor frees them).
     * ESP_RETURN_ON_* would skip that, hence the explicit error handling. */
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "mqtt client init failed");
        free_tls_pems();
        return ESP_FAIL;
    }

    esp_err_t ret = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                  mqtt_event_handler, NULL);
    if (ret == ESP_OK) {
        ret = esp_mqtt_client_start(s_client);
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mqtt client start failed: %s", esp_err_to_name(ret));
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        free_tls_pems();
        return ret;
    }

    ESP_LOGI(TAG, "client started: %s (tls_mode=%s, client_id=%s)",
             uri, tls_mode_str(p->tls_mode), s_client_id);
    return ESP_OK;
}

/* Stop and permanently retire the current esp-mqtt client. PEM buffers are
 * released only after destroy, because esp-mqtt retains their pointers. */
static esp_err_t destroy_current_client(void)
{
    s_connected = false;
    system_status_set(SYS_MODULE_MQTT, SYS_STATUS_OFFLINE);

    if (s_client == NULL) {
        free_tls_pems();
        return ESP_OK;
    }

    esp_mqtt_client_handle_t old_client = s_client;
    esp_err_t stop_ret = esp_mqtt_client_stop(old_client);
    if (stop_ret != ESP_OK) {
        /* A connected client can fail stop() before its task is terminated if
         * the graceful DISCONNECT packet cannot be created. Request an async
         * disconnect, give the MQTT task one scheduler turn, then retry stop.
         * Never destroy while stop still reports failure: that could free a
         * handle whose task is still running. */
        ESP_LOGW(TAG, "stop old MQTT client failed (%s); disconnecting and retrying",
                 esp_err_to_name(stop_ret));
        esp_err_t disconnect_ret = esp_mqtt_client_disconnect(old_client);
        if (disconnect_ret != ESP_OK) {
            ESP_LOGW(TAG, "disconnect old MQTT client failed: %s",
                     esp_err_to_name(disconnect_ret));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        stop_ret = esp_mqtt_client_stop(old_client);
        if (stop_ret != ESP_OK) {
            /* esp_mqtt_client_destroy() is the authoritative lifecycle endpoint
             * and, by its API contract, stops a running client internally. Log
             * the failed explicit stop but still satisfy Apply's requirement to
             * retire the handle rather than leaving stale runtime state alive. */
            ESP_LOGE(TAG, "explicit stop retry failed (%s); forcing client destroy",
                     esp_err_to_name(stop_ret));
        }
    }

    esp_err_t destroy_ret = esp_mqtt_client_destroy(old_client);
    if (destroy_ret != ESP_OK) {
        ESP_LOGE(TAG, "destroy old MQTT client failed: %s",
                 esp_err_to_name(destroy_ret));
        return destroy_ret;
    }

    s_client = NULL;
    /* stop() waits for the esp-mqtt task to finish, so no old-client event can
     * overwrite these final state values after this point. */
    s_connected = false;
    system_status_set(SYS_MODULE_MQTT, SYS_STATUS_OFFLINE);
    free_tls_pems();
    ESP_LOGI(TAG, "old MQTT client destroyed");
    return ESP_OK;
}

/* Load the Configuration Manager-level MQTT publish period. It is not a
 * per-profile field, but it belongs to the runtime configuration refreshed by
 * MQTT Apply. */
static esp_err_t reload_publish_period(void)
{
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = config_manager_get(cfg);
    if (ret == ESP_OK) {
        s_publish_period_ms = cfg->mqtt_publish_ms;
    }
    free(cfg);
    return ret;
}

/* Feature 14 lifecycle: discard all old esp-mqtt state, read the active RAM
 * profile again, rebuild runtime identity/topics/TLS config, and create a fresh
 * client. This function is called only by mqtt_manager_task, which exclusively
 * owns s_client. */
static esp_err_t recreate_client_from_active_profile(void)
{
    ESP_LOGI(TAG, "applying active MQTT profile");

    esp_err_t ret = destroy_current_client();
    if (ret != ESP_OK) {
        return ret;
    }

    config_mqtt_profile_t profile;
    uint8_t active_index = 0;
    ret = config_manager_get_active_mqtt_profile(&profile, &active_index);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read active MQTT profile failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = reload_publish_period();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "read MQTT publish period failed: %s", esp_err_to_name(ret));
        return ret;
    }

    build_identity(&profile);
    strlcpy(s_active_broker, profile.name, sizeof(s_active_broker));

    if (!profile.enable) {
        ESP_LOGI(TAG, "MQTT disabled in active profile %u; client remains OFFLINE",
                 (unsigned)active_index);
        return ESP_OK;
    }

    ret = start_client_for_profile(&profile);
    if (ret != ESP_OK) {
        s_connected = false;
        system_status_set(SYS_MODULE_MQTT, SYS_STATUS_OFFLINE);
        ESP_LOGE(TAG, "apply MQTT profile %u failed (%s); MQTT remains OFFLINE",
                 (unsigned)active_index, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MQTT profile %u applied with a new client",
             (unsigned)active_index);
    return ESP_OK;
}

static void process_apply_request(mqtt_apply_request_t *request)
{
    if (request == NULL || request->done == NULL) {
        ESP_LOGE(TAG, "invalid MQTT Apply request");
        return;
    }

    request->result = recreate_client_from_active_profile();
    xSemaphoreGive(request->done);
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
    (void)arg;
    bool runtime_configured = false;

    /* Preserve the boot-time policy of waiting for a usable data path before
     * creating the first client. Apply requests are still serviced while there
     * is no IP, so runtime reconfiguration never waits behind this boot gate;
     * esp-mqtt then follows its existing reconnect policy until a path appears. */
    while (1) {
        mqtt_apply_request_t *request = NULL;
        if (xQueueReceive(s_apply_queue, &request, pdMS_TO_TICKS(250)) == pdTRUE) {
            process_apply_request(request);
            runtime_configured = true;
        }

        network_status_t st;
        if (network_manager_get_status(&st) == ESP_OK && st.has_ip) {
            break;
        }
    }

    if (!runtime_configured) {
        esp_err_t ret = recreate_client_from_active_profile();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "initial MQTT client not started (%s); waiting for Apply",
                     esp_err_to_name(ret));
        }
    }

    /*
     * Main loop: publish telemetry on the configured period, service MQTT Apply,
     * and watch for an interface handoff. The 250 ms cadence, publish payloads,
     * and esp-mqtt reconnect policy are unchanged.
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
        mqtt_apply_request_t *request = NULL;
        if (xQueueReceive(s_apply_queue, &request, pdMS_TO_TICKS(250)) == pdTRUE) {
            process_apply_request(request);
            last_publish_us = 0;
        }

        network_status_t st;
        if (network_manager_get_status(&st) == ESP_OK && st.active_iface != last_iface) {
            ESP_LOGI(TAG, "active interface changed; reconnecting MQTT on new path");
            if (st.has_ip && s_client != NULL) {
                esp_mqtt_client_reconnect(s_client);
            }
            last_iface = st.active_iface;
        }

        if (!s_connected || s_client == NULL) {
            continue;
        }

        int64_t now = esp_timer_get_time();
        if (last_publish_us == 0 || (now - last_publish_us) / 1000 >= s_publish_period_ms) {
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

    s_apply_queue = xQueueCreate(1, sizeof(mqtt_apply_request_t *));
    ESP_RETURN_ON_FALSE(s_apply_queue != NULL, ESP_ERR_NO_MEM, TAG,
                        "create MQTT Apply queue failed");

    BaseType_t ok = xTaskCreate(mqtt_manager_task, "mqtt_mgr",
                                CONFIG_APP_MQTT_TASK_STACK_SIZE, NULL,
                                CONFIG_APP_MQTT_TASK_PRIORITY, &s_manager_task);
    if (ok != pdPASS) {
        vQueueDelete(s_apply_queue);
        s_apply_queue = NULL;
        ESP_LOGE(TAG, "create mqtt task failed");
        return ESP_FAIL;
    }

    s_started = true;
    return ESP_OK;
}

esp_err_t mqtt_manager_apply(void)
{
    if (!s_started || s_apply_queue == NULL || s_manager_task == NULL) {
        ESP_LOGE(TAG, "MQTT Apply requested before mqtt_manager_start");
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskGetCurrentTaskHandle() == s_manager_task) {
        ESP_LOGE(TAG, "MQTT Apply cannot synchronously wait in the manager task");
        return ESP_ERR_INVALID_STATE;
    }

    mqtt_apply_request_t request = {
        .done = xSemaphoreCreateBinary(),
        .result = ESP_FAIL,
    };
    if (request.done == NULL) {
        ESP_LOGE(TAG, "create MQTT Apply completion semaphore failed");
        return ESP_ERR_NO_MEM;
    }

    mqtt_apply_request_t *request_ptr = &request;
    if (xQueueSend(s_apply_queue, &request_ptr, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(request.done);
        ESP_LOGE(TAG, "queue MQTT Apply request failed");
        return ESP_FAIL;
    }

    xSemaphoreTake(request.done, portMAX_DELAY);
    vSemaphoreDelete(request.done);
    return request.result;
}

bool mqtt_manager_is_connected(void)
{
    return s_connected;
}

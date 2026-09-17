#include "config_manager.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "config_store.h"
#include "energy_meter_task.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

/*
 * Configuration Manager owns the complete configuration schema and the central
 * RAM snapshot. Config Store owns only persistence transport.
 *
 * Feature 15 persists a private, versioned full-snapshot DTO through Config
 * Store's opaque blob API. Load tries that format first. Existing legacy domain
 * blobs are read only when the full snapshot is absent or incompatible, providing
 * one-time migration; the next explicit save writes only the full snapshot.
 * Save does not Apply, Reload, notify, or modify the RAM snapshot.
 *
 * Large Configuration Manager, MQTT legacy and DTO temporaries are heap-allocated
 * to stay within the main/console task stack budgets.
 */

static const char *TAG = "config_mgr";

static SemaphoreHandle_t s_lock;
static config_manager_t s_cfg;
static bool s_loaded;

#define CONFIG_SNAPSHOT_MAGIC 0x43464753U  /* "CFGS" */
/* Transport layout of the full-snapshot DTO below. v2 rewrites the MQTT region
 * from "3 profiles + active index + duplicate legacy scalars" to one broker
 * struct, so the region's size and offsets change: v1 blobs are rejected rather
 * than partially decoded, and the device starts from defaults. Accepted because
 * the product is not shipped and NVS is re-provisioned. */
#define CONFIG_SNAPSHOT_VERSION 2U

typedef struct {
    uint8_t enable;
    uint8_t tls_mode;
    uint16_t port;
    uint16_t keepalive_s;
    uint16_t reserved;
    char name[CONFIG_MANAGER_MQTT_NAME_LEN];
    char broker[CONFIG_MANAGER_MQTT_BROKER_LEN];
    char username[CONFIG_MANAGER_MQTT_USER_LEN];
    char password[CONFIG_MANAGER_MQTT_PASS_LEN];
    char client_id[CONFIG_MANAGER_MQTT_CLIENT_ID_LEN];
    char publish_topic[CONFIG_MANAGER_MQTT_TOPIC_LEN];
    char subscribe_topic[CONFIG_MANAGER_MQTT_TOPIC_LEN];
    char ca_path[CONFIG_MANAGER_MQTT_PATH_LEN];
    char cert_path[CONFIG_MANAGER_MQTT_PATH_LEN];
    char key_path[CONFIG_MANAGER_MQTT_PATH_LEN];
} config_snapshot_mqtt_profile_t;

/* Private persistence DTO (Feature 15). Config Store transports these bytes as
 * an opaque blob; only Configuration Manager owns this schema and its mapping.
 * Fixed-width scalar fields and an append-only layout let newer firmware load
 * an older prefix while magic/version/declared-size validation remains fail
 * closed. Existing fields must not be reordered or resized. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t total_size;
    uint32_t config_version;

    char device_name[CONFIG_MANAGER_DEVICE_NAME_LEN];
    char firmware_version[CONFIG_MANAGER_VERSION_LEN];
    char hardware_version[CONFIG_MANAGER_VERSION_LEN];

    char ota_fw_build[16];
    char ota_version[16];

    uint8_t dhcp_enable;
    uint8_t mb_slave_id;
    char static_ip[CONFIG_MANAGER_IP_LEN];
    char gateway[CONFIG_MANAGER_IP_LEN];
    char netmask[CONFIG_MANAGER_IP_LEN];
    char dns[CONFIG_MANAGER_IP_LEN];
    char wifi_ssid[CONFIG_MANAGER_SSID_LEN];
    char wifi_pass[CONFIG_MANAGER_PASS_LEN];

    uint32_t mqtt_publish_ms;
    config_snapshot_mqtt_profile_t mqtt;

    uint8_t mb_baud_code;
    uint8_t mb_parity_code;
    uint8_t mb_stop_bits;
    uint8_t line_freq;
    uint8_t wiring_mode;
    uint16_t ct_ratio;
    uint16_t pt_ratio;
    uint8_t lcd_backlight;
    uint8_t buzzer_enable;
    uint8_t lcd_autocycle;
    uint8_t reserved1;
    uint32_t lcd_sleep_timeout_s;
    uint32_t lcd_cycle_time_ms;

    /* Appended after v1 shipped. Fields must only ever be added at the end:
     * a shorter stored blob stays readable (the check below accepts any size
     * between V1 and the current struct) and the missing tail decodes as zero,
     * which the legacy ext_meter overlay then fills in with the real values. */
    uint8_t mb_device;
    uint8_t mb_enabled;
    uint16_t reserved2;
    uint32_t mb_poll_period_ms;
    uint8_t buzzer_alarm_enable;

    /* Alarm Settings (appended; fixed-point values use 0.1 unit). */
    uint8_t alarm_voltage_low_enable;
    uint8_t alarm_voltage_high_enable;
    uint8_t alarm_over_current_enable;
    uint8_t alarm_phase_loss_enable;
    uint8_t alarm_frequency_enable;
    uint8_t alarm_voltage_reference;
    uint8_t alarm_nominal_frequency_hz;
    uint8_t alarm_reserved;
    uint16_t alarm_voltage_low_deci_v;
    uint16_t alarm_voltage_high_deci_v;
    uint16_t alarm_over_current_deci_a;
    uint16_t alarm_frequency_low_deci_hz;
    uint16_t alarm_frequency_high_deci_hz;
    uint16_t alarm_trigger_delay_s;
    uint16_t alarm_clear_delay_s;
    uint16_t alarm_hysteresis_deci;

    /* Multi-slot RTU master devices (append-only). Older blobs lack this tail;
     * load synthesizes slot[0] from legacy mb_device/mb_slave_id/mb_enabled. */
    struct {
        uint8_t used;
        uint8_t enabled;
        uint8_t type;
        uint8_t slave_id;
        char name[CONFIG_MANAGER_MB_NAME_LEN];
    } mb_slots[CONFIG_MANAGER_MB_SLOT_COUNT];

    /* CT setup (append-only). R_BURDEN is board-fixed in Kconfig, not here.
     * Older blobs lack this tail → defaults from Kconfig on load. */
    uint16_t i_rated_a;
    uint16_t i_expected_a;

    /* PGA (append-only after CT params). 1 / 2 / 4. Absent on older blobs →
     * filled from Kconfig default PGA choice on load. */
    uint8_t pga;
    uint8_t pga_reserved[3];

    /* SoftAP portal credentials (append-only). Absent → Kconfig defaults. */
    char ap_ssid[CONFIG_MANAGER_SSID_LEN];
    char ap_pass[CONFIG_MANAGER_PASS_LEN];

    /* This device's own RTU slave baud (append-only). Absent on older blobs,
     * which shared one baud code between the master bus and the slave link:
     * the decoder seeds it from mb_baud_code so an upgrade keeps the live
     * line speed instead of dropping to 9600. */
    uint8_t mb_slave_baud_code;
    uint8_t slave_baud_reserved[3];
} config_snapshot_dto_t;

#define CONFIG_SNAPSHOT_V1_SIZE \
    (offsetof(config_snapshot_dto_t, lcd_sleep_timeout_s) + \
     sizeof(((config_snapshot_dto_t *)0)->lcd_sleep_timeout_s))

static void copy_persisted_string(char *dst, size_t dst_size,
                                  const char *src, size_t src_size)
{
    if (dst_size == 0) {
        return;
    }
    size_t len = strnlen(src, src_size);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static void mqtt_profile_to_dto(config_snapshot_mqtt_profile_t *dst,
                                const config_mqtt_profile_t *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->enable = src->enable ? 1U : 0U;
    dst->tls_mode = (uint8_t)src->tls_mode;
    dst->port = src->port;
    dst->keepalive_s = src->keepalive_s;
    copy_persisted_string(dst->name, sizeof(dst->name), src->name, sizeof(src->name));
    copy_persisted_string(dst->broker, sizeof(dst->broker), src->broker, sizeof(src->broker));
    copy_persisted_string(dst->username, sizeof(dst->username), src->username, sizeof(src->username));
    copy_persisted_string(dst->password, sizeof(dst->password), src->password, sizeof(src->password));
    copy_persisted_string(dst->client_id, sizeof(dst->client_id), src->client_id, sizeof(src->client_id));
    copy_persisted_string(dst->publish_topic, sizeof(dst->publish_topic),
                          src->publish_topic, sizeof(src->publish_topic));
    copy_persisted_string(dst->subscribe_topic, sizeof(dst->subscribe_topic),
                          src->subscribe_topic, sizeof(src->subscribe_topic));
    copy_persisted_string(dst->ca_path, sizeof(dst->ca_path), src->ca_path, sizeof(src->ca_path));
    copy_persisted_string(dst->cert_path, sizeof(dst->cert_path), src->cert_path, sizeof(src->cert_path));
    copy_persisted_string(dst->key_path, sizeof(dst->key_path), src->key_path, sizeof(src->key_path));
}

static void mqtt_profile_from_dto(config_mqtt_profile_t *dst,
                                  const config_snapshot_mqtt_profile_t *src)
{
    memset(dst, 0, sizeof(*dst));
    dst->enable = src->enable != 0;
    dst->tls_mode = (mqtt_tls_mode_t)src->tls_mode;
    dst->port = src->port;
    dst->keepalive_s = src->keepalive_s;
    copy_persisted_string(dst->name, sizeof(dst->name), src->name, sizeof(src->name));
    copy_persisted_string(dst->broker, sizeof(dst->broker), src->broker, sizeof(src->broker));
    copy_persisted_string(dst->username, sizeof(dst->username), src->username, sizeof(src->username));
    copy_persisted_string(dst->password, sizeof(dst->password), src->password, sizeof(src->password));
    copy_persisted_string(dst->client_id, sizeof(dst->client_id), src->client_id, sizeof(src->client_id));
    copy_persisted_string(dst->publish_topic, sizeof(dst->publish_topic),
                          src->publish_topic, sizeof(src->publish_topic));
    copy_persisted_string(dst->subscribe_topic, sizeof(dst->subscribe_topic),
                          src->subscribe_topic, sizeof(src->subscribe_topic));
    copy_persisted_string(dst->ca_path, sizeof(dst->ca_path), src->ca_path, sizeof(src->ca_path));
    copy_persisted_string(dst->cert_path, sizeof(dst->cert_path), src->cert_path, sizeof(src->cert_path));
    copy_persisted_string(dst->key_path, sizeof(dst->key_path), src->key_path, sizeof(src->key_path));
}

static void snapshot_to_dto(config_snapshot_dto_t *dto, const config_manager_t *cfg)
{
    memset(dto, 0, sizeof(*dto));
    dto->magic = CONFIG_SNAPSHOT_MAGIC;
    dto->version = CONFIG_SNAPSHOT_VERSION;
    dto->header_size = (uint16_t)offsetof(config_snapshot_dto_t, device_name);
    dto->total_size = sizeof(*dto);
    dto->config_version = cfg->config_version;

    copy_persisted_string(dto->device_name, sizeof(dto->device_name),
                          cfg->device_name, sizeof(cfg->device_name));
    copy_persisted_string(dto->firmware_version, sizeof(dto->firmware_version),
                          cfg->firmware_version, sizeof(cfg->firmware_version));
    copy_persisted_string(dto->hardware_version, sizeof(dto->hardware_version),
                          cfg->hardware_version, sizeof(cfg->hardware_version));
    copy_persisted_string(dto->ota_fw_build, sizeof(dto->ota_fw_build),
                          cfg->ota_fw_build, sizeof(cfg->ota_fw_build));
    copy_persisted_string(dto->ota_version, sizeof(dto->ota_version),
                          cfg->ota_version, sizeof(cfg->ota_version));
    dto->dhcp_enable = cfg->dhcp_enable ? 1U : 0U;
    copy_persisted_string(dto->static_ip, sizeof(dto->static_ip), cfg->static_ip, sizeof(cfg->static_ip));
    copy_persisted_string(dto->gateway, sizeof(dto->gateway), cfg->gateway, sizeof(cfg->gateway));
    copy_persisted_string(dto->netmask, sizeof(dto->netmask), cfg->netmask, sizeof(cfg->netmask));
    copy_persisted_string(dto->dns, sizeof(dto->dns), cfg->dns, sizeof(cfg->dns));
    copy_persisted_string(dto->wifi_ssid, sizeof(dto->wifi_ssid), cfg->wifi_ssid, sizeof(cfg->wifi_ssid));
    copy_persisted_string(dto->wifi_pass, sizeof(dto->wifi_pass), cfg->wifi_pass, sizeof(cfg->wifi_pass));

    dto->mqtt_publish_ms = cfg->mqtt_publish_ms;
    mqtt_profile_to_dto(&dto->mqtt, &cfg->mqtt);

    dto->mb_slave_id = cfg->mb_slave_id;
    dto->mb_baud_code = cfg->mb_baud_code;
    dto->mb_parity_code = cfg->mb_parity_code;
    dto->mb_stop_bits = cfg->mb_stop_bits;
    dto->line_freq = cfg->line_freq;
    dto->ct_ratio = cfg->ct_ratio;
    dto->pt_ratio = cfg->pt_ratio;
    dto->wiring_mode = cfg->wiring_mode;
    dto->i_rated_a = cfg->i_rated_a;
    dto->i_expected_a = cfg->i_expected_a;
    dto->pga = cfg->pga;
    copy_persisted_string(dto->ap_ssid, sizeof(dto->ap_ssid), cfg->ap_ssid, sizeof(cfg->ap_ssid));
    copy_persisted_string(dto->ap_pass, sizeof(dto->ap_pass), cfg->ap_pass, sizeof(cfg->ap_pass));
    dto->mb_slave_baud_code = cfg->mb_slave_baud_code;
    dto->lcd_backlight = cfg->lcd_backlight ? 1U : 0U;
    dto->lcd_sleep_timeout_s = cfg->lcd_sleep_timeout_s;
    dto->lcd_autocycle = cfg->lcd_autocycle ? 1U : 0U;
    dto->lcd_cycle_time_ms = cfg->lcd_cycle_time_ms;
    dto->buzzer_enable = cfg->buzzer_enable ? 1U : 0U;

    dto->mb_device = cfg->mb_device;
    dto->mb_enabled = cfg->mb_enabled ? 1U : 0U;
    dto->mb_poll_period_ms = cfg->mb_poll_period_ms;
    for (size_t i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
        dto->mb_slots[i].used = cfg->mb_slots[i].used ? 1U : 0U;
        dto->mb_slots[i].enabled = cfg->mb_slots[i].enabled ? 1U : 0U;
        dto->mb_slots[i].type = cfg->mb_slots[i].type;
        dto->mb_slots[i].slave_id = cfg->mb_slots[i].slave_id;
        copy_persisted_string(dto->mb_slots[i].name, sizeof(dto->mb_slots[i].name),
                              cfg->mb_slots[i].name, sizeof(cfg->mb_slots[i].name));
    }
    dto->buzzer_alarm_enable = cfg->buzzer_alarm_enable ? 1U : 0U;
    dto->alarm_voltage_low_enable = cfg->alarm_voltage_low_enable ? 1U : 0U;
    dto->alarm_voltage_high_enable = cfg->alarm_voltage_high_enable ? 1U : 0U;
    dto->alarm_over_current_enable = cfg->alarm_over_current_enable ? 1U : 0U;
    dto->alarm_phase_loss_enable = cfg->alarm_phase_loss_enable ? 1U : 0U;
    dto->alarm_frequency_enable = cfg->alarm_frequency_enable ? 1U : 0U;
    dto->alarm_voltage_reference = cfg->alarm_voltage_reference;
    dto->alarm_nominal_frequency_hz = cfg->alarm_nominal_frequency_hz;
    dto->alarm_voltage_low_deci_v = (uint16_t)(cfg->alarm_voltage_low_v * 10.0f + 0.5f);
    dto->alarm_voltage_high_deci_v = (uint16_t)(cfg->alarm_voltage_high_v * 10.0f + 0.5f);
    dto->alarm_over_current_deci_a = (uint16_t)(cfg->alarm_over_current_a * 10.0f + 0.5f);
    dto->alarm_frequency_low_deci_hz = (uint16_t)(cfg->alarm_frequency_low_hz * 10.0f + 0.5f);
    dto->alarm_frequency_high_deci_hz = (uint16_t)(cfg->alarm_frequency_high_hz * 10.0f + 0.5f);
    dto->alarm_trigger_delay_s = cfg->alarm_trigger_delay_s;
    dto->alarm_clear_delay_s = cfg->alarm_clear_delay_s;
    dto->alarm_hysteresis_deci = (uint16_t)(cfg->alarm_hysteresis * 10.0f + 0.5f);
}

static esp_err_t snapshot_from_dto(config_manager_t *cfg, const config_snapshot_dto_t *dto,
                                   size_t stored_size)
{
    if (dto->magic != CONFIG_SNAPSHOT_MAGIC ||
        dto->version != CONFIG_SNAPSHOT_VERSION ||
        dto->header_size != offsetof(config_snapshot_dto_t, device_name) ||
        dto->total_size != stored_size ||
        stored_size < CONFIG_SNAPSHOT_V1_SIZE ||
        stored_size > sizeof(*dto)) {
        return ESP_ERR_INVALID_VERSION;
    }
    if (dto->mqtt.enable > 1U || dto->mqtt.tls_mode > MQTT_TLS_INSECURE ||
        dto->dhcp_enable > 1U ||
        dto->lcd_backlight > 1U || dto->buzzer_enable > 1U ||
        dto->lcd_autocycle > 1U ||
        dto->line_freq > 1U ||
        dto->wiring_mode > 1U) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Above the maximum is corruption — reject outright. Below the minimum is
     * usually a legacy snapshot from before the 5 s floor, which must still
     * load; normalize_loaded_periods() clamps it after the legacy-domain
     * overlay, and config_manager_update() rejects new saves outside the range
     * so a frontend bug surfaces instead of silently persisting. */
    if (dto->mqtt_publish_ms > CONFIG_MANAGER_MQTT_PERIOD_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Appended-field guard: a blob written by older firmware stops before these,
     * and the caller's buffer past stored_size holds uninitialized malloc bytes,
     * so they must not be read. Absent -> left at zero here; the legacy
     * ext_meter overlay supplies the real values right after decoding. */
    bool has_mb_runtime =
        stored_size >= (offsetof(config_snapshot_dto_t, mb_poll_period_ms) +
                        sizeof(dto->mb_poll_period_ms));
    bool has_buzzer_alarm =
        stored_size >= (offsetof(config_snapshot_dto_t, buzzer_alarm_enable) +
                        sizeof(dto->buzzer_alarm_enable));
    bool has_alarm =
        stored_size >= (offsetof(config_snapshot_dto_t, alarm_hysteresis_deci) +
                        sizeof(dto->alarm_hysteresis_deci));
    bool has_mb_slots =
        stored_size >= (offsetof(config_snapshot_dto_t, mb_slots) +
                        sizeof(dto->mb_slots));
    bool has_ct_params =
        stored_size >= (offsetof(config_snapshot_dto_t, i_expected_a) +
                        sizeof(dto->i_expected_a));
    bool has_pga =
        stored_size >= (offsetof(config_snapshot_dto_t, pga) +
                        sizeof(dto->pga));
    bool has_ap_creds =
        stored_size >= (offsetof(config_snapshot_dto_t, ap_pass) +
                        sizeof(dto->ap_pass));
    bool has_slave_baud =
        stored_size >= (offsetof(config_snapshot_dto_t, mb_slave_baud_code) +
                        sizeof(dto->mb_slave_baud_code));
    if (has_mb_runtime && dto->mb_enabled > 1U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (has_slave_baud && dto->mb_slave_baud_code > 4U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (has_buzzer_alarm && dto->buzzer_alarm_enable > 1U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (has_alarm &&
        (dto->alarm_voltage_low_enable > 1U ||
         dto->alarm_voltage_high_enable > 1U ||
         dto->alarm_over_current_enable > 1U ||
         dto->alarm_phase_loss_enable > 1U ||
         dto->alarm_frequency_enable > 1U ||
         dto->alarm_voltage_reference > 2U ||
         (dto->alarm_nominal_frequency_hz != 50U &&
          dto->alarm_nominal_frequency_hz != 60U))) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->config_version = CONFIG_MANAGER_VERSION;
    copy_persisted_string(cfg->device_name, sizeof(cfg->device_name),
                          dto->device_name, sizeof(dto->device_name));
    copy_persisted_string(cfg->firmware_version, sizeof(cfg->firmware_version),
                          dto->firmware_version, sizeof(dto->firmware_version));
    copy_persisted_string(cfg->hardware_version, sizeof(cfg->hardware_version),
                          dto->hardware_version, sizeof(dto->hardware_version));
    copy_persisted_string(cfg->ota_fw_build, sizeof(cfg->ota_fw_build),
                          dto->ota_fw_build, sizeof(dto->ota_fw_build));
    copy_persisted_string(cfg->ota_version, sizeof(cfg->ota_version),
                          dto->ota_version, sizeof(dto->ota_version));
    cfg->dhcp_enable = dto->dhcp_enable != 0;
    copy_persisted_string(cfg->static_ip, sizeof(cfg->static_ip), dto->static_ip, sizeof(dto->static_ip));
    copy_persisted_string(cfg->gateway, sizeof(cfg->gateway), dto->gateway, sizeof(dto->gateway));
    copy_persisted_string(cfg->netmask, sizeof(cfg->netmask), dto->netmask, sizeof(dto->netmask));
    copy_persisted_string(cfg->dns, sizeof(cfg->dns), dto->dns, sizeof(dto->dns));
    copy_persisted_string(cfg->wifi_ssid, sizeof(cfg->wifi_ssid), dto->wifi_ssid, sizeof(dto->wifi_ssid));
    copy_persisted_string(cfg->wifi_pass, sizeof(cfg->wifi_pass), dto->wifi_pass, sizeof(dto->wifi_pass));

    cfg->mqtt_publish_ms = dto->mqtt_publish_ms;
    mqtt_profile_from_dto(&cfg->mqtt, &dto->mqtt);

    cfg->mb_slave_id = dto->mb_slave_id;
    cfg->mb_baud_code = dto->mb_baud_code;
    cfg->mb_parity_code = dto->mb_parity_code;
    cfg->mb_stop_bits = dto->mb_stop_bits;
    /* The slave link used to share mb_baud_code with the master bus. Blobs from
     * that era carry no slave-specific byte: seed it from the (single) stored
     * baud so an upgraded device keeps answering the SCADA at the speed it is
     * answering at right now, instead of silently dropping to 9600. */
    cfg->mb_slave_baud_code = has_slave_baud ? dto->mb_slave_baud_code
                                             : dto->mb_baud_code;
    cfg->line_freq = dto->line_freq;
    cfg->wiring_mode = dto->wiring_mode;
    cfg->ct_ratio = dto->ct_ratio ? dto->ct_ratio : 1U;
    cfg->pt_ratio = dto->pt_ratio ? dto->pt_ratio : 1U;
    if (has_ct_params) {
        cfg->i_rated_a = dto->i_rated_a ? dto->i_rated_a : 1U;
        cfg->i_expected_a = dto->i_expected_a ? dto->i_expected_a : 1U;
    } else {
#if defined(CONFIG_APP_ATM90E32AS_I_RATED_A)
        cfg->i_rated_a = (uint16_t)CONFIG_APP_ATM90E32AS_I_RATED_A;
#else
        cfg->i_rated_a = 100U;
#endif
#if defined(CONFIG_APP_ATM90E32AS_I_EXPECTED_A)
        cfg->i_expected_a = (uint16_t)CONFIG_APP_ATM90E32AS_I_EXPECTED_A;
#else
        cfg->i_expected_a = 75U;
#endif
        /* Legacy blobs used ct_ratio as reserved=1; prefer Kconfig NCT. */
#if defined(CONFIG_APP_ATM90E32AS_CT_RATIO)
        if (cfg->ct_ratio <= 1U) {
            cfg->ct_ratio = (uint16_t)CONFIG_APP_ATM90E32AS_CT_RATIO;
        }
#endif
    }
    /* Product NCT window: 1000..6000, step 100. Snap legacy/out-of-range. */
    {
        const uint16_t nct_min = 1000U;
        const uint16_t nct_max = 6000U;
        const uint16_t nct_step = 100U;
        uint16_t nct = cfg->ct_ratio;
        if (nct < nct_min) {
#if defined(CONFIG_APP_ATM90E32AS_CT_RATIO)
            nct = (uint16_t)CONFIG_APP_ATM90E32AS_CT_RATIO;
#else
            nct = 2000U;
#endif
        } else if (nct > nct_max) {
            nct = nct_max;
        }
        if ((nct % nct_step) != 0U) {
            nct = (uint16_t)(((nct + (nct_step / 2U)) / nct_step) * nct_step);
            if (nct < nct_min) {
                nct = nct_min;
            }
            if (nct > nct_max) {
                nct = nct_max;
            }
        }
        cfg->ct_ratio = nct;
    }
    if (has_pga && (dto->pga == 1U || dto->pga == 2U || dto->pga == 4U)) {
        cfg->pga = dto->pga;
    } else if (has_pga) {
        /* Corrupt value in a v5+ blob — fall back to Kconfig. */
#if defined(CONFIG_APP_ATM90E32AS_DEFAULT_PGA_4X)
        cfg->pga = 4U;
#elif defined(CONFIG_APP_ATM90E32AS_DEFAULT_PGA_2X)
        cfg->pga = 2U;
#else
        cfg->pga = 1U;
#endif
    } else {
        /* Older snapshot without PGA tail. 0 = unset → energy_meter migrates
         * once from calib NVS (or Kconfig) then persists. */
        cfg->pga = 0U;
    }
    if (has_ap_creds) {
        copy_persisted_string(cfg->ap_ssid, sizeof(cfg->ap_ssid),
                              dto->ap_ssid, sizeof(dto->ap_ssid));
        copy_persisted_string(cfg->ap_pass, sizeof(cfg->ap_pass),
                              dto->ap_pass, sizeof(dto->ap_pass));
    } else {
        cfg->ap_ssid[0] = '\0';
        cfg->ap_pass[0] = '\0';
    }
    if (cfg->ap_ssid[0] == '\0') {
#if defined(CONFIG_APP_WIFI_METER_AP_SSID)
        copy_persisted_string(cfg->ap_ssid, sizeof(cfg->ap_ssid),
                              CONFIG_APP_WIFI_METER_AP_SSID,
                              strlen(CONFIG_APP_WIFI_METER_AP_SSID) + 1U);
#else
        copy_persisted_string(cfg->ap_ssid, sizeof(cfg->ap_ssid),
                              "PowerMeter-AP", sizeof("PowerMeter-AP"));
#endif
    }
    if (!has_ap_creds) {
#if defined(CONFIG_APP_WIFI_METER_AP_PASSWORD)
        copy_persisted_string(cfg->ap_pass, sizeof(cfg->ap_pass),
                              CONFIG_APP_WIFI_METER_AP_PASSWORD,
                              strlen(CONFIG_APP_WIFI_METER_AP_PASSWORD) + 1U);
#else
        copy_persisted_string(cfg->ap_pass, sizeof(cfg->ap_pass),
                              "12345678", sizeof("12345678"));
#endif
    }
    cfg->lcd_backlight = dto->lcd_backlight != 0;
    cfg->lcd_sleep_timeout_s = dto->lcd_sleep_timeout_s;
    cfg->lcd_autocycle = dto->lcd_autocycle != 0;
    cfg->lcd_cycle_time_ms = dto->lcd_cycle_time_ms;
    cfg->buzzer_enable = dto->buzzer_enable != 0;
    cfg->buzzer_alarm_enable = has_buzzer_alarm ? (dto->buzzer_alarm_enable != 0)
                                                  : cfg->buzzer_enable;
    if (has_alarm) {
        cfg->alarm_voltage_low_enable = dto->alarm_voltage_low_enable != 0;
        cfg->alarm_voltage_high_enable = dto->alarm_voltage_high_enable != 0;
        cfg->alarm_over_current_enable = dto->alarm_over_current_enable != 0;
        cfg->alarm_phase_loss_enable = dto->alarm_phase_loss_enable != 0;
        cfg->alarm_frequency_enable = dto->alarm_frequency_enable != 0;
        cfg->alarm_voltage_reference = dto->alarm_voltage_reference;
        cfg->alarm_nominal_frequency_hz = dto->alarm_nominal_frequency_hz;
        cfg->alarm_voltage_low_v = dto->alarm_voltage_low_deci_v / 10.0f;
        cfg->alarm_voltage_high_v = dto->alarm_voltage_high_deci_v / 10.0f;
        cfg->alarm_over_current_a = dto->alarm_over_current_deci_a / 10.0f;
        cfg->alarm_frequency_low_hz = dto->alarm_frequency_low_deci_hz / 10.0f;
        cfg->alarm_frequency_high_hz = dto->alarm_frequency_high_deci_hz / 10.0f;
        cfg->alarm_trigger_delay_s = dto->alarm_trigger_delay_s;
        cfg->alarm_clear_delay_s = dto->alarm_clear_delay_s;
        cfg->alarm_hysteresis = dto->alarm_hysteresis_deci / 10.0f;
    } else {
        cfg->alarm_voltage_low_enable = true;
        cfg->alarm_voltage_high_enable = true;
        cfg->alarm_over_current_enable = true;
        cfg->alarm_phase_loss_enable = true;
        cfg->alarm_frequency_enable = true;
        cfg->alarm_voltage_reference = 0;
        cfg->alarm_nominal_frequency_hz = cfg->line_freq ? 60U : 50U;
        cfg->alarm_voltage_low_v = 180.0f;
        cfg->alarm_voltage_high_v = 250.0f;
        cfg->alarm_over_current_a = 10.0f;
        cfg->alarm_frequency_low_hz = cfg->line_freq ? 57.0f : 47.0f;
        cfg->alarm_frequency_high_hz = cfg->line_freq ? 63.0f : 53.0f;
        cfg->alarm_trigger_delay_s = 2U;
        cfg->alarm_clear_delay_s = 2U;
        cfg->alarm_hysteresis = 5.0f;
    }

    if (has_mb_runtime) {
        cfg->mb_device = dto->mb_device;
        cfg->mb_enabled = dto->mb_enabled != 0;
        cfg->mb_poll_period_ms = dto->mb_poll_period_ms;
    }

    /* Multi-slot list: prefer persisted tail; else synthesize slot 0 from the
     * legacy single-device fields so upgrades keep the previous meter. */
    memset(cfg->mb_slots, 0, sizeof(cfg->mb_slots));
    if (has_mb_slots) {
        for (size_t i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
            if (dto->mb_slots[i].used > 1U || dto->mb_slots[i].enabled > 1U ||
                dto->mb_slots[i].type >= 2U /* METER_DEV_COUNT */) {
                return ESP_ERR_INVALID_ARG;
            }
            cfg->mb_slots[i].used = dto->mb_slots[i].used != 0;
            cfg->mb_slots[i].enabled = dto->mb_slots[i].enabled != 0;
            cfg->mb_slots[i].type = dto->mb_slots[i].type;
            cfg->mb_slots[i].slave_id = dto->mb_slots[i].slave_id;
            copy_persisted_string(cfg->mb_slots[i].name, sizeof(cfg->mb_slots[i].name),
                                  dto->mb_slots[i].name, sizeof(dto->mb_slots[i].name));
        }
    } else if (has_mb_runtime && (cfg->mb_slave_id != 0 || cfg->mb_enabled)) {
        cfg->mb_slots[0].used = true;
        cfg->mb_slots[0].enabled = cfg->mb_enabled;
        cfg->mb_slots[0].type = cfg->mb_device;
        cfg->mb_slots[0].slave_id = cfg->mb_slave_id ? cfg->mb_slave_id : 1U;
        strlcpy(cfg->mb_slots[0].name,
                cfg->mb_device == 1U ? "EM-07K" : "PM710",
                sizeof(cfg->mb_slots[0].name));
    }

    /* Keep the legacy type mirror aligned with the first used slot (register
     * map / UI readers). Only mb_device is mirrored: mb_slave_id is this
     * device's own RTU slave address (modbus_slave_task is its single
     * consumer) — overwriting it with a downstream meter's address made the
     * slave lose its saved address on every reboot that had a used slot. */
    for (size_t i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
        if (cfg->mb_slots[i].used) {
            cfg->mb_device = cfg->mb_slots[i].type;
            break;
        }
    }

    /* Firmware version describes the running image, not the image that saved
     * the snapshot. Refresh it after decoding while every configuration field
     * still round-trips through the DTO. */
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc != NULL) {
        strlcpy(cfg->firmware_version, desc->version, sizeof(cfg->firmware_version));
    }
    return ESP_OK;
}

/* Fill the broker struct with its factory state: switched off, TLS off, no
 * address. The label/credentials stay empty until the operator fills the MQTT
 * section of the web portal. */
static void mqtt_broker_default(config_mqtt_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    p->port = 1883;          /* Default non-TLS port */
    p->keepalive_s = 60;     /* MQTT standard default: 60 seconds */
    p->tls_mode = MQTT_TLS_DISABLE;
}

/*
 * The legacy-owned field values as of the last load or save. Saving needs to
 * tell "the Configuration Manager changed this field" from "a legacy writer
 * (console net-cfg, web network/system form) changed it", and comparing both
 * sides against this shadow is what makes that possible.
 */
typedef struct {
    bool valid;
    char device_name[CONFIG_MANAGER_DEVICE_NAME_LEN];
    bool dhcp_enable;
    char static_ip[CONFIG_MANAGER_IP_LEN];
    char gateway[CONFIG_MANAGER_IP_LEN];
    char netmask[CONFIG_MANAGER_IP_LEN];
    char dns[CONFIG_MANAGER_IP_LEN];
    char wifi_ssid[CONFIG_MANAGER_SSID_LEN];
    char wifi_pass[CONFIG_MANAGER_PASS_LEN];
    uint8_t mb_baud_code;
    uint8_t mb_parity_code;
    uint8_t mb_device;
    bool mb_enabled;
    uint32_t mb_poll_period_ms;
} legacy_shadow_t;

static legacy_shadow_t s_legacy;

static void legacy_shadow_store(const config_manager_t *c)
{
    strlcpy(s_legacy.device_name, c->device_name, sizeof(s_legacy.device_name));
    s_legacy.dhcp_enable = c->dhcp_enable;
    strlcpy(s_legacy.static_ip, c->static_ip, sizeof(s_legacy.static_ip));
    strlcpy(s_legacy.gateway, c->gateway, sizeof(s_legacy.gateway));
    strlcpy(s_legacy.netmask, c->netmask, sizeof(s_legacy.netmask));
    strlcpy(s_legacy.dns, c->dns, sizeof(s_legacy.dns));
    strlcpy(s_legacy.wifi_ssid, c->wifi_ssid, sizeof(s_legacy.wifi_ssid));
    strlcpy(s_legacy.wifi_pass, c->wifi_pass, sizeof(s_legacy.wifi_pass));
    s_legacy.mb_baud_code = c->mb_baud_code;
    s_legacy.mb_parity_code = c->mb_parity_code;
    s_legacy.mb_device = c->mb_device;
    s_legacy.mb_enabled = c->mb_enabled;
    s_legacy.mb_poll_period_ms = c->mb_poll_period_ms;
    s_legacy.valid = true;
}

/* Keep fields still owned by legacy runtime domains synchronized with the full
 * snapshot. A missing/invalid legacy blob must not replace a valid snapshot
 * value with a compile-time default. Remove each domain from this overlay when
 * that domain migrates fully to Configuration Manager ownership. */
static void snapshot_overlay_legacy_domains(config_manager_t *c)
{
    config_system_t sys;
    if (config_store_get_system(&sys) == ESP_OK) {
        strlcpy(c->device_name, sys.device_name, sizeof(c->device_name));
    }

    config_network_t net;
    if (config_store_get_network(&net) == ESP_OK) {
        c->dhcp_enable = net.eth_dhcp;
        strlcpy(c->static_ip, net.static_ip, sizeof(c->static_ip));
        strlcpy(c->gateway, net.gateway, sizeof(c->gateway));
        strlcpy(c->netmask, net.netmask, sizeof(c->netmask));
        strlcpy(c->dns, net.dns, sizeof(c->dns));
        strlcpy(c->wifi_ssid, net.wifi_ssid, sizeof(c->wifi_ssid));
        strlcpy(c->wifi_pass, net.wifi_pass, sizeof(c->wifi_pass));
    }

    config_ext_meter_t ext;
    if (config_store_get_ext_meter(&ext) == ESP_OK) {
        /* ext.slave_addr is the LEGACY single-device master field: the address
         * of the downstream meter the master polled before slots existed
         * (config_store.c seeds it that way too). It is NOT this device's own
         * Modbus unit id, so it must not land in mb_slave_id, which now belongs
         * to the RTU slave link alone. */
        c->mb_baud_code = ext.baud_code;
        c->mb_parity_code = ext.parity_code;
        c->mb_device = ext.device;
        c->mb_enabled = ext.enabled;
        c->mb_poll_period_ms = ext.poll_period_ms;
    }
}

/* Save direction of the same legacy overlay. Without this, a field that only the
 * Configuration Manager changed (for example through data_point_write) would be
 * overwritten again by the legacy blob on the next load, so the
 * write -> save -> reboot -> read chain could never complete. Pushing here keeps
 * both copies equal, which makes the load overlay a no-op for those fields while
 * still letting a legacy writer that ran after the last save win.
 *
 * Three-way merge against the shadow, per field: if the legacy blob moved away
 * from the shadow, a legacy writer ran last and wins (same result as the load
 * overlay). Otherwise, if the snapshot moved, the Configuration Manager changed
 * it and the value is pushed down into the legacy blob. Fields the Configuration
 * Manager does not own (hostname, network mode, ap_pass) are read-modify-written,
 * so they are preserved.
 *
 * A getter reporting ESP_ERR_NVS_NOT_FOUND has still filled the struct with
 * defaults, so that case is safe to merge; a real NVS fault skips the domain. */
static bool legacy_getter_usable(esp_err_t ret)
{
    return ret == ESP_OK || ret == ESP_ERR_NVS_NOT_FOUND;
}

/* Returns true when the legacy blob was modified and needs writing back. */
static bool merge_str(char *cm, size_t cm_size, char *legacy, size_t legacy_size,
                      const char *shadow)
{
    if (strcmp(legacy, shadow) != 0) {
        strlcpy(cm, legacy, cm_size);   /* legacy writer wins */
        return false;
    }
    if (strcmp(cm, shadow) != 0) {
        strlcpy(legacy, cm, legacy_size);
        return true;
    }
    return false;
}

static bool merge_u8(uint8_t *cm, uint8_t *legacy, uint8_t shadow)
{
    if (*legacy != shadow) {
        *cm = *legacy;
        return false;
    }
    if (*cm != shadow) {
        *legacy = *cm;
        return true;
    }
    return false;
}

static bool merge_u32(uint32_t *cm, uint32_t *legacy, uint32_t shadow)
{
    if (*legacy != shadow) {
        *cm = *legacy;
        return false;
    }
    if (*cm != shadow) {
        *legacy = *cm;
        return true;
    }
    return false;
}

static bool merge_bool(bool *cm, bool *legacy, bool shadow)
{
    if (*legacy != shadow) {
        *cm = *legacy;
        return false;
    }
    if (*cm != shadow) {
        *legacy = *cm;
        return true;
    }
    return false;
}

static void snapshot_merge_legacy_domains(config_manager_t *c)
{
    if (!s_legacy.valid) {
        /* No reference point yet: fall back to letting the stored blobs win. */
        snapshot_overlay_legacy_domains(c);
        return;
    }

    config_system_t sys;
    if (legacy_getter_usable(config_store_get_system(&sys))) {
        if (merge_str(c->device_name, sizeof(c->device_name),
                      sys.device_name, sizeof(sys.device_name), s_legacy.device_name) &&
            config_store_set_system(&sys) != ESP_OK) {
            ESP_LOGW(TAG, "sync device_name to legacy system domain failed");
        }
    }

    config_network_t net;
    if (legacy_getter_usable(config_store_get_network(&net))) {
        bool dirty = false;
        dirty |= merge_bool(&c->dhcp_enable, &net.eth_dhcp, s_legacy.dhcp_enable);
        dirty |= merge_str(c->static_ip, sizeof(c->static_ip),
                           net.static_ip, sizeof(net.static_ip), s_legacy.static_ip);
        dirty |= merge_str(c->gateway, sizeof(c->gateway),
                           net.gateway, sizeof(net.gateway), s_legacy.gateway);
        dirty |= merge_str(c->netmask, sizeof(c->netmask),
                           net.netmask, sizeof(net.netmask), s_legacy.netmask);
        dirty |= merge_str(c->dns, sizeof(c->dns),
                           net.dns, sizeof(net.dns), s_legacy.dns);
        dirty |= merge_str(c->wifi_ssid, sizeof(c->wifi_ssid),
                           net.wifi_ssid, sizeof(net.wifi_ssid), s_legacy.wifi_ssid);
        dirty |= merge_str(c->wifi_pass, sizeof(c->wifi_pass),
                           net.wifi_pass, sizeof(net.wifi_pass), s_legacy.wifi_pass);
        if (dirty && config_store_set_network(&net) != ESP_OK) {
            ESP_LOGW(TAG, "sync network fields to legacy network domain failed");
        }
    }

    config_ext_meter_t ext;
    if (legacy_getter_usable(config_store_get_ext_meter(&ext))) {
        bool dirty = false;
        /* ext.slave_addr (legacy master-domain downstream address) is not
         * merged with mb_slave_id (this device's slave address) — different
         * meanings, no cross-write in either direction. */
        dirty |= merge_u8(&c->mb_baud_code, &ext.baud_code, s_legacy.mb_baud_code);
        dirty |= merge_u8(&c->mb_parity_code, &ext.parity_code, s_legacy.mb_parity_code);
        dirty |= merge_u8(&c->mb_device, &ext.device, s_legacy.mb_device);
        dirty |= merge_bool(&c->mb_enabled, &ext.enabled, s_legacy.mb_enabled);
        dirty |= merge_u32(&c->mb_poll_period_ms, &ext.poll_period_ms, s_legacy.mb_poll_period_ms);
        if (dirty && config_store_set_ext_meter(&ext) != ESP_OK) {
            ESP_LOGW(TAG, "sync Modbus fields to legacy meter domain failed");
        }
    }
}

/* Build a snapshot from the config_store domains + read-only sources. Fills
 * reserved fields with sensible defaults (no hardware/driver source yet). */
static esp_err_t snapshot_from_store(config_manager_t *c)
{
    memset(c, 0, sizeof(*c));
    c->config_version = CONFIG_MANAGER_VERSION;

    /* System */
    config_system_t sys;
    config_store_get_system(&sys);
    strlcpy(c->device_name, sys.device_name, sizeof(c->device_name));

    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc != NULL) {
        strlcpy(c->firmware_version, desc->version, sizeof(c->firmware_version));
    }
    strlcpy(c->hardware_version, "1.0", sizeof(c->hardware_version));

    /* OTA tracking: default values before any OTA update */
    strlcpy(c->ota_fw_build, "000000-00", sizeof(c->ota_fw_build));
    strlcpy(c->ota_version, "0.0.0", sizeof(c->ota_version));

    /* Network + WiFi */
    config_network_t net;
    config_store_get_network(&net);
    c->dhcp_enable = net.eth_dhcp;
    strlcpy(c->static_ip, net.static_ip, sizeof(c->static_ip));
    strlcpy(c->gateway, net.gateway, sizeof(c->gateway));
    strlcpy(c->netmask, net.netmask, sizeof(c->netmask));
    strlcpy(c->dns, net.dns, sizeof(c->dns));
    strlcpy(c->wifi_ssid, net.wifi_ssid, sizeof(c->wifi_ssid));
    strlcpy(c->wifi_pass, net.wifi_pass, sizeof(c->wifi_pass));

    /* MQTT: one broker, factory-off. The legacy config_store mqtt domain is
     * NOT read here anymore: it stored 3 profiles + PEM blobs with no
     * counterpart in this snapshot (v7 dropped the model), and the device is
     * re-provisioned from an empty NVS — the broker comes from the web portal,
     * it is not migrated. */
    mqtt_broker_default(&c->mqtt);
    c->mqtt_publish_ms = CONFIG_APP_MQTT_PUBLISH_PERIOD_MS;

    /* Modbus bus from legacy ext_meter. Only synthesize slot[0] when a real
     * downstream address was stored — factory default is empty slots. */
    config_ext_meter_t ext;
    config_store_get_ext_meter(&ext);
    /* ext.slave_addr is the legacy MASTER downstream address: it seeds slot[0]
     * below but must NOT populate mb_slave_id (this device's own unit id) —
     * pre-split firmware never let the slave read it, and the slave task
     * already falls back to CONFIG_APP_MB_SLAVE_ADDR when it is 0. */
    c->mb_baud_code = ext.baud_code;
    c->mb_parity_code = ext.parity_code;
    /* Pre-split blob: the slave link ran on the shared bus baud, so seed the
     * new per-link field from it rather than dropping to the 9600 default. */
    c->mb_slave_baud_code = ext.baud_code;
    c->mb_stop_bits = 1;   /* RESERVED default */
    c->mb_device = ext.device;
    c->mb_enabled = ext.enabled;
    c->mb_poll_period_ms = ext.poll_period_ms;
    memset(c->mb_slots, 0, sizeof(c->mb_slots));
    if (ext.slave_addr >= 1U && ext.slave_addr <= 247U) {
        c->mb_slots[0].used = true;
        c->mb_slots[0].enabled = ext.enabled;
        c->mb_slots[0].type = ext.device;
        c->mb_slots[0].slave_id = ext.slave_addr;
        strlcpy(c->mb_slots[0].name,
                ext.device == 1U ? "EM-07K" : "PM710",
                sizeof(c->mb_slots[0].name));
    }

    /* Measurement: line_freq + wiring_mode from calibration; CT from Kconfig. */
    atm90e32as_calib_t calib;
    if (energy_meter_get_calibration(&calib) == ESP_OK) {
        c->line_freq = (calib.line_freq == ATM90E32AS_LINE_FREQ_60HZ) ? 1 : 0;
        c->wiring_mode = (calib.wiring_mode == ATM90E32AS_WIRING_3P3W) ? 1 : 0;
    } else {
        c->line_freq = 0;   /* default 50Hz */
        c->wiring_mode = 0; /* default 3P4W */
    }
#if defined(CONFIG_APP_ATM90E32AS_CT_RATIO)
    c->ct_ratio = (uint16_t)CONFIG_APP_ATM90E32AS_CT_RATIO;
#else
    c->ct_ratio = 2000U;
#endif
    c->pt_ratio = 1U; /* RESERVED */
#if defined(CONFIG_APP_ATM90E32AS_I_RATED_A)
    c->i_rated_a = (uint16_t)CONFIG_APP_ATM90E32AS_I_RATED_A;
#else
    c->i_rated_a = 100U;
#endif
#if defined(CONFIG_APP_ATM90E32AS_I_EXPECTED_A)
    c->i_expected_a = (uint16_t)CONFIG_APP_ATM90E32AS_I_EXPECTED_A;
#else
    c->i_expected_a = 75U;
#endif
    /* Unset until first-boot CT→PGA or operator CT Apply / dev console. */
    c->pga = 0U;
#if defined(CONFIG_APP_WIFI_METER_AP_SSID)
    copy_persisted_string(c->ap_ssid, sizeof(c->ap_ssid),
                          CONFIG_APP_WIFI_METER_AP_SSID,
                          strlen(CONFIG_APP_WIFI_METER_AP_SSID) + 1U);
#else
    copy_persisted_string(c->ap_ssid, sizeof(c->ap_ssid),
                          "PowerMeter-AP", sizeof("PowerMeter-AP"));
#endif
#if defined(CONFIG_APP_WIFI_METER_AP_PASSWORD)
    copy_persisted_string(c->ap_pass, sizeof(c->ap_pass),
                          CONFIG_APP_WIFI_METER_AP_PASSWORD,
                          strlen(CONFIG_APP_WIFI_METER_AP_PASSWORD) + 1U);
#else
    copy_persisted_string(c->ap_pass, sizeof(c->ap_pass),
                          "12345678", sizeof("12345678"));
#endif

    /* LCD + buzzer reserved defaults. */
    c->lcd_backlight = true;
    c->lcd_sleep_timeout_s = 0;
    c->lcd_autocycle = true;
    c->lcd_cycle_time_ms = 3000;
    c->buzzer_enable = true;
    c->buzzer_alarm_enable = true;
    c->alarm_voltage_low_enable = true;
    c->alarm_voltage_high_enable = true;
    c->alarm_over_current_enable = true;
    c->alarm_phase_loss_enable = true;
    c->alarm_frequency_enable = true;
    c->alarm_voltage_reference = 0;
    c->alarm_nominal_frequency_hz = c->line_freq ? 60U : 50U;
    c->alarm_voltage_low_v = 180.0f;
    c->alarm_voltage_high_v = 250.0f;
    c->alarm_over_current_a = 10.0f;
    c->alarm_frequency_low_hz = c->line_freq ? 57.0f : 47.0f;
    c->alarm_frequency_high_hz = c->line_freq ? 63.0f : 53.0f;
    c->alarm_trigger_delay_s = 2U;
    c->alarm_clear_delay_s = 2U;
    c->alarm_hysteresis = 5.0f;
    return ESP_OK;
}

/* Persist the complete RAM snapshot in the Feature 15 full-snapshot format.
 * Legacy domain blobs are migration inputs only and are never updated here. */
static esp_err_t snapshot_to_store(const config_manager_t *cfg)
{
    config_snapshot_dto_t *dto = malloc(sizeof(*dto));
    ESP_RETURN_ON_FALSE(dto != NULL, ESP_ERR_NO_MEM, TAG,
                        "no memory for persistent config snapshot");

    snapshot_to_dto(dto, cfg);
    esp_err_t ret = config_store_set_snapshot(dto, sizeof(*dto));
    free(dto);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "save full configuration snapshot failed: %s",
                 esp_err_to_name(ret));
    }
    return ret;
}

/* Legacy snapshots may hold periods outside the current windows (RTU poll was
 * 200..600000 ms, MQTT publish was 1..60 s). Clamp on load so an upgrade keeps
 * the stored configuration instead of failing future update() saves. */
static void normalize_loaded_periods(config_manager_t *c)
{
    if (c->mqtt_publish_ms < CONFIG_MANAGER_MQTT_PERIOD_MIN_MS) {
        ESP_LOGW(TAG, "mqtt publish %u ms below the %u ms floor; clamped",
                 (unsigned)c->mqtt_publish_ms,
                 (unsigned)CONFIG_MANAGER_MQTT_PERIOD_MIN_MS);
        c->mqtt_publish_ms = CONFIG_MANAGER_MQTT_PERIOD_MIN_MS;
    }
    if (c->mb_poll_period_ms < CONFIG_MANAGER_MB_POLL_PERIOD_MIN_MS) {
        ESP_LOGW(TAG, "mb poll %u ms below the %u ms floor; clamped",
                 (unsigned)c->mb_poll_period_ms,
                 (unsigned)CONFIG_MANAGER_MB_POLL_PERIOD_MIN_MS);
        c->mb_poll_period_ms = CONFIG_MANAGER_MB_POLL_PERIOD_MIN_MS;
    }
    if (c->mb_poll_period_ms > CONFIG_MANAGER_MB_POLL_PERIOD_MAX_MS) {
        ESP_LOGW(TAG, "mb poll %u ms above the %u ms ceiling; clamped",
                 (unsigned)c->mb_poll_period_ms,
                 (unsigned)CONFIG_MANAGER_MB_POLL_PERIOD_MAX_MS);
        c->mb_poll_period_ms = CONFIG_MANAGER_MB_POLL_PERIOD_MAX_MS;
    }
}

esp_err_t config_manager_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "create mutex failed");
    }
    return config_manager_load();
}

esp_err_t config_manager_load(void)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* config_manager_t is >2 KB (broker struct + 8 mb_slots + alarms) — heap it
     * rather than putting it on the caller's stack.
     * config_manager_init() runs on main_task during boot, whose stack is
     * only CONFIG_ESP_MAIN_TASK_STACK_SIZE (3584 B in this build). */
    config_manager_t *tmp = malloc(sizeof(*tmp));
    ESP_RETURN_ON_FALSE(tmp != NULL, ESP_ERR_NO_MEM, TAG, "no mem for config snapshot");
    config_snapshot_dto_t *dto = malloc(sizeof(*dto));
    if (dto != NULL) {
        size_t stored_size = 0;
        esp_err_t read_ret = config_store_get_snapshot_sized(dto, sizeof(*dto), &stored_size);
        if (read_ret == ESP_OK) {
            esp_err_t decode_ret = snapshot_from_dto(tmp, dto, stored_size);
            if (decode_ret == ESP_OK) {
                snapshot_overlay_legacy_domains(tmp);
                legacy_shadow_store(tmp);
                ESP_LOGI(TAG, "loaded full configuration snapshot v%u",
                         (unsigned)CONFIG_SNAPSHOT_VERSION);
                normalize_loaded_periods(tmp);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_cfg = *tmp;
                s_loaded = true;
                xSemaphoreGive(s_lock);
                free(dto);
                free(tmp);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "full configuration snapshot is incompatible (%s); "
                          "loading legacy configuration",
                     esp_err_to_name(decode_ret));
        } else if (read_ret == ESP_ERR_NOT_FOUND || read_ret == ESP_ERR_INVALID_SIZE) {
            ESP_LOGI(TAG, "full configuration snapshot unavailable; loading legacy configuration");
        } else {
            ESP_LOGE(TAG, "full configuration snapshot read failed (%s); "
                          "loading legacy configuration",
                     esp_err_to_name(read_ret));
        }
        free(dto);
    } else {
        ESP_LOGW(TAG, "no memory for full snapshot DTO; loading legacy configuration");
    }

    /* One-time migration fallback only. Nothing is written here; the next
     * explicit config_manager_save() creates the full snapshot. */
    esp_err_t migrate_ret = snapshot_from_store(tmp);
    if (migrate_ret != ESP_OK) {
        free(tmp);
        return migrate_ret;
    }
    legacy_shadow_store(tmp);
    normalize_loaded_periods(tmp);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *tmp;
    s_loaded = true;
    xSemaphoreGive(s_lock);
    free(tmp);
    return ESP_OK;
}

esp_err_t config_manager_get(config_manager_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    bool loaded = s_loaded;
    xSemaphoreGive(s_lock);
    return loaded ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t config_manager_get_mqtt(config_mqtt_profile_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* Copy only the broker struct out under the lock (not the whole
     * config_manager_t onto the caller's stack, unlike config_manager_get()). */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool loaded = s_loaded;
    *out = s_cfg.mqtt;
    xSemaphoreGive(s_lock);

    return loaded ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t validate_alarm_config(const config_manager_t *c)
{
    ESP_RETURN_ON_FALSE(c->alarm_voltage_reference <= 2U, ESP_ERR_INVALID_ARG,
                        TAG, "invalid alarm voltage reference");
    ESP_RETURN_ON_FALSE(c->alarm_nominal_frequency_hz == 50U ||
                        c->alarm_nominal_frequency_hz == 60U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid nominal frequency");
    ESP_RETURN_ON_FALSE(c->alarm_voltage_low_v >= 1.0f &&
                        c->alarm_voltage_high_v <= 1000.0f &&
                        c->alarm_voltage_low_v < c->alarm_voltage_high_v,
                        ESP_ERR_INVALID_ARG, TAG, "invalid voltage alarm range");
    ESP_RETURN_ON_FALSE(c->alarm_over_current_a >= 0.1f &&
                        c->alarm_over_current_a <= 1000.0f,
                        ESP_ERR_INVALID_ARG, TAG, "invalid over-current threshold");
    ESP_RETURN_ON_FALSE(c->alarm_frequency_low_hz >= 40.0f &&
                        c->alarm_frequency_high_hz <= 70.0f &&
                        c->alarm_frequency_low_hz < c->alarm_frequency_high_hz,
                        ESP_ERR_INVALID_ARG, TAG, "invalid frequency alarm range");
    ESP_RETURN_ON_FALSE(c->alarm_trigger_delay_s <= 300U &&
                        c->alarm_clear_delay_s <= 300U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid alarm delay");
    ESP_RETURN_ON_FALSE(c->alarm_hysteresis >= 0.0f &&
                        c->alarm_hysteresis <= 100.0f,
                        ESP_ERR_INVALID_ARG, TAG, "invalid alarm hysteresis");
    return ESP_OK;
}

static esp_err_t validate_mb_config(const config_manager_t *c)
{
    ESP_RETURN_ON_FALSE(c->mb_baud_code <= 4U, ESP_ERR_INVALID_ARG, TAG, "invalid mb baud");
    ESP_RETURN_ON_FALSE(c->mb_parity_code <= 2U, ESP_ERR_INVALID_ARG, TAG, "invalid mb parity");
    ESP_RETURN_ON_FALSE(c->mb_slave_baud_code <= 4U, ESP_ERR_INVALID_ARG, TAG,
                        "invalid mb slave baud");
    ESP_RETURN_ON_FALSE(c->mb_poll_period_ms >= CONFIG_MANAGER_MB_POLL_PERIOD_MIN_MS &&
                        c->mb_poll_period_ms <= CONFIG_MANAGER_MB_POLL_PERIOD_MAX_MS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid mb poll period");

    for (size_t i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
        const config_mb_slot_t *s = &c->mb_slots[i];
        if (!s->used) {
            continue;
        }
        ESP_RETURN_ON_FALSE(s->type < 2U /* METER_DEV_COUNT */, ESP_ERR_INVALID_ARG,
                            TAG, "invalid mb slot type");
        ESP_RETURN_ON_FALSE(s->slave_id >= 1U && s->slave_id <= 247U, ESP_ERR_INVALID_ARG,
                            TAG, "invalid mb slave id");
        for (size_t j = i + 1; j < CONFIG_MANAGER_MB_SLOT_COUNT; j++) {
            if (c->mb_slots[j].used && c->mb_slots[j].slave_id == s->slave_id) {
                ESP_LOGE(TAG, "duplicate mb slave id %u (slots %u/%u)",
                         (unsigned)s->slave_id, (unsigned)i, (unsigned)j);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }
    return ESP_OK;
}

/* Legacy single-device mirror (register map / old readers): mb_device reflects
 * the first used slot's type. mb_slave_id is NOT mirrored here anymore — it is
 * this device's own RTU slave address with a single writer (LCD Settings >
 * RTU Slave), and an earlier version auto-filled it from slot[0], which made
 * master-side edits silently change the address this device answers to. */
static void sync_mb_legacy_mirrors(config_manager_t *c)
{
    c->mb_device = 0;
    for (size_t i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
        if (c->mb_slots[i].used) {
            c->mb_device = c->mb_slots[i].type;
            break;
        }
    }
}

esp_err_t config_manager_update(const config_manager_t *in)
{
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "in is NULL");
    ESP_RETURN_ON_ERROR(validate_alarm_config(in), TAG, "invalid alarm configuration");
    ESP_RETURN_ON_ERROR(validate_mb_config(in), TAG, "invalid modbus configuration");
    /* pga: 0 = unset (migrate path); otherwise only 1/2/4 */
    ESP_RETURN_ON_FALSE(in->pga == 0U || in->pga == 1U || in->pga == 2U || in->pga == 4U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid pga (use 1|2|4)");
    /* NCT: product range 1000..6000 step 100 */
    ESP_RETURN_ON_FALSE(in->ct_ratio >= 1000U && in->ct_ratio <= 6000U &&
                        (in->ct_ratio % 100U) == 0U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ct_ratio (1000..6000 step 100)");
    /* SoftAP SSID required (1..32); password empty = open, else WPA2 needs >= 8. */
    size_t ap_ssid_len = strnlen(in->ap_ssid, sizeof(in->ap_ssid));
    ESP_RETURN_ON_FALSE(ap_ssid_len >= 1U && ap_ssid_len <= 32U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid ap_ssid length");
    size_t ap_pass_len = strnlen(in->ap_pass, sizeof(in->ap_pass));
    ESP_RETURN_ON_FALSE(ap_pass_len == 0U || (ap_pass_len >= 8U && ap_pass_len <= 63U),
                        ESP_ERR_INVALID_ARG, TAG, "invalid ap_pass (empty or 8..63)");
    /* Publish period: 5..60 s, rejected rather than clamped so a frontend bug
     * surfaces instead of silently persisting a different cadence. */
    ESP_RETURN_ON_FALSE(in->mqtt_publish_ms >= CONFIG_MANAGER_MQTT_PERIOD_MIN_MS &&
                        in->mqtt_publish_ms <= CONFIG_MANAGER_MQTT_PERIOD_MAX_MS,
                        ESP_ERR_INVALID_ARG, TAG, "invalid mqtt_publish_ms (5000..60000)");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_loaded) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_cfg = *in;
    s_cfg.config_version = CONFIG_MANAGER_VERSION;
    sync_mb_legacy_mirrors(&s_cfg);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t config_manager_save(void)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* Same stack-budget reasoning as config_manager_load(). */
    config_manager_t *tmp = malloc(sizeof(*tmp));
    ESP_RETURN_ON_FALSE(tmp != NULL, ESP_ERR_NO_MEM, TAG, "no mem for config snapshot");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool loaded = s_loaded;
    if (loaded) {
        *tmp = s_cfg;
    }
    xSemaphoreGive(s_lock);
    if (!loaded) {
        free(tmp);
        ESP_LOGE(TAG, "cannot save before configuration is loaded");
        return ESP_ERR_INVALID_STATE;
    }

    /* Merge instead of overlay: a field the Configuration Manager changed must
     * reach the legacy blob, otherwise the next load would overlay the stale
     * legacy value back over it. */
    snapshot_merge_legacy_domains(tmp);

    esp_err_t err = snapshot_to_store(tmp);
    if (err == ESP_OK) {
        /* Publish the merged result and move the shadow forward, so both copies
         * and the reference point agree after a successful save. */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_loaded) {
            s_cfg = *tmp;
        }
        xSemaphoreGive(s_lock);
        legacy_shadow_store(tmp);
    }
    free(tmp);
    return err;
}

esp_err_t config_manager_factory_reset(void)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    /* Config Store owns the configuration namespaces; the ATM90E32AS
     * calibration blob is device calibration held in its own namespace by
     * energy_meter_task, so the Configuration Manager erases it here too —
     * factory reset must not leave a persisted calibration behind. Both errors
     * are reported, but the first one wins so the caller sees a failure. */
    esp_err_t ret = config_store_factory_reset();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "erase configuration namespaces failed: %s", esp_err_to_name(ret));
    }

    esp_err_t calib_ret = energy_meter_erase_calibration();
    if (calib_ret != ESP_OK) {
        ESP_LOGE(TAG, "erase calibration namespace failed: %s", esp_err_to_name(calib_ret));
        if (ret == ESP_OK) {
            ret = calib_ret;
        }
    }

    /* Reload so the RAM snapshot reflects the defaults even before the reboot
     * that makes the runtime actually use them. */
    esp_err_t load_ret = config_manager_load();
    if (load_ret != ESP_OK && ret == ESP_OK) {
        ret = load_ret;
    }
    return ret;
}

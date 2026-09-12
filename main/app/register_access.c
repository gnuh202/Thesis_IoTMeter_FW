#include "register_access.h"

#include <stdlib.h>
#include <string.h>

#include "config_manager.h"
#include "measurement_data.h"
#include "system_status.h"
#include "io_expander.h"

/*
 * Register Access Layer (RAL) — skeleton implementation (Feature 08).
 *
 * data_point_read()/data_point_write() dispatch on data_point_id_t and serve each
 * data point from its existing source module's public getter/setter. Wired so
 * far: Measurement, Energy, System Status, Digital Input, Digital Output
 * (read/write), and Configuration (CFG_*, RAM-only via Configuration Manager —
 * see write_config()). IDs outside all of these return ESP_ERR_NOT_SUPPORTED.
 *
 * This layer owns no state, no task, and no timer. It does not modify any source
 * module — it only calls their existing public APIs.
 */

/* Validate the caller buffer size against the data point's type, then copy out.
 * Returns ESP_ERR_INVALID_SIZE on a mismatch so a caller cannot misread a field. */
static esp_err_t copy_out(void *buffer, size_t size, const void *src, size_t type_size)
{
    if (size != type_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buffer, src, type_size);
    return ESP_OK;
}

/* Measurement + Energy: both live in the measurement snapshot. A pre-first-update
 * snapshot returns ESP_ERR_INVALID_STATE with *m zeroed; we still serve the
 * zeros (reserved fields behave the same way), so the state is not fatal here. */
static esp_err_t read_measurement(data_point_id_t id, void *buffer, size_t size)
{
    measurement_data_t m;
    (void)measurement_data_get(&m);

    switch (id) {
    /* Voltage. */
    case MEAS_VOLTAGE_L1:        return copy_out(buffer, size, &m.voltage_l1, sizeof(float));
    case MEAS_VOLTAGE_L2:        return copy_out(buffer, size, &m.voltage_l2, sizeof(float));
    case MEAS_VOLTAGE_L3:        return copy_out(buffer, size, &m.voltage_l3, sizeof(float));
    case MEAS_VOLTAGE_AVG:       return copy_out(buffer, size, &m.voltage_avg, sizeof(float));
    /* Current. */
    case MEAS_CURRENT_L1:        return copy_out(buffer, size, &m.current_l1, sizeof(float));
    case MEAS_CURRENT_L2:        return copy_out(buffer, size, &m.current_l2, sizeof(float));
    case MEAS_CURRENT_L3:        return copy_out(buffer, size, &m.current_l3, sizeof(float));
    case MEAS_CURRENT_NEUTRAL:   return copy_out(buffer, size, &m.current_neutral, sizeof(float));
    case MEAS_CURRENT_AVG:       return copy_out(buffer, size, &m.current_avg, sizeof(float));
    /* Frequency. */
    case MEAS_FREQUENCY:         return copy_out(buffer, size, &m.frequency, sizeof(float));
    /* Active power. */
    case MEAS_POWER_ACTIVE_L1:   return copy_out(buffer, size, &m.p1, sizeof(float));
    case MEAS_POWER_ACTIVE_L2:   return copy_out(buffer, size, &m.p2, sizeof(float));
    case MEAS_POWER_ACTIVE_L3:   return copy_out(buffer, size, &m.p3, sizeof(float));
    case MEAS_POWER_ACTIVE_TOTAL:return copy_out(buffer, size, &m.p_total, sizeof(float));
    /* Reactive power. */
    case MEAS_POWER_REACTIVE_L1: return copy_out(buffer, size, &m.q1, sizeof(float));
    case MEAS_POWER_REACTIVE_L2: return copy_out(buffer, size, &m.q2, sizeof(float));
    case MEAS_POWER_REACTIVE_L3: return copy_out(buffer, size, &m.q3, sizeof(float));
    case MEAS_POWER_REACTIVE_TOTAL: return copy_out(buffer, size, &m.q_total, sizeof(float));
    /* Apparent power. */
    case MEAS_POWER_APPARENT_L1: return copy_out(buffer, size, &m.s1, sizeof(float));
    case MEAS_POWER_APPARENT_L2: return copy_out(buffer, size, &m.s2, sizeof(float));
    case MEAS_POWER_APPARENT_L3: return copy_out(buffer, size, &m.s3, sizeof(float));
    case MEAS_POWER_APPARENT_TOTAL: return copy_out(buffer, size, &m.s_total, sizeof(float));
    /* Power factor. */
    case MEAS_PF_L1:             return copy_out(buffer, size, &m.pf1, sizeof(float));
    case MEAS_PF_L2:             return copy_out(buffer, size, &m.pf2, sizeof(float));
    case MEAS_PF_L3:             return copy_out(buffer, size, &m.pf3, sizeof(float));
    case MEAS_PF_TOTAL:          return copy_out(buffer, size, &m.pf_total, sizeof(float));
    /* THD (reserved source, reads 0). */
    case MEAS_VOLTAGE_THD:       return copy_out(buffer, size, &m.voltage_thd, sizeof(float));
    case MEAS_CURRENT_THD:       return copy_out(buffer, size, &m.current_thd, sizeof(float));
    /* Temperature. */
    case MEAS_TEMP_ATM90:        return copy_out(buffer, size, &m.temp_atm90, sizeof(float));
    case MEAS_TEMP_MCU:          return copy_out(buffer, size, &m.temp_mcu, sizeof(float));
    case MEAS_TEMP_RESERVED:     return copy_out(buffer, size, &m.temp_reserved, sizeof(float));
    /* Timestamp + validity. */
    case MEAS_LAST_UPDATE_US:    return copy_out(buffer, size, &m.last_update_us, sizeof(uint64_t));
    case MEAS_VALID: {
        uint8_t v = m.valid ? 1 : 0;
        return copy_out(buffer, size, &v, sizeof(uint8_t));
    }

    /* Energy. */
    case ENERGY_ACTIVE_IMPORT:   return copy_out(buffer, size, &m.energy_import, sizeof(float));
    case ENERGY_ACTIVE_EXPORT:   return copy_out(buffer, size, &m.energy_export, sizeof(float));
    case ENERGY_REACTIVE_IMPORT: return copy_out(buffer, size, &m.energy_reactive_import, sizeof(float));
    case ENERGY_REACTIVE_EXPORT: return copy_out(buffer, size, &m.energy_reactive_export, sizeof(float));
    case ENERGY_APPARENT:        return copy_out(buffer, size, &m.energy_apparent, sizeof(float));

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

/* System Status: one enum state per module, served as a single byte. */
static esp_err_t read_system_status(data_point_id_t id, void *buffer, size_t size)
{
    system_module_t mod;
    switch (id) {
    case SYS_ATM90_STATUS:          mod = SYS_MODULE_ATM90; break;
    case SYS_RS485_MASTER_STATUS:   mod = SYS_MODULE_RS485_MASTER; break;
    case SYS_RS485_SLAVE_STATUS:    mod = SYS_MODULE_RS485_SLAVE; break;
    case SYS_ETHERNET_STATUS:       mod = SYS_MODULE_ETHERNET; break;
    case SYS_WIFI_STATUS:           mod = SYS_MODULE_WIFI; break;
    case SYS_MQTT_STATUS:           mod = SYS_MODULE_MQTT; break;
    case SYS_SD_CARD_STATUS:        mod = SYS_MODULE_SD_CARD; break;
    case SYS_DIGITAL_INPUT_STATUS:  mod = SYS_MODULE_DIGITAL_INPUT; break;
    case SYS_DIGITAL_OUTPUT_STATUS: mod = SYS_MODULE_DIGITAL_OUTPUT; break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t state = (uint8_t)system_status_get(mod);
    return copy_out(buffer, size, &state, sizeof(uint8_t));
}

/* Digital Input / Digital Output read: one PCF8574 pin per ID, served as a byte
 * (DO reads return the cached last-set level). */
static esp_err_t read_digital(data_point_id_t id, void *buffer, size_t size)
{
    bool level = false;
    esp_err_t err;

    switch (id) {
    case DI_INPUT0_STATE: err = io_expander_get_in0(&level); break;
    case DI_INPUT1_STATE: err = io_expander_get_in1(&level); break;
    case DO_RELAY0_STATE: err = io_expander_get_out0(&level); break;
    case DO_RELAY1_STATE: err = io_expander_get_out1(&level); break;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        return err;
    }

    uint8_t v = level ? 1 : 0;
    return copy_out(buffer, size, &v, sizeof(uint8_t));
}

/* ---- Configuration (CFG_*) ----
 *
 * Every CFG_ data point maps to one field of config_manager_t. Reads copy from
 * the Configuration Manager RAM snapshot; writes update only that RAM snapshot
 * (config_manager_update) — no NVS save, no apply, no restart of any module.
 *
 * Table-driven: offset/size into config_manager_t + kind + writability. Scalar
 * reads/writes require an exact size match; string reads/writes take the full
 * field size (n bytes, NUL-terminated) per the header's buffer contract.
 */
typedef struct {
    size_t offset;      /* into config_manager_t */
    size_t size;        /* field size in bytes */
    bool is_string;     /* char[n] vs scalar */
    bool writable;      /* false -> RO: data_point_write returns NOT_SUPPORTED */
} cfg_field_t;

#define CFG_FIELD(member, str, rw) \
    { offsetof(config_manager_t, member), sizeof(((config_manager_t *)0)->member), (str), (rw) }

/* Look up the config field descriptor for a CFG_ id; NULL if not a CFG_ id. */
static const cfg_field_t *cfg_field(data_point_id_t id)
{
    static const cfg_field_t fields[] = {
        [CFG_VERSION - CFG_VERSION]             = CFG_FIELD(config_version, false, false),  /* RO */
        [CFG_DEVICE_NAME - CFG_VERSION]         = CFG_FIELD(device_name, true, true),
        [CFG_FIRMWARE_VERSION - CFG_VERSION]    = CFG_FIELD(firmware_version, true, false), /* RO */
        [CFG_HARDWARE_VERSION - CFG_VERSION]    = CFG_FIELD(hardware_version, true, false), /* RO */
        [CFG_DHCP_ENABLE - CFG_VERSION]         = CFG_FIELD(dhcp_enable, false, true),
        [CFG_STATIC_IP - CFG_VERSION]           = CFG_FIELD(static_ip, true, true),
        [CFG_GATEWAY - CFG_VERSION]             = CFG_FIELD(gateway, true, true),
        [CFG_NETMASK - CFG_VERSION]             = CFG_FIELD(netmask, true, true),
        [CFG_DNS - CFG_VERSION]                 = CFG_FIELD(dns, true, true),
        [CFG_WIFI_SSID - CFG_VERSION]           = CFG_FIELD(wifi_ssid, true, true),
        [CFG_WIFI_PASS - CFG_VERSION]           = CFG_FIELD(wifi_pass, true, true),
        [CFG_MQTT_ENABLE - CFG_VERSION]         = CFG_FIELD(mqtt_enable, false, true),
        [CFG_MQTT_BROKER - CFG_VERSION]         = CFG_FIELD(mqtt_broker, true, true),
        [CFG_MQTT_PORT - CFG_VERSION]           = CFG_FIELD(mqtt_port, false, true),
        [CFG_MQTT_USER - CFG_VERSION]           = CFG_FIELD(mqtt_user, true, true),
        [CFG_MQTT_PASS - CFG_VERSION]           = CFG_FIELD(mqtt_pass, true, true),
        [CFG_MQTT_PUBLISH_MS - CFG_VERSION]     = CFG_FIELD(mqtt_publish_ms, false, true),
        [CFG_MQTT_CLIENT_ID - CFG_VERSION]      = CFG_FIELD(mqtt_client_id, true, true),   /* reserved field, RAM only */
        [CFG_MB_SLAVE_ID - CFG_VERSION]         = CFG_FIELD(mb_slave_id, false, true),      /* this device's own RTU slave address (LCD-owned) */
        [CFG_MB_BAUD_CODE - CFG_VERSION]        = CFG_FIELD(mb_baud_code, false, true),     /* master bus only */
        [CFG_MB_PARITY_CODE - CFG_VERSION]      = CFG_FIELD(mb_parity_code, false, true),   /* master bus only */
        [CFG_MB_STOP_BITS - CFG_VERSION]        = CFG_FIELD(mb_stop_bits, false, true),    /* reserved field, RAM only */
        [CFG_LINE_FREQ - CFG_VERSION]           = CFG_FIELD(line_freq, false, false), /* RO: mirror of meter calib; set via console */
        [CFG_WIRING_MODE - CFG_VERSION]         = CFG_FIELD(wiring_mode, false, false),   /* RO: energy_meter is master */
        [CFG_CT_RATIO - CFG_VERSION]            = CFG_FIELD(ct_ratio, false, true),
        [CFG_PT_RATIO - CFG_VERSION]            = CFG_FIELD(pt_ratio, false, true),        /* reserved field, RAM only */
        [CFG_LCD_BACKLIGHT - CFG_VERSION]       = CFG_FIELD(lcd_backlight, false, true),
        [CFG_LCD_SLEEP_TIMEOUT_S - CFG_VERSION] = CFG_FIELD(lcd_sleep_timeout_s, false, true),
        [CFG_BUZZER_ENABLE - CFG_VERSION]       = CFG_FIELD(buzzer_enable, false, true),   /* reserved field, RAM only */
        [CFG_MB_SLAVE_BAUD - CFG_VERSION]       = CFG_FIELD(mb_slave_baud_code, false, true), /* slave link baud (LCD-owned); parity fixed 8N1 */
    };

    /* The table is indexed directly by (id - CFG_VERSION): every entry above is
     * a designated initializer at its own id's offset, and the CFG_ enum is
     * contiguous from CFG_VERSION to CFG_MB_SLAVE_BAUD (the tail id added with
     * the master/slave split). An earlier version added
     * a manual "-1" shift for ids past CFG_WIRING_MODE and NULLed out
     * CFG_WIRING_MODE+1, as if a gap followed CFG_WIRING_MODE — there is none.
     * That off-by-one made CFG_CT_RATIO resolve to NULL and routed
     * CFG_PT_RATIO / CFG_LCD_BACKLIGHT / CFG_LCD_SLEEP_TIMEOUT_S /
     * CFG_BUZZER_ENABLE each to the *previous* field, so the exact-size guard in
     * read_config()/write_config() rejected all five with ESP_ERR_INVALID_SIZE.
     * A plain bounds check + direct index is correct. */
    if (id < CFG_VERSION || id > CFG_MB_SLAVE_BAUD) {
        return NULL;
    }
    return &fields[id - CFG_VERSION];
}

static esp_err_t read_config(data_point_id_t id, void *buffer, size_t size)
{
    const cfg_field_t *f = cfg_field(id);
    if (f == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (size != f->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* config_manager_t is ~2.2 KB since Feature 12 (mqtt_profiles[3]); heap it
     * rather than putting it on the caller's stack (callers include the
     * console REPL task, whose stack is a few KB total). */
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = config_manager_get(cfg);
    if (err == ESP_OK) {
        memcpy(buffer, (const uint8_t *)cfg + f->offset, f->size);
    }
    free(cfg);
    return err;
}

static esp_err_t write_config(data_point_id_t id, const void *buffer, size_t size)
{
    const cfg_field_t *f = cfg_field(id);
    if (f == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!f->writable) {
        return ESP_ERR_NOT_SUPPORTED;  /* RO data point */
    }
    if (size != f->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    /* Read-modify-write of the RAM snapshot only: no NVS save, no apply, no
     * module restart. Persisting is a separate, explicit config_manager_save().
     * Heap-allocated for the same reason as read_config() above. */
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = config_manager_get(cfg);
    if (err != ESP_OK) {
        free(cfg);
        return err;
    }

    memcpy((uint8_t *)cfg + f->offset, buffer, f->size);
    if (f->is_string) {
        /* Force NUL-termination so a hostile/truncated write cannot leave an
         * unterminated string in the snapshot. */
        ((char *)cfg)[f->offset + f->size - 1] = '\0';
    }
    err = config_manager_update(cfg);
    free(cfg);
    return err;
}

/* ---- MQTT profiles (Feature 12) ----
 *
 * CFG_MQTT_ACTIVE_PROFILE selects which of config_manager_t.mqtt_profiles[3]
 * the 12 CFG_MQTT_P_* ids below address — write the selector first, then
 * read/write a field. There is no per-profile fixed addressing; this mirrors
 * the single-active-profile pattern the legacy CFG_MQTT_* ids already use.
 *
 * password / ca_path / cert_path / key_path are write-only: data_point_read()
 * on those four returns ESP_ERR_NOT_SUPPORTED (Feature 12 security
 * requirement — no secret / cert-path readback over any protocol).
 *
 * Same RAM-only contract as write_config() above: read-modify-write of the
 * Configuration Manager snapshot, no NVS save, no apply, no module restart.
 */
typedef struct {
    size_t offset;      /* into config_mqtt_profile_t */
    size_t size;        /* data-point wire size in bytes */
    bool is_string;
    bool readable;       /* false -> data_point_read returns NOT_SUPPORTED */
} mqtt_p_field_t;

#define MQTT_P_FIELD(member, str, rdbl) \
    { offsetof(config_mqtt_profile_t, member), sizeof(((config_mqtt_profile_t *)0)->member), (str), (rdbl) }

/* CFG_MQTT_P_TLS_MODE is handled separately (enum <-> 1-byte wire marshal, not
 * a raw memcpy of the enum's own in-memory size) and is not in this table. */
static const mqtt_p_field_t *mqtt_p_field(data_point_id_t id)
{
    static const mqtt_p_field_t fields[] = {
        [CFG_MQTT_P_ENABLE - CFG_MQTT_P_ENABLE]         = MQTT_P_FIELD(enable, false, true),
        [CFG_MQTT_P_BROKER - CFG_MQTT_P_ENABLE]         = MQTT_P_FIELD(broker, true, true),
        [CFG_MQTT_P_PORT - CFG_MQTT_P_ENABLE]           = MQTT_P_FIELD(port, false, true),
        [CFG_MQTT_P_USERNAME - CFG_MQTT_P_ENABLE]       = MQTT_P_FIELD(username, true, true),
        [CFG_MQTT_P_PASSWORD - CFG_MQTT_P_ENABLE]       = MQTT_P_FIELD(password, true, false),        /* write-only */
        [CFG_MQTT_P_CLIENT_ID - CFG_MQTT_P_ENABLE]      = MQTT_P_FIELD(client_id, true, true),
        [CFG_MQTT_P_PUBLISH_TOPIC - CFG_MQTT_P_ENABLE]  = MQTT_P_FIELD(publish_topic, true, true),
        [CFG_MQTT_P_SUBSCRIBE_TOPIC - CFG_MQTT_P_ENABLE]= MQTT_P_FIELD(subscribe_topic, true, true),
        [CFG_MQTT_P_CA_PATH - CFG_MQTT_P_ENABLE]        = MQTT_P_FIELD(ca_path, true, false),         /* write-only */
        [CFG_MQTT_P_CERT_PATH - CFG_MQTT_P_ENABLE]      = MQTT_P_FIELD(cert_path, true, false),       /* write-only */
        [CFG_MQTT_P_KEY_PATH - CFG_MQTT_P_ENABLE]       = MQTT_P_FIELD(key_path, true, false),        /* write-only */
    };

    if (id < CFG_MQTT_P_ENABLE || id > CFG_MQTT_P_KEY_PATH || id == CFG_MQTT_P_TLS_MODE) {
        return NULL;
    }
    return &fields[id - CFG_MQTT_P_ENABLE];
}

static esp_err_t read_mqtt_profile(data_point_id_t id, void *buffer, size_t size)
{
    /* Heap-allocated for the same reason as read_config() above. */
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = config_manager_get(cfg);
    if (err != ESP_OK) {
        free(cfg);
        return err;
    }

    if (id == CFG_MQTT_ACTIVE_PROFILE) {
        uint8_t v = cfg->mqtt_active_profile;
        err = copy_out(buffer, size, &v, sizeof(v));
        free(cfg);
        return err;
    }

    /* active_profile is range-checked on write below; this only guards a
     * snapshot that predates that check (e.g. a fresh default of 0 is fine). */
    uint8_t idx = (cfg->mqtt_active_profile < CONFIG_MANAGER_MQTT_PROFILE_COUNT)
                  ? cfg->mqtt_active_profile : 0;
    const config_mqtt_profile_t *p = &cfg->mqtt_profiles[idx];

    if (id == CFG_MQTT_P_TLS_MODE) {
        uint8_t v = (uint8_t)p->tls_mode;
        err = copy_out(buffer, size, &v, sizeof(v));
        free(cfg);
        return err;
    }

    const mqtt_p_field_t *f = mqtt_p_field(id);
    if (f == NULL) {
        free(cfg);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!f->readable) {
        free(cfg);
        return ESP_ERR_NOT_SUPPORTED;  /* password / ca_path / cert_path / key_path */
    }
    if (size != f->size) {
        free(cfg);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy(buffer, (const uint8_t *)p + f->offset, f->size);
    free(cfg);
    return ESP_OK;
}

static esp_err_t write_mqtt_profile(data_point_id_t id, const void *buffer, size_t size)
{
    /* Heap-allocated for the same reason as write_config() above. */
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = config_manager_get(cfg);
    if (err != ESP_OK) {
        free(cfg);
        return err;
    }

    if (id == CFG_MQTT_ACTIVE_PROFILE) {
        if (size != sizeof(uint8_t)) {
            free(cfg);
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t v = *(const uint8_t *)buffer;
        if (v >= CONFIG_MANAGER_MQTT_PROFILE_COUNT) {
            free(cfg);
            return ESP_ERR_INVALID_ARG;  /* would index mqtt_profiles[] out of bounds */
        }
        cfg->mqtt_active_profile = v;
        err = config_manager_update(cfg);
        free(cfg);
        return err;
    }

    uint8_t idx = (cfg->mqtt_active_profile < CONFIG_MANAGER_MQTT_PROFILE_COUNT)
                  ? cfg->mqtt_active_profile : 0;
    config_mqtt_profile_t *p = &cfg->mqtt_profiles[idx];

    if (id == CFG_MQTT_P_TLS_MODE) {
        if (size != sizeof(uint8_t)) {
            free(cfg);
            return ESP_ERR_INVALID_SIZE;
        }
        uint8_t v = *(const uint8_t *)buffer;
        if (v > MQTT_TLS_INSECURE) {
            free(cfg);
            return ESP_ERR_INVALID_ARG;
        }
        p->tls_mode = (mqtt_tls_mode_t)v;
        err = config_manager_update(cfg);
        free(cfg);
        return err;
    }

    const mqtt_p_field_t *f = mqtt_p_field(id);
    if (f == NULL) {
        free(cfg);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (size != f->size) {
        free(cfg);
        return ESP_ERR_INVALID_SIZE;
    }

    memcpy((uint8_t *)p + f->offset, buffer, f->size);
    if (f->is_string) {
        ((char *)p)[f->offset + f->size - 1] = '\0';
    }
    err = config_manager_update(cfg);
    free(cfg);
    return err;
}

esp_err_t data_point_read(data_point_id_t id, void *buffer, size_t size)
{
    if (buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)id < 0 || id >= DATA_POINT_ID_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Measurement + Energy live in the measurement snapshot. */
    if ((id >= MEAS_VOLTAGE_L1 && id <= MEAS_VALID) ||
        (id >= ENERGY_ACTIVE_IMPORT && id <= ENERGY_APPARENT)) {
        return read_measurement(id, buffer, size);
    }

    /* System Status. */
    if (id >= SYS_ATM90_STATUS && id <= SYS_DIGITAL_OUTPUT_STATUS) {
        return read_system_status(id, buffer, size);
    }

    /* Digital Input / Digital Output. */
    if (id == DI_INPUT0_STATE || id == DI_INPUT1_STATE ||
        id == DO_RELAY0_STATE || id == DO_RELAY1_STATE) {
        return read_digital(id, buffer, size);
    }

    /* Configuration (CFG_*): served from the Configuration Manager RAM snapshot. */
    if (id >= CFG_VERSION && id <= CFG_MB_SLAVE_BAUD) {
        return read_config(id, buffer, size);
    }

    /* Configuration: MQTT profiles (Feature 12). */
    if (id >= CFG_MQTT_ACTIVE_PROFILE && id <= CFG_MQTT_P_KEY_PATH) {
        return read_mqtt_profile(id, buffer, size);
    }

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t data_point_write(data_point_id_t id, const void *buffer, size_t size)
{
    if (buffer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((int)id < 0 || id >= DATA_POINT_ID_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Configuration (CFG_*): RAM snapshot only. RO ids return NOT_SUPPORTED. */
    if (id >= CFG_VERSION && id <= CFG_MB_SLAVE_BAUD) {
        return write_config(id, buffer, size);
    }

    /* Configuration: MQTT profiles (Feature 12). RAM snapshot only, same as
     * write_config() above — every field here is writable. */
    if (id >= CFG_MQTT_ACTIVE_PROFILE && id <= CFG_MQTT_P_KEY_PATH) {
        return write_mqtt_profile(id, buffer, size);
    }

    switch (id) {
    /* Digital Output relays. */
    case DO_RELAY0_STATE:
    case DO_RELAY1_STATE: {
        if (size != sizeof(uint8_t)) {
            return ESP_ERR_INVALID_SIZE;
        }
        bool level = (*(const uint8_t *)buffer) != 0;
        return (id == DO_RELAY0_STATE) ? io_expander_set_out0(level)
                                       : io_expander_set_out1(level);
    }

    /* Everything else is a read-only data point. */
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

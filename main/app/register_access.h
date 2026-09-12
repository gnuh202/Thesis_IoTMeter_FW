#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register Access Layer (RAL) — skeleton (Feature 08).
 *
 * A single, uniform data-access facade between the firmware data models
 * (Measurement Data / System Status / Configuration Manager / Digital IO) and
 * the communication protocols that will consume them (Modbus, MQTT, Web, LCD,
 * Data Logger).
 *
 * Every data point in the Data Dictionary (docs/register_map.md) has one stable
 * identifier here — data_point_id_t. Consumers never reach into the source
 * modules directly; they go through data_point_read()/data_point_write() with an
 * ID, a caller-owned buffer, and its size.
 *
 * Scope (Feature 08, skeleton only):
 *   - This layer defines the ID space and the two access entry points.
 *   - It does NOT implement every data point yet. IDs that are not wired to a
 *     source return ESP_ERR_NOT_SUPPORTED (see register_access.c).
 *   - It adds no task, no timer, and does not modify Measurement, System
 *     Status, or Configuration Manager — it only reads/writes through their
 *     existing public getters/setters.
 *
 * Buffer contract (per data type, host byte order, no protocol framing here):
 *   - float          -> 4 bytes
 *   - uint32_t       -> 4 bytes
 *   - uint16_t       -> 2 bytes
 *   - uint8_t / enum -> 1 byte
 *   - bool           -> 1 byte (0/1)
 *   - uint64_t       -> 8 bytes
 *   - string[n]      -> n bytes (NUL-terminated, caller buffer >= n)
 * The caller must pass a buffer whose size matches the data point's type;
 * a mismatch returns ESP_ERR_INVALID_SIZE.
 */

typedef enum {
    /* ---- Measurement (RO) ---- */
    MEAS_VOLTAGE_L1 = 0,
    MEAS_VOLTAGE_L2,
    MEAS_VOLTAGE_L3,
    MEAS_VOLTAGE_AVG,
    MEAS_CURRENT_L1,
    MEAS_CURRENT_L2,
    MEAS_CURRENT_L3,
    MEAS_CURRENT_NEUTRAL,
    MEAS_CURRENT_AVG,
    MEAS_FREQUENCY,
    MEAS_POWER_ACTIVE_L1,
    MEAS_POWER_ACTIVE_L2,
    MEAS_POWER_ACTIVE_L3,
    MEAS_POWER_ACTIVE_TOTAL,
    MEAS_POWER_REACTIVE_L1,
    MEAS_POWER_REACTIVE_L2,
    MEAS_POWER_REACTIVE_L3,
    MEAS_POWER_REACTIVE_TOTAL,
    MEAS_POWER_APPARENT_L1,
    MEAS_POWER_APPARENT_L2,
    MEAS_POWER_APPARENT_L3,
    MEAS_POWER_APPARENT_TOTAL,
    MEAS_PF_L1,
    MEAS_PF_L2,
    MEAS_PF_L3,
    MEAS_PF_TOTAL,
    MEAS_VOLTAGE_THD,        /* reserved source (reads 0) */
    MEAS_CURRENT_THD,        /* reserved source (reads 0) */
    MEAS_TEMP_ATM90,
    MEAS_TEMP_MCU,           /* reserved source (reads 0) */
    MEAS_TEMP_RESERVED,      /* reserved source (reads 0) */
    MEAS_LAST_UPDATE_US,
    MEAS_VALID,

    /* ---- Energy (RO) ---- */
    ENERGY_ACTIVE_IMPORT,
    ENERGY_ACTIVE_EXPORT,
    ENERGY_REACTIVE_IMPORT,
    ENERGY_REACTIVE_EXPORT,
    ENERGY_APPARENT,         /* reserved source (reads 0) */

    /* ---- System Status (RO) ---- */
    SYS_ATM90_STATUS,
    SYS_RS485_MASTER_STATUS,
    SYS_RS485_SLAVE_STATUS,
    SYS_ETHERNET_STATUS,
    SYS_WIFI_STATUS,
    SYS_MQTT_STATUS,
    SYS_SD_CARD_STATUS,
    SYS_DIGITAL_INPUT_STATUS,
    SYS_DIGITAL_OUTPUT_STATUS,

    /* ---- Configuration (RW) ---- */
    CFG_VERSION,             /* RO */
    CFG_DEVICE_NAME,
    CFG_FIRMWARE_VERSION,    /* RO */
    CFG_HARDWARE_VERSION,    /* RO */
    CFG_DHCP_ENABLE,
    CFG_STATIC_IP,
    CFG_GATEWAY,
    CFG_NETMASK,
    CFG_DNS,
    CFG_WIFI_SSID,
    CFG_WIFI_PASS,
    CFG_MQTT_PUBLISH_MS,     /* cfg.mqtt_publish_ms: 1000..60000 (config_manager_update) */
    CFG_MB_SLAVE_ID,         /* this device's own RTU slave address (LCD-owned) */
    CFG_MB_BAUD_CODE,        /* master bus baud only (portal-owned) */
    CFG_MB_PARITY_CODE,      /* master bus parity only (portal-owned) */
    CFG_MB_STOP_BITS,        /* reserved */
    CFG_LINE_FREQ,
    CFG_WIRING_MODE,
    CFG_CT_RATIO,            /* reserved */
    CFG_PT_RATIO,            /* reserved */
    CFG_LCD_BACKLIGHT,       /* reserved */
    CFG_LCD_SLEEP_TIMEOUT_S, /* reserved */
    CFG_BUZZER_ENABLE,       /* reserved */
    CFG_MB_SLAVE_BAUD,       /* this device's own RTU slave baud (LCD-owned) */

    /* ---- MQTT broker (single, cfg.mqtt = config_mqtt_profile_t) ----
     * The device has exactly one broker, so these address it directly — there
     * is no profile selector. CFG_MQTT_ENABLE is the flag the MQTT runtime
     * gates on and the LCD Settings > MQTT toggle writes; the web portal
     * configures the rest.
     * CFG_MQTT_PASSWORD / CA_PATH / CERT_PATH / KEY_PATH are write-only:
     * data_point_read() returns ESP_ERR_NOT_SUPPORTED for those four. */
    CFG_MQTT_ENABLE,
    CFG_MQTT_BROKER,
    CFG_MQTT_PORT,
    CFG_MQTT_USERNAME,
    CFG_MQTT_PASSWORD,        /* write-only */
    CFG_MQTT_CLIENT_ID,
    CFG_MQTT_PUBLISH_TOPIC,
    CFG_MQTT_SUBSCRIBE_TOPIC,
    CFG_MQTT_TLS_MODE,
    CFG_MQTT_CA_PATH,         /* write-only */
    CFG_MQTT_CERT_PATH,       /* write-only */
    CFG_MQTT_KEY_PATH,        /* write-only */

    /* ---- Digital Input (RO) ---- */
    DI_INPUT0_STATE,
    DI_INPUT1_STATE,

    /* ---- Digital Output (RW) ---- */
    DO_RELAY0_STATE,
    DO_RELAY1_STATE,

    DATA_POINT_ID_COUNT,     /* sentinel — number of data points (do not hardcode: recount via this enum) */
} data_point_id_t;

/*
 * Read one data point by ID into a caller-owned buffer.
 *   id     — data point identifier.
 *   buffer — destination, owned by the caller.
 *   size   — size of buffer in bytes; must match the data point's type.
 * Returns:
 *   ESP_OK                on success,
 *   ESP_ERR_INVALID_ARG   if buffer is NULL or id is out of range,
 *   ESP_ERR_INVALID_SIZE  if size does not match the data point's type,
 *   ESP_ERR_NOT_SUPPORTED if the ID is not yet wired to a source (TODO),
 *   or an error propagated from the underlying source module.
 */
esp_err_t data_point_read(data_point_id_t id, void *buffer, size_t size);

/*
 * Write one data point by ID from a caller-owned buffer. Same argument and
 * error contract as data_point_read(). Read-only data points return
 * ESP_ERR_NOT_SUPPORTED.
 */
esp_err_t data_point_write(data_point_id_t id, const void *buffer, size_t size);

#ifdef __cplusplus
}
#endif

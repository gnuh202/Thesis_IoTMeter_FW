#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configuration Manager: the single access point for system configuration.
 *
 * This is a facade over the existing config_store (NVS) component. It holds one
 * central snapshot in RAM, loaded from config_store at init and written back on
 * save. Future consumers (LCD menu, web config, MQTT config, factory reset)
 * should go through here rather than reading config_store directly.
 *
 * NVS ownership: config_manager never opens NVS itself — it delegates all
 * persistence to config_store, which keeps the magic/version blob scheme.
 *
 * Scope (Feature 05, foundation only): this module stores and persists config;
 * it does NOT apply settings to drivers and does NOT notify other modules. Some
 * struct fields are RESERVED (no hardware/driver source yet) and only carry a
 * default value in RAM — they are documented inline below.
 */

#define CONFIG_MANAGER_DEVICE_NAME_LEN 33
#define CONFIG_MANAGER_IP_LEN 16
#define CONFIG_MANAGER_SSID_LEN 33
#define CONFIG_MANAGER_PASS_LEN 65
#define CONFIG_MANAGER_VERSION_LEN 24

/* ---- MQTT broker (single) ---- */
#define CONFIG_MANAGER_MQTT_BROKER_LEN 64
#define CONFIG_MANAGER_MQTT_USER_LEN 32
#define CONFIG_MANAGER_MQTT_PASS_LEN 64
#define CONFIG_MANAGER_MQTT_CLIENT_ID_LEN 64
#define CONFIG_MANAGER_MQTT_TOPIC_LEN 64
#define CONFIG_MANAGER_MQTT_PATH_LEN 64
#define CONFIG_MANAGER_MQTT_NAME_LEN 32   /* broker label */

/* Telemetry publish period bounds, in milliseconds (5..60 s). One broker means
 * one interval, and both frontends express it in seconds, so the range is
 * enforced centrally in config_manager_update() rather than per-frontend. */
#define CONFIG_MANAGER_MQTT_PERIOD_MIN_MS 5000U
#define CONFIG_MANAGER_MQTT_PERIOD_MAX_MS 60000U

/* RTU master poll period bounds, in milliseconds (5..60 s — same window as the
 * MQTT publish period; both frontends express it in seconds). Enforced centrally
 * in config_manager_update(); values above the ceiling are clamped down on load. */
#define CONFIG_MANAGER_MB_POLL_PERIOD_MIN_MS 5000U
#define CONFIG_MANAGER_MB_POLL_PERIOD_MAX_MS 60000U

/* RTU master multi-device slots (PM710 / EM-07K on one RS485 bus). */
#define CONFIG_MANAGER_MB_SLOT_COUNT 5
#define CONFIG_MANAGER_MB_NAME_LEN 16

/* Schema version of the central config snapshot; bump when the field layout
 * changes so a future firmware can migrate/interpret older data.
 *   1 -> single MQTT config (Feature 05)
 *   2 -> + mqtt_profiles[3] / mqtt_active_profile (Feature 12)
 *   3 -> + mb_slots[] multi-device RTU master
 *   4 -> CT params: ct_ratio=NCT, + i_rated_a / i_expected_a (R_BURDEN is Kconfig)
 *   5 -> + pga (1/2/4): system-owned current PGA; not stored in calib blob
 *   6 -> + SoftAP SSID/password for config portal (runtime, not Kconfig-only)
 *   7 -> single MQTT broker: mqtt_profiles[]/mqtt_active_profile and the
 *        duplicate legacy scalars are gone. Pre-7 blobs are rejected; NVS is
 *        re-provisioned from defaults (the product is not shipped yet). */
#define CONFIG_MANAGER_VERSION 7

/*
 * One downstream Modbus RTU meter on the shared master bus.
 * type: meter_device_t (0=PM710, 1=EM-07K). name is a short operator label.
 */
typedef struct {
    bool used;       /* slot occupies a list entry (may still be disabled) */
    bool enabled;    /* poll this slot when master bus is on */
    uint8_t type;    /* meter_device_t */
    uint8_t slave_id;/* 1..247, unique among used slots */
    char name[CONFIG_MANAGER_MB_NAME_LEN];
} config_mb_slot_t;

/*
 * How a profile authenticates the TLS session. MQTT_TLS_INSECURE skips server
 * verification entirely and exists for bring-up/debug only — never ship with it.
 * MQTT Runtime consumes this field from the active Configuration Manager profile.
 */
typedef enum {
    MQTT_TLS_DISABLE = 0,   /* plain TCP, no TLS */
    MQTT_TLS_CA_ONLY,       /* verify the broker against a CA */
    MQTT_TLS_MUTUAL,        /* verify the broker + present a client cert/key */
    MQTT_TLS_INSECURE,      /* TLS with no server verification — DEBUG ONLY */
} mqtt_tls_mode_t;

/*
 * The device's ONE MQTT broker: connection parameters, credentials, TLS mode
 * and the certificate paths it uses. enable gates whether the MQTT runtime
 * connects at all; it is changed on the LCD (Settings > MQTT), never on the
 * web portal.
 *
 * Certificates are referenced by PATH only — the PEM contents are never held in
 * this snapshot. Whoever eventually opens the TLS session reads the files at
 * that point; the configuration layer stays small and holds no key material.
 *
 * Named config_mqtt_profile_t, not mqtt_profile_t: config_store.h already
 * defines an mqtt_profile_t with a different legacy persistence layout; both
 * headers land in some of the same translation units.
 */
typedef struct {
    bool enable;

    /* Human-readable label, e.g. "Mosquitto local". The MQTT runtime publishes
     * it as "active_broker" in the heartbeat. */
    char name[CONFIG_MANAGER_MQTT_NAME_LEN];

    char broker[CONFIG_MANAGER_MQTT_BROKER_LEN];
    uint16_t port;

    /* MQTT keepalive in seconds (Feature 12A). */
    uint16_t keepalive_s;

    char username[CONFIG_MANAGER_MQTT_USER_LEN];
    char password[CONFIG_MANAGER_MQTT_PASS_LEN];

    char client_id[CONFIG_MANAGER_MQTT_CLIENT_ID_LEN];

    char publish_topic[CONFIG_MANAGER_MQTT_TOPIC_LEN];
    char subscribe_topic[CONFIG_MANAGER_MQTT_TOPIC_LEN];

    mqtt_tls_mode_t tls_mode;

    /* Filesystem paths, not PEM contents. */
    char ca_path[CONFIG_MANAGER_MQTT_PATH_LEN];
    char cert_path[CONFIG_MANAGER_MQTT_PATH_LEN];
    char key_path[CONFIG_MANAGER_MQTT_PATH_LEN];
} config_mqtt_profile_t;

typedef struct {
    uint32_t config_version;   /* schema version (CONFIG_MANAGER_VERSION) */

    /* ---- System ---- */
    char device_name[CONFIG_MANAGER_DEVICE_NAME_LEN];
    char firmware_version[CONFIG_MANAGER_VERSION_LEN];  /* read-only, from app desc */
    char hardware_version[CONFIG_MANAGER_VERSION_LEN];  /* read-only */

    /* OTA firmware tracking (persisted in NVS, updated after successful OTA) */
    char ota_fw_build[16];      /* Format: "YYMMDD-NN" e.g. "260914-00", default "000000-00" */
    char ota_version[16];       /* Format: "major.minor.patch" e.g. "1.0.0", default "0.0.0" */

    /* ---- Network ---- (backed by config_network_t) */
    bool dhcp_enable;
    char static_ip[CONFIG_MANAGER_IP_LEN];
    char gateway[CONFIG_MANAGER_IP_LEN];
    char netmask[CONFIG_MANAGER_IP_LEN];
    char dns[CONFIG_MANAGER_IP_LEN];

    /* ---- WiFi ---- (backed by config_network_t) */
    char wifi_ssid[CONFIG_MANAGER_SSID_LEN];
    char wifi_pass[CONFIG_MANAGER_PASS_LEN];
    /* SoftAP credentials for Config Portal (editable from web portal).
     * Empty ssid falls back to Kconfig APP_WIFI_METER_AP_SSID at AP start. */
    char ap_ssid[CONFIG_MANAGER_SSID_LEN];
    char ap_pass[CONFIG_MANAGER_PASS_LEN];

    /* ---- MQTT ----
     * Exactly one broker, configured on the web portal and switched on/off on
     * the LCD. mqtt_publish_ms is deliberately outside the broker struct: it is
     * a runtime cadence, not part of how the device identifies itself to the
     * broker, and mqtt_manager refreshes it independently of the broker. */
    config_mqtt_profile_t mqtt;
    uint32_t mqtt_publish_ms;

    /* ---- Modbus RTU: two independent links on two physical UARTs ----
     * SLAVE (this device, UART1): mb_slave_id + mb_slave_baud_code. Edited only
     * on the LCD (Settings > RTU Slave); parity is fixed 8N1. The master stack
     * and portal must never read or write these — the master has no unit id of
     * its own (each request carries mb_slots[i].slave_id).
     * MASTER (downstream meters, UART2): mb_baud_code / mb_parity_code /
     * mb_enabled / mb_poll_period_ms / mb_slots[]. Baud+parity here are bus
     * settings ONLY and no longer affect the slave link.
     * mb_device stays a legacy mirror of slot 0's type for old register maps. */
    uint8_t mb_slave_id;         /* this device's own Modbus unit id (slave) */
    uint8_t mb_slave_baud_code;  /* slave link baud: 0=9600,1=19200,2=38400,3=57600,4=115200 */
    uint8_t mb_baud_code;        /* master bus only: 0=9600,1=19200,2=38400,3=57600,4=115200 */
    uint8_t mb_parity_code;      /* master bus only: 0=none,1=even,2=odd */
    uint8_t mb_stop_bits;        /* RESERVED: no field in firmware yet (default 1) */
    uint8_t mb_device;           /* legacy mirror: first used slot type */
    bool mb_enabled;             /* RTU master bus on/off */
    uint32_t mb_poll_period_ms;  /* full poll cycle period (all slots) */
    config_mb_slot_t mb_slots[CONFIG_MANAGER_MB_SLOT_COUNT];

    /* ---- Measurement ---- */
    uint8_t line_freq;       /* 0=50Hz,1=60Hz */
    uint8_t wiring_mode;     /* 0=3P4W,1=3P3W */
    uint16_t ct_ratio;       /* NCT primary:1 — 1000..6000, step 100 (current CT) */
    uint16_t ct_ratio_calib; /* NCT at calibration time (measurement rescale baseline) */
    uint16_t pt_ratio;       /* RESERVED (PT not used yet) */
    uint16_t i_rated_a;      /* CT nameplate primary current (A) */
    uint16_t i_expected_a;   /* Operator expected max primary (A); true range after Apply */
    /* System-owned PGA for ATM90 current channels: fixed at 4 per thesis requirement.
     * Legacy field kept for config snapshot compatibility. */
    uint8_t pga;

    /* ---- LCD ---- (applied by the Home Screen task) */
    bool lcd_backlight;            /* configured backlight state */
    uint32_t lcd_sleep_timeout_s;  /* idle seconds before auto-sleep; 0 = disabled */
    bool lcd_autocycle;            /* true = Main pages auto-cycle; false = manual navigation */
    uint32_t lcd_cycle_time_ms;    /* auto-cycle period in milliseconds */

    /* ---- Alarm Settings ---- */
    bool alarm_voltage_low_enable;
    bool alarm_voltage_high_enable;
    bool alarm_over_current_enable;
    bool alarm_phase_loss_enable;
    bool alarm_frequency_enable;
    uint8_t alarm_voltage_reference; /* 0=TBD, 1=L-N, 2=L-L */
    uint8_t alarm_nominal_frequency_hz; /* 50 or 60 */
    float alarm_voltage_low_v;
    float alarm_voltage_high_v;
    float alarm_over_current_a;
    float alarm_frequency_low_hz;
    float alarm_frequency_high_hz;
    uint16_t alarm_trigger_delay_s;
    uint16_t alarm_clear_delay_s;  /* reserved: latch has no auto-clear (v1) */
    float alarm_hysteresis;        /* reserved: IC thresholds need no firmware hysteresis */

    /* Output roles + threshold preset. Manual (0) is always available to the
     * user; "alarm" (1) lets the latch edge drive the output ON. The preset
     * records which threshold set is in effect: 0=default, 1=EN 50160,
     * 2=ANSI C84.1 Range A, 3=custom (limits hand-edited). */
    uint8_t alarm_out0_role;       /* OUT1: 0=manual, 1=alarm */
    uint8_t alarm_out1_role;       /* OUT2: 0=manual, 1=alarm */
    uint8_t alarm_preset;

    /* ---- Buzzer ---- */
    bool buzzer_enable;          /* button feedback sound */
    bool buzzer_alarm_enable;    /* persisted preference; alarm backend deferred */
} config_manager_t;

/* Load the snapshot from NVS (via config_store); missing domains fall back to
 * defaults. Creates the internal mutex. Safe to call once early in boot, after
 * NVS is up and before consumers read. Does not apply settings or notify. */
esp_err_t config_manager_init(void);

/* Copy the RAM snapshot out (thread-safe). */
esp_err_t config_manager_get(config_manager_t *out);

/*
 * Copy out the device's single MQTT broker config (the same struct
 * config_apply/mqtt_manager consume). Thread-safe, and cheaper for a caller
 * than config_manager_get() because it copies only this struct instead of the
 * whole snapshot — several callers run on small task stacks.
 * Returns whatever config_manager_get() would (ESP_ERR_INVALID_STATE before
 * the first load, ESP_ERR_INVALID_ARG if out is NULL).
 */
esp_err_t config_manager_get_mqtt(config_mqtt_profile_t *out);

/* Replace the RAM snapshot (thread-safe). Does not write NVS, apply, or notify. */
esp_err_t config_manager_update(const config_manager_t *in);

/* Persist the complete RAM snapshot through Config Store's opaque, versioned
 * full-snapshot blob. Does not Apply, Reload, notify, or alter RAM. */
esp_err_t config_manager_save(void);

/* Reload the RAM snapshot. Prefers the full snapshot; if absent/incompatible,
 * reads legacy Config Store domains and defaults as a migration fallback. */
esp_err_t config_manager_load(void);

/* Erase persisted config (config_store factory reset) and reload defaults. */
esp_err_t config_manager_factory_reset(void);

#ifdef __cplusplus
}
#endif

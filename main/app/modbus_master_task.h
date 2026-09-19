#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "config_manager.h"
#include "esp_err.h"
#include "modbus_meters.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Modbus RTU master on UART (default port 2, RX=GPIO11, TX=GPIO12).
 *
 * Polls up to CONFIG_MANAGER_MB_SLOT_COUNT downstream commercial meters
 * (Schneider PM710 and/or TENSE EM-07K) on one RS485 bus. Bus params and the
 * slot table live in config_manager (mb_enabled / baud / parity / period +
 * mb_slots[]). Each slot has its own type, slave id, name, enabled flag and
 * cached meter_readings_t.
 */

#define MODBUS_MASTER_SLOT_COUNT CONFIG_MANAGER_MB_SLOT_COUNT

/*
 * Tri-state device state, derived in one place (modbus_master_get_slot_status):
 *   ON       - the master is polling and the slot answered its last poll.
 *   OFF      - the master is polling but the slot missed MB_MASTER_OFFLINE_
 *              THRESHOLD (5) consecutive polls: the device is known to be down.
 *   INACTIVE - the master is NOT polling this slot (bus disabled, slot
 *              disabled, or config portal active): the device state is
 *              UNKNOWN, not off.
 * Consumers (LCD, MQTT, console) must never re-derive this from `online`,
 * so "master inactive" and "device off" stay distinguishable everywhere.
 */
typedef enum {
    MODBUS_MASTER_DEV_INACTIVE = 0,  /* master not polling: state unknown */
    MODBUS_MASTER_DEV_ON,            /* answering */
    MODBUS_MASTER_DEV_OFF,           /* polling, offline threshold reached */
} modbus_master_dev_state_t;

/* Per-slot runtime status. */
typedef struct {
    bool used;
    bool enabled;             /* slot enabled in config */
    bool online;              /* last successful poll within offline threshold */
    bool readings_valid;      /* at least one successful decode since start */
    modbus_master_dev_state_t state;  /* tri-state above, derived */
    uint8_t type;             /* meter_device_t */
    uint8_t slave_id;
    char name[CONFIG_MANAGER_MB_NAME_LEN];
    uint32_t poll_count;
    uint32_t error_count;     /* consecutive failures since last success */
    uint32_t reading_age_ms;  /* age of the cached readings (0 = never read) */
} modbus_master_slot_status_t;

/* Bus-level status snapshot. */
typedef struct {
    bool enabled;             /* master bus enabled */
    bool stack_up;            /* RTU stack running */
    uint8_t slot_count;       /* used slots */
    uint8_t online_count;     /* slots currently online */
    uint32_t cycle_count;     /* completed full poll cycles */
    /* Legacy single-device view: first used slot (or zeros). */
    meter_device_t device;
    bool online;
    bool configured;
    uint32_t poll_count;
    uint32_t error_count;
} modbus_master_status_t;

esp_err_t modbus_master_task_start(void);

/* Re-read config and restart the RTU stack if bus params changed. */
esp_err_t modbus_master_reconfigure(void);

/* Slot count capacity (always MODBUS_MASTER_SLOT_COUNT). */
uint8_t modbus_master_slot_capacity(void);

/* Copy latest readings for one slot. ESP_ERR_INVALID_STATE if never valid. */
esp_err_t modbus_master_get_readings_slot(uint8_t slot, meter_readings_t *out);

/* Status of one slot. ESP_ERR_INVALID_ARG if slot out of range. */
esp_err_t modbus_master_get_slot_status(uint8_t slot, modbus_master_slot_status_t *out);

/* "ON" / "OFF" / "INACTIVE" for logs and console UI. */
const char *modbus_master_dev_state_name(modbus_master_dev_state_t state);

/* Bus-level status (includes legacy single-device mirrors). */
esp_err_t modbus_master_get_status(modbus_master_status_t *out);

/*
 * Legacy helpers: first used+enabled online slot, else first used slot with
 * valid readings, else ESP_ERR_INVALID_STATE. Keeps external-calib path working.
 */
esp_err_t modbus_master_get_readings(meter_readings_t *out);

#ifdef __cplusplus
}
#endif

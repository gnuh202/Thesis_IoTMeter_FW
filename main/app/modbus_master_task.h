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

/* Per-slot runtime status. */
typedef struct {
    bool used;
    bool enabled;             /* slot enabled in config */
    bool online;              /* last successful poll within offline threshold */
    bool readings_valid;      /* at least one successful decode since start */
    uint8_t type;             /* meter_device_t */
    uint8_t slave_id;
    char name[CONFIG_MANAGER_MB_NAME_LEN];
    uint32_t poll_count;
    uint32_t error_count;     /* consecutive failures since last success */
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

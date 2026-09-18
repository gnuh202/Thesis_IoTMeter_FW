#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "modbus_master_task.h"

#define MQTT_TELEMETRY_MAX_SLAVES 5

#if defined(CONFIG_MANAGER_MB_SLOT_COUNT) && CONFIG_MANAGER_MB_SLOT_COUNT > MQTT_TELEMETRY_MAX_SLAVES
#warning "CONFIG_MANAGER_MB_SLOT_COUNT exceeds MQTT_TELEMETRY_MAX_SLAVES (5) - only first 5 slaves will be published"
#endif

/* Main device telemetry (ATM90E32AS + IO + warnings) */
typedef struct {
    // Measurements (power in kW/kvar/kVA)
    float voltage[3];           // V
    float current[3];           // A
    float current_neutral;      // A
    float active_power_kw;      // kW (total)
    float reactive_power_kvar;  // kvar (total)
    float apparent_power_kva;   // kVA (total)
    float power_factor[3];      // per phase
    float total_power_factor;
    float frequency;            // Hz
    float temperature;          // °C
    float active_energy_kwh;    // kWh

    // Digital IO
    bool relay_out0;
    bool relay_out1;
    bool digital_in0;
    bool digital_in1;

    // Warning flags (reserved for alarm system)
    uint8_t warning_flags;      // 8 bits: bit set = warning active
} mqtt_telemetry_main_t;

/* Slave device telemetry (PM710/EM07K via Modbus) */
typedef struct {
    char device_name[32];       // from config slot name
    uint8_t slave_id;           // Modbus address
    uint8_t device_type;        // METER_DEV_PM710 or METER_DEV_EM07K
    modbus_master_dev_state_t state;  // ON / OFF / INACTIVE (see modbus_master_task.h)
    bool online;                // convenience flag: true only when state == ON

    // Measurements (power in kW/kvar/kVA). Copied from the slot cache only
    // when trustworthy (state ON and cache valid); otherwise they stay 0 —
    // a lost device publishes the default zeros, never stale numbers.
    float voltage[3];           // V
    float current[3];           // A
    float active_power_kw;      // kW (total)
    float reactive_power_kvar;  // kvar (total, 0 for EM07K)
    float apparent_power_kva;   // kVA (total)
    float power_factor;         // total (0 for EM07K)
    float frequency;            // Hz
    float active_energy_kwh;    // kWh
} mqtt_telemetry_slave_t;

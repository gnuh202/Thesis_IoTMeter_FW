#include "modbus_meters.h"

#include <stddef.h>

/*
 * Downstream meter register maps (reference / optional CID tables).
 *
 * Runtime multi-slot master polls via raw FC03 using the PM710_REG_* /
 * EM07K_REG_* constants in the header. The tables below document the PM710
 * float layout for tooling; EM-07K stays empty (scaled path only).
 *
 * mb_slave_addr is a placeholder — callers that use the table must patch it.
 * mb_reg_start is 0-based (datasheet register number - 1).
 *
 * NOTE: PM710 power CIDs are still in kW/kVAR/kVA at the wire; any consumer
 * of get_parameter must scale *1000 for W/var/VA. The multi-slot master does
 * that in its dedicated PM710 decode path.
 */

static const mb_parameter_descriptor_t s_pm710_table[] = {
    {0, "V1", "V", 1, MB_PARAM_HOLDING, 1059, 2, offsetof(meter_readings_t, voltage[0]),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1060 V A-N */
    {1, "V2", "V", 1, MB_PARAM_HOLDING, 1061, 2, offsetof(meter_readings_t, voltage[1]),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1062 V B-N */
    {2, "V3", "V", 1, MB_PARAM_HOLDING, 1063, 2, offsetof(meter_readings_t, voltage[2]),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1064 V C-N */
    {3, "I1", "A", 1, MB_PARAM_HOLDING, 1033, 2, offsetof(meter_readings_t, current[0]),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1034 I A */
    {4, "I2", "A", 1, MB_PARAM_HOLDING, 1035, 2, offsetof(meter_readings_t, current[1]),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1036 I B */
    {5, "I3", "A", 1, MB_PARAM_HOLDING, 1037, 2, offsetof(meter_readings_t, current[2]),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1038 I C */
    {6, "P", "kW", 1, MB_PARAM_HOLDING, 1005, 2, offsetof(meter_readings_t, active_power),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1006 P total kW */
    {7, "Q", "kVAR", 1, MB_PARAM_HOLDING, 1009, 2, offsetof(meter_readings_t, reactive_power),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1010 Q total kVAR */
    {8, "S", "kVA", 1, MB_PARAM_HOLDING, 1007, 2, offsetof(meter_readings_t, apparent_power),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1008 S total kVA */
    {9, "PF", "-", 1, MB_PARAM_HOLDING, 1011, 2, offsetof(meter_readings_t, power_factor),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1012 PF total */
    {10, "F", "Hz", 1, MB_PARAM_HOLDING, 1019, 2, offsetof(meter_readings_t, frequency),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1020 frequency */
    {11, "E", "kWh", 1, MB_PARAM_HOLDING, 999, 2, offsetof(meter_readings_t, active_energy),
     PARAM_TYPE_FLOAT_ABCD, PARAM_SIZE_FLOAT, {{0, 0, 0}}, PAR_PERMS_READ},   /* 1000 real energy kWh */
};

/* EM-07K: empty — scaled U16 + multi-reg energy needs a dedicated decode path. */
static const mb_parameter_descriptor_t s_em07k_table[] = {
    /* none */
};

const char *modbus_meters_device_name(meter_device_t dev)
{
    switch (dev) {
    case METER_DEV_PM710: return "PM710";
    case METER_DEV_EM07K: return "EM-07K";
    default: return "unknown";
    }
}

esp_err_t modbus_meters_get_descriptors(meter_device_t dev,
                                        const mb_parameter_descriptor_t **table,
                                        uint16_t *count)
{
    if (table == NULL || count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (dev) {
    case METER_DEV_PM710:
        *table = s_pm710_table;
        *count = (uint16_t)(sizeof(s_pm710_table) / sizeof(s_pm710_table[0]));
        return ESP_OK;
    case METER_DEV_EM07K:
        *table = s_em07k_table;
        *count = (uint16_t)(sizeof(s_em07k_table) / sizeof(s_em07k_table[0]));
        return ESP_OK;
    default:
        *table = NULL;
        *count = 0;
        return ESP_ERR_INVALID_ARG;
    }
}

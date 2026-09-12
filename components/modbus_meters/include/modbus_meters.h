#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_modbus_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Register maps for downstream commercial meters read by the RTU master.
 *
 * Supported types (fixed product set):
 *   METER_DEV_PM710  — Schneider PowerLogic PM710 (IEEE float holding regs)
 *   METER_DEV_EM07K  — TENSE EM-07 / EM-07K (U16 + VTR/CTR scale, energy float32)
 *
 * Both decode into the shared meter_readings_t layout (SI-ish units). Fields a
 * meter does not provide stay 0.
 */

typedef enum {
    METER_DEV_PM710 = 0,   /* Schneider PM710 */
    METER_DEV_EM07K,       /* TENSE EM-07K */
    METER_DEV_COUNT,
} meter_device_t;

/*
 * Values decoded from a downstream meter. Units are normalized for app use:
 *   voltage V, current A, power W / var / VA, energy kWh, frequency Hz.
 * PM710 float area reports power in kW/kVAR/kVA — the master converts to W/var/VA.
 * EM-07K has no Q/PF registers (left 0).
 */
typedef struct {
    float voltage[3];        /* L-N voltage per phase (V) */
    float current[3];        /* current per phase (A) */
    float active_power;      /* total active power (W) */
    float reactive_power;    /* total reactive power (var); 0 on EM-07K */
    float apparent_power;    /* total apparent power (VA) */
    float power_factor;      /* total PF; 0 on EM-07K */
    float frequency;         /* line frequency (Hz) */
    float active_energy;     /* imported active energy (kWh) */
} meter_readings_t;

/*
 * Schneider PM710 — Appendix B Table B-2, float metered-data area.
 * Source: PM710 user manual 63230-501-209A1.
 * Datasheet register numbers are 1-based; master uses (reg - 1) for PDU.
 * Each float is 2 holding registers, IEEE-754, word order ABCD (high word first).
 * Power registers are in kW / kVAR / kVA (master scales *1000 → W/var/VA).
 * Energy is already kWh.
 */
#define PM710_REG_ENERGY_KWH     1000  /* Real Energy Total, float, kWh */
#define PM710_REG_P_TOTAL_KW     1006  /* Real Power Total, float, kW */
#define PM710_REG_S_TOTAL_KVA    1008  /* Apparent Power Total, float, kVA */
#define PM710_REG_Q_TOTAL_KVAR   1010  /* Reactive Power Total, float, kVAR */
#define PM710_REG_PF_TOTAL       1012  /* Power Factor Total, float */
#define PM710_REG_FREQ_HZ        1020  /* Frequency, float, Hz */
#define PM710_REG_I_A            1034  /* Current A, float, A */
#define PM710_REG_I_B            1036
#define PM710_REG_I_C            1038
#define PM710_REG_V_AN           1060  /* Voltage A-N, float, V */
#define PM710_REG_V_BN           1062
#define PM710_REG_V_CN           1064

/*
 * EM-07K register map (TENSE EM-07 Modbus table).
 * Source: em07k_user_manual.pdf. Addresses are datasheet DECIMAL values; the
 * master converts to 0-based (address - 1) before reading.
 *
 * Values are raw unsigned 16-bit scaled with VTR/CTR (regs 4000/4001):
 *   voltage = raw * 0.1  * VTR
 *   current = raw * 0.01 * CTR
 *   power   = raw * 1    * CTR * VTR   (Watt / VA on the wire)
 *   freq    = raw * 0.1
 * Energy is float32 high/low words (Wh) per phase — master sums L1+L2+L3 → kWh.
 * No reactive power / power factor in this table.
 */
#define EM07K_REG_VTR          4000  /* voltage transformer ratio, U16 */
#define EM07K_REG_CTR          4001  /* current transformer ratio, U16 */
#define EM07K_REG_VOLT_L1      4002  /* L1-N, U16, x0.1 x VTR */
#define EM07K_REG_VOLT_L2      4003
#define EM07K_REG_VOLT_L3      4004
#define EM07K_REG_CURR_L1      4026  /* U16, x0.01 x CTR */
#define EM07K_REG_CURR_L2      4027
#define EM07K_REG_CURR_L3      4028
#define EM07K_REG_ACTIVE_L1    4042  /* Watt, U16, x1 x CTR x VTR */
#define EM07K_REG_ACTIVE_L2    4043
#define EM07K_REG_ACTIVE_L3    4044
#define EM07K_REG_APPARENT_L1  4072  /* VA, U16, x1 x CTR x VTR */
#define EM07K_REG_APPARENT_L2  4073
#define EM07K_REG_APPARENT_L3  4074
#define EM07K_REG_FREQ_L1      4105  /* Hz, U16, x0.1 */
#define EM07K_REG_ENERGY_L1_HI 4117  /* Wh, float32 high word */
#define EM07K_REG_ENERGY_L1_LO 4118
#define EM07K_REG_ENERGY_L2_HI 4119
#define EM07K_REG_ENERGY_L2_LO 4120
#define EM07K_REG_ENERGY_L3_HI 4121
#define EM07K_REG_ENERGY_L3_LO 4122

/* Human-readable device name for logs/UI. */
const char *modbus_meters_device_name(meter_device_t dev);

/*
 * Optional descriptor table for tools that still want esp-modbus CID maps.
 * Runtime master multi-slot polling uses raw FC03 paths (address per slot).
 * count may be 0 for EM-07K (scaled U16 cannot map 1:1 into float CIDs).
 */
esp_err_t modbus_meters_get_descriptors(meter_device_t dev,
                                        const mb_parameter_descriptor_t **table,
                                        uint16_t *count);

#ifdef __cplusplus
}
#endif

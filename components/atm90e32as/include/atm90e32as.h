#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATM90E32AS_PHASE_COUNT 3

typedef struct atm90e32as_dev_t *atm90e32as_handle_t;

typedef enum {
    ATM90E32AS_PHASE_A = 0,
    ATM90E32AS_PHASE_B,
    ATM90E32AS_PHASE_C,
} atm90e32as_phase_t;

typedef enum {
    ATM90E32AS_LINE_FREQ_50HZ = 0,
    ATM90E32AS_LINE_FREQ_60HZ,
} atm90e32as_line_freq_t;

typedef enum {
    ATM90E32AS_WIRING_3P4W = 0,
    ATM90E32AS_WIRING_3P3W,
} atm90e32as_wiring_mode_t;

typedef enum {
    ATM90E32AS_PGA_GAIN_1X = 0,
    ATM90E32AS_PGA_GAIN_2X,
    ATM90E32AS_PGA_GAIN_4X,
} atm90e32as_pga_gain_t;

typedef enum {
    ATM90E32AS_VOLTAGE_UAN = 0,
    ATM90E32AS_VOLTAGE_UBN,
    ATM90E32AS_VOLTAGE_UCN,
    ATM90E32AS_VOLTAGE_UAB,
    ATM90E32AS_VOLTAGE_UCB,
} atm90e32as_voltage_semantic_t;

/* Phase calibration registers only. No Vref/Iref — factory gains are 0x8000 and
 * fine-tune is auto-cal / manual gain with a one-shot reference argument.
 * PQGain and Phi are stored using their logical signed/cycle representation;
 * the driver encodes them to the ATM90E32AS register format on write. */
typedef struct {
    uint16_t voltage_gain;
    uint16_t current_gain;
    int16_t voltage_offset;
    int16_t current_offset;
    int16_t active_power_offset;
    int16_t reactive_power_offset;
    int16_t pq_gain;
    int16_t phase_comp;
    int16_t fundamental_power_gain;
    uint16_t _abi_pad;
    uint32_t _abi_reserved[2];
} atm90e32as_phase_calib_t;

/*
 * Runtime apply image for the chip.
 * - phase[]     : owned by calib NVS / SD bin format C
 * - pga_gain    : owned by config_manager (CT Apply or dev console); stamped
 *                 into this struct only for atm90e32as_set_calibration()
 * - line_freq   : chip-wide; mirrored in config_manager
 * - wiring_mode : active profile select + relay
 */
typedef struct {
    atm90e32as_line_freq_t line_freq;
    atm90e32as_wiring_mode_t wiring_mode;
    atm90e32as_pga_gain_t pga_gain;
    atm90e32as_phase_calib_t phase[ATM90E32AS_PHASE_COUNT];
} atm90e32as_calib_t;

typedef struct {
    int cs_gpio;
    int spi_clock_hz;
    atm90e32as_calib_t calib;
} atm90e32as_config_t;

typedef struct {
    float voltage[ATM90E32AS_PHASE_COUNT];
    bool voltage_valid[ATM90E32AS_PHASE_COUNT];
    atm90e32as_voltage_semantic_t voltage_semantic[ATM90E32AS_PHASE_COUNT];
    atm90e32as_wiring_mode_t wiring_mode;
    float current[ATM90E32AS_PHASE_COUNT];
    /* Chip-calculated N-line RMS current (IrmsN, not a fourth CT input). */
    float current_neutral;
    float active_power[ATM90E32AS_PHASE_COUNT];
    float reactive_power[ATM90E32AS_PHASE_COUNT];
    float apparent_power[ATM90E32AS_PHASE_COUNT];
    float total_active_power;
    float total_reactive_power;
    float total_apparent_power;
    float power_factor[ATM90E32AS_PHASE_COUNT];
    float total_power_factor;
    float phase_angle[ATM90E32AS_PHASE_COUNT];
    float current_peak[ATM90E32AS_PHASE_COUNT];
    float frequency;
    float temperature;
    uint16_t sys_status0;
    uint16_t sys_status1;
    uint16_t meter_status0;
    uint16_t meter_status1;
} atm90e32as_measurements_t;

/*
 * Energy accumulator counts. The ATM90E32AS total-energy registers are
 * read-to-clear, so each field is the increment since the previous read.
 * Convert to Wh/varh with: count * (10.0f / 3200.0f).
 *
 * valid_mask says which of the four reads actually succeeded
 * (ATM90E32AS_ENERGY_VALID_* bits). Read-to-clear makes this essential: a
 * register that was read has already been zeroed in the chip, so its count
 * exists nowhere else. The caller must accumulate every field whose bit is
 * set — treating a partial read as a total failure silently discards energy
 * that can never be recovered.
 */
typedef struct {
    uint16_t active_import;
    uint16_t active_export;
    uint16_t reactive_import;
    uint16_t reactive_export;
    uint8_t valid_mask;
} atm90e32as_energy_counts_t;

#define ATM90E32AS_ENERGY_VALID_ACTIVE_IMPORT   (1U << 0)
#define ATM90E32AS_ENERGY_VALID_ACTIVE_EXPORT   (1U << 1)
#define ATM90E32AS_ENERGY_VALID_REACTIVE_IMPORT (1U << 2)
#define ATM90E32AS_ENERGY_VALID_REACTIVE_EXPORT (1U << 3)
#define ATM90E32AS_ENERGY_VALID_ALL             (0x0FU)

#define ATM90E32AS_ENERGY_COUNT_TO_WH (10.0f / 3200.0f)

void atm90e32as_get_default_calib(atm90e32as_calib_t *calib);
esp_err_t atm90e32as_validate_calibration(const atm90e32as_calib_t *calib);
esp_err_t atm90e32as_calculate_gain(uint16_t old_gain, float reference, float measured, uint16_t *new_gain);
esp_err_t atm90e32as_create(const atm90e32as_config_t *config, atm90e32as_handle_t *handle);
esp_err_t atm90e32as_delete(atm90e32as_handle_t handle);
esp_err_t atm90e32as_init(atm90e32as_handle_t handle);
esp_err_t atm90e32as_read_register(atm90e32as_handle_t handle, uint16_t reg, uint16_t *value);
esp_err_t atm90e32as_write_register(atm90e32as_handle_t handle, uint16_t reg, uint16_t value);

/*
 * Native warning-threshold API (datasheet 0x06/0x08/0x09/0x0B/0x0C/0x0D).
 * Values are raw comparator thresholds computed by the caller (the datasheet
 * formula xxTh = RmsReg * sqrt(2) * 2^14 / gain applies). write_oi_th gates
 * the OIth write so an unanchored over-current threshold can be omitted.
 * Registers 0x05..0x0D sit in the config space behind CFG_REG_ACC_EN; this
 * helper handles the unlock/lock window internally.
 */
typedef struct {
    uint16_t ov_th;         /* OVth 0x06         */
    uint16_t sag_th;        /* SagTh 0x08        */
    uint16_t phase_loss_th; /* PhaseLossTh 0x09  */
    uint16_t oi_th;         /* OIth 0x0B         */
    uint16_t freq_lo_th;    /* FreqLoTh 0x0C     */
    uint16_t freq_hi_th;    /* FreqHiTh 0x0D     */
    bool write_oi_th;
} atm90e32as_warning_thresholds_t;

esp_err_t atm90e32as_write_warning_thresholds(atm90e32as_handle_t handle,
                                              const atm90e32as_warning_thresholds_t *th);
/* Raw RMS channel values for the ratio threshold anchor (URMS A/B/C, IRMS A/B/C). */
esp_err_t atm90e32as_read_raw_rms(atm90e32as_handle_t handle, uint16_t urms[3], uint16_t irms[3]);
esp_err_t atm90e32as_get_calibration(atm90e32as_handle_t handle, atm90e32as_calib_t *calib);
esp_err_t atm90e32as_set_calibration(atm90e32as_handle_t handle, const atm90e32as_calib_t *calib, bool apply);
esp_err_t atm90e32as_apply_calibration(atm90e32as_handle_t handle, const atm90e32as_calib_t *calib);
esp_err_t atm90e32as_read_measurements(atm90e32as_handle_t handle, atm90e32as_measurements_t *out);
esp_err_t atm90e32as_read_power_raw(atm90e32as_handle_t handle, atm90e32as_phase_t phase,
                                    bool reactive, int32_t *value);
/* Read (and thereby clear) the four total-energy registers. Every register is
 * attempted even when an earlier one fails; out->valid_mask reports which
 * counts are real. Returns ESP_OK when at least one read succeeded, or the
 * first error when all four failed. */
esp_err_t atm90e32as_read_energy_counts(atm90e32as_handle_t handle, atm90e32as_energy_counts_t *out);

#ifdef __cplusplus
}
#endif

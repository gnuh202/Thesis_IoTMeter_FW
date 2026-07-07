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
    ATM90E32AS_PGA_GAIN_1X = 0x0000,
    ATM90E32AS_PGA_GAIN_2X = 0x0015,
    ATM90E32AS_PGA_GAIN_4X = 0x002A,
} atm90e32as_pga_gain_t;

typedef struct {
    uint16_t voltage_gain;
    uint16_t current_gain;
    int16_t voltage_offset;
    int16_t current_offset;
    int16_t active_power_offset;
    int16_t reactive_power_offset;
    uint16_t pq_gain;
    int16_t phase_comp;
    uint16_t fundamental_power_gain;
    float reference_voltage;
    float reference_current;
} atm90e32as_phase_calib_t;

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
    float current[ATM90E32AS_PHASE_COUNT];
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
 */
typedef struct {
    uint16_t active_import;
    uint16_t active_export;
    uint16_t reactive_import;
    uint16_t reactive_export;
} atm90e32as_energy_counts_t;

#define ATM90E32AS_ENERGY_COUNT_TO_WH (10.0f / 3200.0f)

void atm90e32as_get_default_calib(atm90e32as_calib_t *calib);
esp_err_t atm90e32as_create(const atm90e32as_config_t *config, atm90e32as_handle_t *handle);
esp_err_t atm90e32as_delete(atm90e32as_handle_t handle);
esp_err_t atm90e32as_init(atm90e32as_handle_t handle);
esp_err_t atm90e32as_read_register(atm90e32as_handle_t handle, uint16_t reg, uint16_t *value);
esp_err_t atm90e32as_write_register(atm90e32as_handle_t handle, uint16_t reg, uint16_t value);
esp_err_t atm90e32as_get_calibration(atm90e32as_handle_t handle, atm90e32as_calib_t *calib);
esp_err_t atm90e32as_set_calibration(atm90e32as_handle_t handle, const atm90e32as_calib_t *calib, bool apply);
esp_err_t atm90e32as_apply_calibration(atm90e32as_handle_t handle, const atm90e32as_calib_t *calib);
esp_err_t atm90e32as_read_measurements(atm90e32as_handle_t handle, atm90e32as_measurements_t *out);
esp_err_t atm90e32as_read_energy_counts(atm90e32as_handle_t handle, atm90e32as_energy_counts_t *out);

#ifdef __cplusplus
}
#endif

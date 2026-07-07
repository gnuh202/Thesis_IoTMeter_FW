#pragma once

#include <stdbool.h>
#include "atm90e32as.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float active_import_kwh;
    float active_export_kwh;
    float reactive_import_kvarh;
    float reactive_export_kvarh;
} energy_meter_energy_t;

typedef struct {
    float active_power_demand_w;
    float active_power_demand_max_w;
} energy_meter_demand_t;

esp_err_t energy_meter_task_start(void);
esp_err_t energy_meter_get_latest(atm90e32as_measurements_t *out);
bool energy_meter_has_latest(void);
esp_err_t energy_meter_get_energy(energy_meter_energy_t *out);
esp_err_t energy_meter_get_demand(energy_meter_demand_t *out);
esp_err_t energy_meter_reset_energy(void);
esp_err_t energy_meter_reset_demand(void);
esp_err_t energy_meter_set_demand_window_minutes(uint16_t minutes);
esp_err_t energy_meter_read_register(uint16_t reg, uint16_t *value);
esp_err_t energy_meter_write_register(uint16_t reg, uint16_t value);
esp_err_t energy_meter_get_calibration(atm90e32as_calib_t *calib);
esp_err_t energy_meter_set_calibration(const atm90e32as_calib_t *calib);
esp_err_t energy_meter_apply_calibration(void);
esp_err_t energy_meter_save_calibration(void);
esp_err_t energy_meter_load_calibration(bool apply);
esp_err_t energy_meter_reset_calibration_defaults(bool apply);

#ifdef __cplusplus
}
#endif

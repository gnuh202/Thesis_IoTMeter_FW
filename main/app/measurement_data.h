#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Central measurement data model.
 *
 * A pure in-RAM snapshot of the latest electrical measurements. It talks to no
 * hardware, reads no SPI, and does not touch the ATM90E32AS driver. The
 * measurement task decodes the meter and calls measurement_data_update(); every
 * other module reads through measurement_data_get() instead of reading the
 * meter directly.
 *
 * Fields the current hardware/driver does not provide (THD, MCU temperature,
 * apparent energy) are reserved and left at 0 until a source exists.
 */

typedef struct {
    /* Voltage (V). */
    float voltage_l1;
    float voltage_l2;
    float voltage_l3;
    float voltage_avg;
    float voltage_uab;
    float voltage_ucb;
    uint8_t voltage_valid_mask; /* bit0/1/2 correspond to voltage_l1/l2/l3 */
    uint8_t wiring_mode;        /* 0=3P4W: Uan/Ubn/Ucn, 1=3P3W: Uab/unused/Ucb */

    /* Current (A). */
    float current_l1;
    float current_l2;
    float current_l3;
    /* Chip-calculated N-line RMS current (IrmsN); no fourth CT input. */
    float current_neutral;
    /* Arithmetic mean of the three current array entries; no special metering meaning. */
    float current_avg;

    /* Frequency (Hz). */
    float frequency;

    /* Active power (W). */
    float p1;
    float p2;
    float p3;
    float p_total;

    /* Reactive power (var). */
    float q1;
    float q2;
    float q3;
    float q_total;

    /* Apparent power (VA). */
    float s1;
    float s2;
    float s3;
    float s_total;

    /* Power factor. */
    float pf1;
    float pf2;
    float pf3;
    float pf_total;

    /* Energy. */
    float energy_import;           /* kWh */
    float energy_export;           /* kWh */
    float energy_reactive_import;  /* kvarh */
    float energy_reactive_export;  /* kvarh */
    float energy_apparent;         /* kVAh, reserved */

    /* THD (reserved until a source exists). */
    float voltage_thd;             /* %, reserved */
    float current_thd;             /* %, reserved */

    /* Temperature (degC). */
    float temp_atm90;
    float temp_mcu;                /* reserved */
    float temp_reserved;           /* reserved */

    /* Timestamp of the last update (esp_timer microseconds since boot). */
    uint64_t last_update_us;

    /* True once at least one update has been stored. */
    bool valid;
} measurement_data_t;

/* Create the internal mutex. Safe to call more than once. Call before the first
 * update/get (the measurement task's init path does this). */
esp_err_t measurement_data_init(void);

/* Replace the stored snapshot with *in (thread-safe copy). */
esp_err_t measurement_data_update(const measurement_data_t *in);

/* Copy the latest snapshot into *out (thread-safe). Returns ESP_ERR_INVALID_STATE
 * if no update has been stored yet; *out is still zeroed in that case. */
esp_err_t measurement_data_get(measurement_data_t *out);

#ifdef __cplusplus
}
#endif

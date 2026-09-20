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

typedef enum {
    ENERGY_METER_CALIB_REFERENCE_MANUAL = 0,
    ENERGY_METER_CALIB_REFERENCE_EXTERNAL,
} energy_meter_calib_reference_t;

typedef struct {
    atm90e32as_phase_t phase;
    bool current;
    /* When true the request measures the no-load reading (0V/0A applied) and
     * writes the voltage/current offset register instead of the gain. Reference
     * source/manual_reference/tolerance_percent are ignored in this mode. */
    bool calibrate_offset;
    energy_meter_calib_reference_t source;
    float manual_reference;
    uint16_t samples;
    uint32_t settle_ms;
    float tolerance_percent;
} energy_meter_auto_calib_request_t;

typedef struct {
    float reference;
    float measured_before;
    float measured_after;
    uint16_t old_gain;
    uint16_t new_gain;
    int16_t old_offset;
    int16_t new_offset;
    bool offset_calibrated;
    atm90e32as_pga_gain_t old_pga;
    atm90e32as_pga_gain_t new_pga;
    uint8_t iterations;
    bool rolled_back;
    bool pga_increased;
} energy_meter_auto_calib_result_t;

esp_err_t energy_meter_auto_calibrate(const energy_meter_auto_calib_request_t *request,
                                      energy_meter_auto_calib_result_t *result);

/* Multi-phase U/I gain (or offset) calibration in a single command.
 *
 * phase_mask selects the phases to calibrate (bit0=A, bit1=B, bit2=C). Omitting
 * --phase at the console calibrates all three with one shared reference (one
 * single-phase AC source + neutral wired to every voltage channel, or three CTs
 * clamped on the same load). All selected phases are captured from the SAME DSP
 * snapshot and committed with ONE chip write.
 *
 * Single-shot: no iterative convergence loop. The gain is computed from the
 * measured value, applied once, then re-measured purely to report the residual
 * error per phase (a WARN, never a rollback). A rollback happens only when a
 * compute/apply/SPI step fails — and then for ALL phases (all-or-nothing).
 *
 * Calibration is blocked entirely in 3P3W (returns ESP_ERR_NOT_SUPPORTED): a
 * neutral is required and the per-phase gains are shared across wiring modes, so
 * calibrating in 3P4W is correct for 3P3W too. */
#define ENERGY_METER_PHASE_MASK_A (1u << ATM90E32AS_PHASE_A)
#define ENERGY_METER_PHASE_MASK_B (1u << ATM90E32AS_PHASE_B)
#define ENERGY_METER_PHASE_MASK_C (1u << ATM90E32AS_PHASE_C)
#define ENERGY_METER_PHASE_MASK_ALL \
    (ENERGY_METER_PHASE_MASK_A | ENERGY_METER_PHASE_MASK_B | ENERGY_METER_PHASE_MASK_C)

typedef struct {
    uint8_t phase_mask;            /* ENERGY_METER_PHASE_MASK_* bits; 1..0x07 */
    bool current;                  /* true = current gain/offset, false = voltage */
    bool calibrate_offset;
    energy_meter_calib_reference_t source;
    float manual_reference;        /* shared reference for all masked phases */
    uint16_t samples;
    uint32_t settle_ms;
    float tolerance_percent;
} energy_meter_multi_calib_request_t;

typedef struct {
    float reference;
    float measured_before;
    float measured_after;
    float error_percent;           /* |after - ref| / ref * 100; gain path only */
    uint16_t old_gain;
    uint16_t new_gain;
    int16_t old_offset;
    int16_t new_offset;
    bool done;                     /* this phase was part of the calibration */
} energy_meter_multi_phase_result_t;

typedef struct {
    uint8_t calibrated_mask;       /* phases committed on success */
    bool rolled_back;
    bool offset_calibrated;
    atm90e32as_pga_gain_t pga;     /* PGA in effect (never changed by auto-cal) */
    energy_meter_multi_phase_result_t phase[ATM90E32AS_PHASE_COUNT];
} energy_meter_multi_calib_result_t;

esp_err_t energy_meter_auto_calibrate_multi(const energy_meter_multi_calib_request_t *request,
                                            energy_meter_multi_calib_result_t *result);

typedef struct {
    atm90e32as_phase_t phase;
    float reference_w;
    uint16_t samples;
    uint32_t settle_ms;
    float tolerance_percent;
} energy_meter_pq_gain_request_t;

typedef struct {
    float reference;
    float measured_before;
    float measured_after;
    int16_t old_pq_gain;
    int16_t new_pq_gain;
    bool rolled_back;
} energy_meter_pq_gain_result_t;

esp_err_t energy_meter_auto_calibrate_pq_gain(const energy_meter_pq_gain_request_t *request,
                                              energy_meter_pq_gain_result_t *result);

/* Phase-angle (Phi) calibration per AN46103 §4.2.7, measured at PF=0.5L with the
 * phase's rated current. Production calibrates once: the caller must first run
 * `meter-cal default --field phi` so phase_comp is 0, because this writes the
 * computed Phi directly (no composition with any existing value). If the applied
 * phase_comp is non-zero the call rejects with ESP_ERR_INVALID_STATE. */
typedef struct {
    atm90e32as_phase_t phase;
    float reference_w;          /* P_ref active power at PF=0.5L (W) */
    uint16_t samples;           /* P_chip averaging samples (default 3) */
    uint32_t settle_ms;         /* DSP settle after write (default 700) */
    float tolerance_percent;    /* residual error PASS/FAIL bound (default 1.0) */
} energy_meter_phase_calib_request_t;

typedef struct {
    float reference;
    float measured_before;      /* P_chip at Phi=0 (W) */
    float measured_after;       /* P_chip after write (W) */
    int16_t old_phase_comp;     /* always 0 here (non-zero is rejected) */
    int16_t new_phase_comp;     /* Phi written, ±255 cycles @2.048MHz */
    uint32_t gphase_x1000;      /* Gphase*1000 selected from line_freq */
    float phase_angle_after;    /* PAngle (deg) — sanity log only, not PASS/FAIL */
    bool rolled_back;
} energy_meter_phase_calib_result_t;

esp_err_t energy_meter_auto_calibrate_phase(const energy_meter_phase_calib_request_t *request,
                                            energy_meter_phase_calib_result_t *result);

typedef enum {
    ENERGY_METER_POWER_OFFSET_ACTIVE = 0,
    ENERGY_METER_POWER_OFFSET_REACTIVE,
} energy_meter_power_offset_type_t;

typedef struct {
    atm90e32as_phase_t phase;
    energy_meter_power_offset_type_t type;
    uint16_t samples;
    uint32_t settle_ms;
    int32_t residual_tolerance_counts;
} energy_meter_power_offset_request_t;

typedef struct {
    int16_t old_offset;
    int16_t new_offset;
    int32_t average_before_counts;
    int32_t average_after_counts;
    bool offset_calibrated;
    bool rolled_back;
} energy_meter_power_offset_result_t;

esp_err_t energy_meter_auto_calibrate_power_offset(
    const energy_meter_power_offset_request_t *request,
    energy_meter_power_offset_result_t *result);

/* Get average active power measurement from the chip (not reference meter).
 * Returns integer milliwatts for deterministic math. Used for manual
 * calibration workflows where P_chip is measured separately.
 * Reads from cached measurements (updated every 100ms by task), interval_ms
 * controls spacing between samples (1-1000ms, default 100ms). */
esp_err_t energy_meter_get_average_active_power(atm90e32as_phase_t phase,
                                                uint16_t samples,
                                                uint16_t interval_ms,
                                                int64_t *average_mw);

esp_err_t energy_meter_task_start(void);
esp_err_t energy_meter_get_latest(atm90e32as_measurements_t *out);
bool energy_meter_has_latest(void);
esp_err_t energy_meter_get_energy(energy_meter_energy_t *out);
esp_err_t energy_meter_get_demand(energy_meter_demand_t *out);
esp_err_t energy_meter_reset_energy(void);
esp_err_t energy_meter_reset_demand(void);
/* Commit the RAM energy accumulators (and the time floor) to NVS right now.
 * Call before any controlled reboot or before the AP portal takes the device
 * out of normal operation; the periodic writer only runs inside the task loop. */
esp_err_t energy_meter_flush_persist(void);
esp_err_t energy_meter_set_demand_window_minutes(uint16_t minutes);
esp_err_t energy_meter_get_demand_window_minutes(uint16_t *out_minutes);
esp_err_t energy_meter_read_register(uint16_t reg, uint16_t *value);
esp_err_t energy_meter_write_register(uint16_t reg, uint16_t value);
esp_err_t energy_meter_get_calibration(atm90e32as_calib_t *calib);
/* Gains/offsets/phase only. pga_gain and line_freq in *calib are ignored —
 * those are chip-wide stamps. PGA is owned by config_manager (CT Apply or
 * energy_meter_set_pga_gain); line_freq via energy_meter_set_line_freq. */
esp_err_t energy_meter_set_calibration(const atm90e32as_calib_t *calib);
/* System PGA (config-owned). Product path: CT Apply. Dev path: console set
 * --field pga. Both stamp the chip and persist config_manager.pga. */
esp_err_t energy_meter_set_pga_gain(atm90e32as_pga_gain_t pga, bool apply);
esp_err_t energy_meter_set_line_freq(atm90e32as_line_freq_t freq, bool apply);
esp_err_t energy_meter_apply_calibration(void);
/* Switch the active wiring mode (drives MODE_SEL relay + chip mode bit only).
 * Phase gains are SHARED across wiring modes — switching never touches them.
 * line_freq/pga stay chip-wide. */
esp_err_t energy_meter_set_wiring_mode(atm90e32as_wiring_mode_t mode, bool apply);
esp_err_t energy_meter_save_calibration(void);
esp_err_t energy_meter_load_calibration(bool apply);
esp_err_t energy_meter_reset_calibration_defaults(bool apply);
/* Erase the persisted calibration blob (factory reset). Safe to call before the
 * meter task exists; the next boot then uses the bring-up defaults. */
esp_err_t energy_meter_erase_calibration(void);

/* Export current RAM calibration state (single profile, no version) into an
 * NVS-compatible blob. out_len receives the actual blob size on success. */
esp_err_t energy_meter_export_blob(uint8_t *out, size_t out_cap, size_t *out_len);

/* Import and validate a calibration blob (single-profile layout, no version).
 * Checks magic and validates via atm90e32as_validate_calibration.
 *   apply=true     → install to chip registers + relay
 *   save_nvs=true  → persist to NVS after validation
 * Returns ESP_ERR_INVALID_VERSION, ESP_ERR_INVALID_ARG on bad blob. */
esp_err_t energy_meter_import_blob(const uint8_t *in, size_t len, bool apply, bool save_nvs);

/* Pack current calibration (single profile) into portable CALB file.
 * Phase cal fields only — U/I gain+offset, phase_comp, P/Q offset, pq_gain,
 * fundamental_power_gain ×3. No PGA/freq/CT/refs in bin. */
esp_err_t calib_backup_pack_single(uint8_t *file_out, size_t cap, size_t *file_len);

/* Pack current calibration into a portable backup file (header + payload + CRC32).
 * File format: magic 'CALB', no version, wiring tag, CRC32. */
esp_err_t calib_backup_pack(uint8_t *file_out, size_t cap, size_t *file_len);

/* Unpack and validate backup file, then import phase cal fields.
 * Validates file magic, length, and CRC32. Accepts legacy v1/v2 files by
 * payload size for read-back compatibility.
 * apply_to_current: if true, apply file's calibration to current mode.
 *                   if false, only stash gains in the single profile without applying.
 * save_nvs: persist to NVS after import
 * Never overrides running PGA / line_freq / CT config (system-owned).
 * Returns ESP_ERR_INVALID_VERSION, ESP_ERR_INVALID_CRC, or import errors. */
esp_err_t calib_backup_unpack(const uint8_t *file_in, size_t file_len, bool apply_to_current, bool save_nvs);

/* Generate human-readable CSV metadata for current calibration + CT snapshot.
 * Display-only companion to the bin; not authoritative for restore. */
esp_err_t calib_backup_json(char *json_out, size_t cap, size_t *json_len);

/* ---- Current CT setup / PGA (fixed 4×) ----
 * R_BURDEN is Kconfig (board fixed, 4.4Ω). VADC limit = 720 mVrms.
 * PGA is locked at 4× per thesis requirement: CT swaps rescale digitally
 * without recalibration. Ilim = 0.72 * NCT / (4.4 * 4) [primary A].
 *
 * expected_clamped: true when Ilim(PGA=4) < I_Expected (operator must lower
 *   Expected or use higher-ratio CT; cannot increase PGA).
 * rated_truncated: true when Ilim < I_Rated (headroom warning; PGA stays 4). */

typedef struct {
    uint16_t ct_ratio;      /* NCT primary:1 — 1000..6000 step 100 */
    uint16_t i_rated_a;     /* CT nameplate primary (A) */
    uint16_t i_expected_a;  /* Operator expected max primary (A) — never mutated */
    atm90e32as_pga_gain_t pga;
    float ilim_a;           /* Ilim of selected PGA (primary A) */
    /* True when Ilim(PGA=4) cannot cover I_Expected. Expected is left
     * unchanged in i_expected_a; the caller should show a range warning. */
    bool expected_clamped;
    /* True when Ilim < I_Rated (headroom warning; PGA stays 4). */
    bool rated_truncated;
} energy_meter_ct_apply_result_t;

/* Pure compute: pick PGA + optional Expected clamp. Does not touch hardware. */
esp_err_t energy_meter_ct_select_pga(uint16_t ct_ratio, uint16_t i_rated_a,
                                     uint16_t i_expected_a,
                                     energy_meter_ct_apply_result_t *out);

/* Convert PGA enum to the integer × multiplier used by the Ilim formula
 * (1, 2 or 4). Used by LCD / console display formatting. */
unsigned energy_meter_pga_mult(atm90e32as_pga_gain_t pga);

/* Apply CT setup:
 *   - compute PGA (always 4×), flag Expected/Rated warnings if Ilim insufficient
 *   - stamp PGA=4 to chip (idempotent)
 *   - Igain kept (CT is just a ratio; measurement rescale handles NCT changes)
 *   - does NOT persist config_manager CT fields (caller saves those)
 *   - save_calib_nvs: persist calib blob after apply
 * out may be NULL. */
esp_err_t energy_meter_ct_apply(uint16_t ct_ratio, uint16_t i_rated_a,
                                uint16_t i_expected_a,
                                bool save_calib_nvs,
                                energy_meter_ct_apply_result_t *out);

#ifdef __cplusplus
}
#endif

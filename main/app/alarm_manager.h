/*
 * alarm_manager.h — ATM90E32AS native warning backend (latching alarm system).
 *
 * The IC compares voltage/current/frequency against its own threshold
 * registers (OVth 0x06, SagTh 0x08, PhaseLossTh 0x09, OIth 0x0B,
 * FreqLoTh/FreqHiTh 0x0C/0x0D) and exposes the real-time verdicts in the
 * EMM status registers (EMMState0 0x71 / EMMState1 0x72), which the driver
 * already reads every measurement cycle. This module:
 *
 *   1. anchors the IC thresholds from live measurements (ratio method: the
 *      raw Urms/Irms register value at a known reading absorbs the channel
 *      LSB and gain, so no hardcoded scale constant is needed),
 *   2. polls the status bits each cycle (no ISR; the IRQ0/IRQ1 pins stay
 *      enabled for hardware observers, we simply do not rely on them),
 *   3. latches alarms after a configurable continuous-confirm window,
 *   4. drives the outputs whose role is "alarm" ON at the latch edge only
 *      (the user is the highest authority and can override at any time;
 *      alarm never switches anything OFF by itself),
 *   5. clears latches ONLY on an explicit reset (LCD: Alarm > Reset Latch);
 *      a persisting fault re-latches after its confirm window.
 *
 * Concurrency: alarm_manager_service() / alarm_manager_apply_ic() run in the
 * energy-meter task (the caller serialises IC access with the meter mutex).
 * The getters, alarm_manager_request_apply() and alarm_manager_reset_latch()
 * are called from HMI/MQTT tasks and are guarded internally.
 */
#pragma once

#include <stdint.h>
#include "atm90e32as.h"
#include "config_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Latch bitmap (uint16). Per-phase detail lives here; it drives the LCD ALARMS
 * page and is published on MQTT as telemetry.main.warn_bits. The MQTT
 * `warnings` byte is the coarser category mapping of the same state
 * (see alarm_manager_warning_byte). */
typedef enum {
    ALARM_BIT_OV_A = 0, ALARM_BIT_OV_B, ALARM_BIT_OV_C,      /* b0..2  */
    ALARM_BIT_OC_A, ALARM_BIT_OC_B, ALARM_BIT_OC_C,          /* b3..5  */
    ALARM_BIT_UV_A, ALARM_BIT_UV_B, ALARM_BIT_UV_C,          /* b6..8  */
    ALARM_BIT_PL_A, ALARM_BIT_PL_B, ALARM_BIT_PL_C,          /* b9..11 */
    ALARM_BIT_FREQ_HI, ALARM_BIT_FREQ_LO,                    /* b12,13 */
    ALARM_BIT_IC_ERROR,                                      /* b14 (WarnOut pin) */
    ALARM_BIT_COUNT                                          /* 15 */
} alarm_manager_bit_t;

/* Queue a threshold re-anchor + IC register write on the next energy-task
 * cycle (called by config_apply on CONFIG_APPLY_ALARM and after calibration). */
void alarm_manager_request_apply(void);

/* True while the module still needs meter-mutex IC access (initial anchor,
 * apply request, or over-current anchor waiting for load current). */
bool alarm_manager_ic_access_pending(void);

/* Anchor thresholds from *m and write them to the IC. Caller MUST hold the
 * meter mutex (it performs SPI reads/writes). cfg supplies the alarm config;
 * calib supplies the per-phase gain registers used by the datasheet formula
 * xxTh = RmsReg * sqrt(2) * 2^14 / gain. Idempotent; cheap when already armed. */
void alarm_manager_apply_ic(atm90e32as_handle_t handle, const atm90e32as_calib_t *calib,
                            const config_manager_t *cfg, const atm90e32as_measurements_t *m);

/* Evaluate one measurement snapshot: decode EMMState0/1 warning bits, run the
 * confirm window, latch, and edge-drive alarm-role outputs. No SPI access. */
void alarm_manager_service(const atm90e32as_measurements_t *m);

/* User action: clear every latch and switch alarm-driven outputs OFF. */
void alarm_manager_reset_latch(void);

/* Number of latched alarm bits (drives the 5th LED). */
int alarm_manager_active_count(void);

/* Latched bitmap (ALARM_BIT_* layout), for the LCD ALARMS page. */
uint16_t alarm_manager_latched_bitmap(void);

/* MQTT telemetry `warnings` byte — the per-category summary of the latched
 * bitmap above (each bit ORs the three phases together). Latched, not
 * instantaneous: it stays set until Reset Latch even if the fault has cleared.
 *
 *   b0 0x01  over-voltage, any phase      (ALARM_BIT_OV_A..C)
 *   b1 0x02  under-voltage / sag, any     (ALARM_BIT_UV_A..C)
 *   b2 0x04  over-current, any phase      (ALARM_BIT_OC_A..C)
 *   b3 0x08  phase loss, any phase        (ALARM_BIT_PL_A..C)
 *   b4 0x10  frequency high               (ALARM_BIT_FREQ_HI)
 *   b5 0x20  frequency low                (ALARM_BIT_FREQ_LO)
 *   b6 0x40  IC fatal error               (ALARM_BIT_IC_ERROR, WarnOut pin)
 *   b7 0x80  unused, always 0
 *
 * Which phase tripped is in alarm_manager_latched_bitmap(), published beside
 * this byte as `warn_bits`. Full reference: docs/mqtt_payloads.md section 3.1. */
uint8_t alarm_manager_warning_byte(void);

#ifdef __cplusplus
}
#endif

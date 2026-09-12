#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Modbus RTU slave for the ATM90E32AS-based 3-phase meter.
 * Register map is documented in docs/modbus_slave_register_map.md.
 *
 * The slave runs always-on: it is brought up once at boot from
 * config_manager values (with Kconfig defaults as a fallback for the very
 * first boot, before NVS holds a snapshot) and torn down / rebuilt on demand
 * via modbus_slave_reconfigure() when the operator changes ID or baud.
 */

/* Start the slave task + RTU stack with the current config_manager values. */
esp_err_t modbus_slave_task_start(void);

/*
 * Schedule a teardown + rebuild of the RTU stack using the latest
 * config_manager snapshot and wait for the result. The owner task performs
 * destruction only after TX drain plus an RTU silent interval, so the write
 * acknowledgement that caused a configuration change remains intact. Returns
 * ESP_ERR_INVALID_STATE if the stack has not started, ESP_ERR_TIMEOUT if the
 * owner task cannot complete within its bounded wait, otherwise the rebuild
 * result.
 */
esp_err_t modbus_slave_reconfigure(void);

/* Schedule a clean firmware restart after the current Modbus response has time
 * to leave the UART. New ID/baud settings are loaded on the next boot. */
esp_err_t modbus_slave_request_restart(void);

/*
 * Map a baud_code (0..4 in config_manager) to the integer baud rate used
 * by the UART driver. Returns 9600 for any code outside that range.
 */
uint32_t modbus_slave_baud_from_code(uint8_t code);

/*
 * Live slave diagnostics, snapshotted on demand for the `mb-slave-diag`
 * console command. All counters are monotonically increasing from the
 * last stack_start; they reset to zero on every reconfigure.
 *
 *   events_*_rd / _wr  — number of register access events of that type
 *                        drained from the library's notification queue.
 *   queue_overflow_drops — number of times the library had to drop a
 *                        notification because the queue was full (the
 *                        lib blocks for 10 ms trying to push, then gives
 *                        up). Counts only events that actually exist; if
 *                        this climbs, the app event mask is incomplete
 *                        (events arrive but no one consumes the queue).
 *   time_since_last_req_ms — ms since the most recent register access.
 *                        Useful as a "master went silent" indicator.
 *   stack_up        — copy of s_stack_up at snapshot time.
 */
typedef struct {
    uint32_t events_holding_rd;
    uint32_t events_holding_wr;
    uint32_t events_input_rd;
    uint32_t events_coils_rd;
    uint32_t events_coils_wr;
    uint32_t events_discrete_rd;
    uint32_t queue_overflow_drops;
    uint32_t time_since_last_req_ms;
    uint8_t  stack_up;
} modbus_slave_diag_t;

void modbus_slave_get_diag(modbus_slave_diag_t *out);

/*
 * Toggle verbose esp-modbus logging (MB_SERIAL, MB_CONTROLLER_SLAVE,
 * MBS_TIMER). Default OFF — the library's per-frame DEBUG output is
 * off-by-one ("RX: N+1 bytes" instead of "RX: N bytes") and floods the
 * console. Use this to enable detailed logging only when actively
 * chasing a bus issue. Survives reconfigure (re-applied on every
 * slave_stack_start).
 */
void modbus_slave_set_verbose_logging(bool enable);
bool modbus_slave_get_verbose_logging(void);

#ifdef __cplusplus
}
#endif

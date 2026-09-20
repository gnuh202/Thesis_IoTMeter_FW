#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Modbus TCP server for the EVN rooftop-PV monitoring spec (QD-EVN 12/2024,
 * "He thong thu thap, giam sat, dieu khien DMTMN").
 *
 * Deliberately NOT built on esp-modbus: that component (v1.0.18) keeps a single
 * process-wide slave instance (freemodbus/common/esp_modbus_slave.c:37 and the
 * global FSM in freemodbus/modbus/mb.c:69), so a second mbc_slave_init() would
 * tear down the RS485 slave in modbus_slave_task.c. The EVN profile only needs
 * FC 01/02/03/04/05/06 over a plain socket, which is small enough to own here.
 *
 * Control commands are accepted and persisted, not acted on: this device is a
 * meter, not an inverter controller. modbus_tcp_get_control() is the seam where
 * a future stage can forward the setpoint downstream over the RTU master.
 */

/* Latest control state received from the DSO, restored from NVS at boot so a
 * comms loss or a reboot keeps the last setpoint (spec section A.2). */
typedef struct {
    bool p_control_enabled;   /* coil, doc address 11 */
    bool q_control_enabled;   /* coil, doc address 15 */
    uint16_t p_setpoint_raw;  /* holding, doc address 13 — raw, see .c */
    uint16_t q_setpoint_raw;  /* holding, doc address 17 — raw, see .c */
} modbus_tcp_control_t;

esp_err_t modbus_tcp_task_start(void);

esp_err_t modbus_tcp_get_control(modbus_tcp_control_t *out);

#ifdef __cplusplus
}
#endif

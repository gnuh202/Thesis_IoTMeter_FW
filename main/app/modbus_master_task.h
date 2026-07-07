#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Modbus RTU master on UART (RX=GPIO11, TX=GPIO12).
 *
 * Not implemented yet. Purpose reserved for future use, e.g. polling
 * downstream Modbus devices (auxiliary meters, sensors, PLCs) and merging
 * their data into this device. Currently only reserves configuration.
 */
esp_err_t modbus_master_task_start(void);

#ifdef __cplusplus
}
#endif

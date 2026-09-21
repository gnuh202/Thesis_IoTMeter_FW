#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * DS1307 real-time clock (I2C, fixed address 0x68, BCD registers).
 *
 * A thin register driver: it knows BCD, the CH oscillator bit and the 7 clock
 * registers, and nothing else. It does not touch the system clock and does not
 * know about time_source — app/time_source.c owns the policy (which reading to
 * trust, when to resync from the network, what quality flag to publish).
 *
 * Two DS1307 traits shape the API:
 *
 *  - The year register holds two digits with no century bit, so the driver
 *    maps it to 2000..2099. Anything outside that cannot be represented.
 *  - CH (register 0x00 bit 7) is set when the oscillator is halted. It is NOT
 *    an "oscillator stopped since you last looked" flag like the DS3231's OSF:
 *    a dead backup battery leaves CH clear and the chip happily reports a stale
 *    or garbage date. So ds1307_get_time() succeeding is necessary but not
 *    sufficient — the caller still has to sanity-check the value it gets.
 */

/* Fixed by the chip; no address pins. */
#define DS1307_I2C_ADDRESS 0x68

/* Representable year range (two BCD digits, century assumed 20xx). */
#define DS1307_YEAR_MIN 2000
#define DS1307_YEAR_MAX 2099

typedef struct ds1307_dev_t *ds1307_handle_t;

/*
 * Attach to an already-initialized I2C master bus. Follows the same create /
 * delete + per-device mutex contract as the other bus drivers in components/:
 * every failure path unwinds completely, so a failed create leaks nothing and
 * leaves *handle untouched.
 *
 * A successful create only means the device answered on the bus; it says
 * nothing about whether the time it holds is usable. Probe with
 * ds1307_is_running() and validate the reading.
 */
esp_err_t ds1307_create(i2c_master_bus_handle_t bus_handle, uint8_t address,
                        ds1307_handle_t *handle);
esp_err_t ds1307_delete(ds1307_handle_t handle);

/*
 * Oscillator state from the CH bit: *running == false means the clock is
 * halted and its registers are meaningless (fresh chip, or someone stopped
 * it). ds1307_set_time() clears CH, which is the only way to start it.
 */
esp_err_t ds1307_is_running(ds1307_handle_t handle, bool *running);

/*
 * Read the clock into a broken-down local-time struct (tm_year offset from
 * 1900, tm_mon 0-based, tm_isdst = -1, as mktime() expects).
 *
 * Returns ESP_ERR_INVALID_STATE when CH is set (oscillator halted), and
 * ESP_ERR_INVALID_RESPONSE when a BCD field is out of range, which is what a
 * corrupted or unpowered chip typically reads back as.
 */
esp_err_t ds1307_get_time(ds1307_handle_t handle, struct tm *out);

/*
 * Write the clock and clear CH so the oscillator runs. Fields are taken as
 * local time in the same convention as ds1307_get_time(); tm_wday is derived
 * internally, so the caller may leave it unset.
 *
 * Returns ESP_ERR_INVALID_ARG when the year falls outside DS1307_YEAR_MIN..MAX.
 */
esp_err_t ds1307_set_time(ds1307_handle_t handle, const struct tm *in);

#ifdef __cplusplus
}
#endif

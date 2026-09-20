/*
 * time_source.h — single source of wall-clock time for the whole firmware.
 *
 * Every consumer that needs a date/time (SD energy CSV, MQTT heartbeat, Modbus
 * time registers, future event logs) goes through this module instead of
 * calling time()/localtime() directly. That indirection is the whole point:
 * the backend that actually sets the clock can change without touching a
 * single consumer.
 *
 * Current state — NO backend is wired yet. Nothing calls settimeofday(), so
 * the system clock sits at the epoch and every stamp legitimately reads
 * "1970-01-01 00:00:0X" with quality 'U' (uptime-only). This is intentional,
 * not a stub to be deleted later: the quality flag tells a log reader exactly
 * how much to trust the timestamp column.
 *
 * Adding the DS1307 later is four steps and touches no consumer:
 *   1. create components/ds1307/ (I2C 0x68 on the existing shared bus —
 *      i2c_bus_get_handle(), same bus as PCF8574/PCF8575/LCD),
 *   2. in time_source_init(), under CONFIG_APP_TIME_RTC_ENABLE, read the chip
 *      and hand the epoch to time_source_set(epoch, TIME_SOURCE_RTC),
 *   3. set APP_TIME_RTC_ENABLE=y,
 *   4. add the LCD "Settings > Time > Set Clock" screen that writes the chip
 *      and then calls time_source_set().
 * The CSV schema, MQTT fields and register map stay byte-for-byte identical.
 *
 * Concurrency: the setters/service run from the energy-meter task, the getters
 * from HMI/MQTT/Modbus tasks. Scalar state is guarded internally.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Where the current wall-clock reading came from. */
typedef enum {
    TIME_SOURCE_NONE = 0,   /* no time base at all — clock reads 1970 */
    TIME_SOURCE_RTC,        /* DS1307 battery-backed RTC (hardware pending) */
    TIME_SOURCE_NVS,        /* restored from the persisted floor, free-running */
    TIME_SOURCE_NTP,        /* network sync (not enabled yet) */
} time_source_kind_t;

/* How much a timestamp produced right now can be trusted. Published verbatim
 * as the `tq` column of the SD energy CSV and the `tq` MQTT field. */
typedef enum {
    TIME_QUALITY_NONE = 0,  /* 'U' — uptime only, the date part is meaningless */
    TIME_QUALITY_ESTIMATE,  /* 'E' — restored floor, drifting, date is a lower bound */
    TIME_QUALITY_SYNCED,    /* 'S' — RTC/NTP backed */
} time_quality_t;

/* Longest string time_source_format_stamp() can produce, including the NUL. */
#define TIME_SOURCE_STAMP_LEN 20   /* "YYYY-MM-DD HH:MM:SS" */

/* Any epoch below this is treated as "clock was never set" (2020-01-01 UTC).
 * Guards against a corrupt NVS floor being restored as a plausible date. */
#define TIME_SOURCE_EPOCH_MIN ((time_t)1577836800)

/* Install the timezone, bump the boot counter, and restore the persisted
 * epoch floor if one exists. Safe to call once, early in boot (needs NVS).
 * With no backend compiled in this leaves the clock at the epoch. */
esp_err_t time_source_init(void);

/* Hand a known-good epoch to the module: sets the system clock, records the
 * source, and persists the new floor. This is the seam the DS1307 driver (and
 * later SNTP) plugs into — the ONLY way the clock is ever set. */
esp_err_t time_source_set(time_t epoch, time_source_kind_t kind);

/* Periodic upkeep: re-persists the epoch floor every
 * CONFIG_APP_TIME_NVS_SAVE_PERIOD_S so a reboot resumes near the right date.
 * Cheap and does nothing while the clock is unset. Called from the
 * energy-meter poll tick — deliberately no task of its own. */
void time_source_service(void);

/* Force the epoch floor to NVS now. Call before any controlled reboot so the
 * restored floor is as fresh as possible. No-op while the clock is unset. */
esp_err_t time_source_flush(void);

time_source_kind_t time_source_kind(void);
time_quality_t time_source_quality(void);

/* 'S' / 'E' / 'U' — the single character written to logs and telemetry. */
char time_source_quality_char(void);

/* True once any backend has set the clock (quality != NONE). */
bool time_source_is_valid(void);

/* Current epoch seconds. Returns whatever the system clock holds, which is a
 * small number near zero while the clock is unset — pair it with the quality
 * flag rather than testing it for plausibility. */
time_t time_source_now(void);

/* Seconds since boot. Always meaningful, never depends on a time backend;
 * this is the trustworthy time axis until the RTC exists. */
uint32_t time_source_uptime_s(void);

/* How many times the firmware has booted. Lets a log reader separate sessions
 * while every session still stamps the same 1970 date. */
uint32_t time_source_boot_count(void);

/* Write "YYYY-MM-DD HH:MM:SS" (local time) into buf. Always produces a valid
 * string — the epoch renders as 1970-01-01 — so callers never need a fallback.
 * Returns the number of characters written, excluding the NUL. */
size_t time_source_format_stamp(char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

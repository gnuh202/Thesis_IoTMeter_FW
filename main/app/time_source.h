/*
 * time_source.h — single source of wall-clock time for the whole firmware.
 *
 * Every consumer that needs a date/time (SD energy CSV, MQTT heartbeat, Modbus
 * time registers, future event logs) goes through this module instead of
 * calling time()/localtime() directly. That indirection is the whole point:
 * the backend that actually sets the clock can change without touching a
 * single consumer.
 *
 * Backends, in the order the module trusts them:
 *
 *   NTP   — a network sync. Earns quality 'S' and is written back to the RTC.
 *   RTC   — a DS1307 read at boot. Earns 'S' too, but only after the reading
 *           survives the sanity checks below.
 *   floor — the epoch persisted in NVS. NOT a time source: it is a monotonic
 *           lower bound, used to notice an RTC that has gone backwards and as
 *           the last resort when no RTC answers. Always flagged 'E'.
 *
 * The floor exists because the DS1307's CH bit is not an oscillator-stopped
 * flag: a dead backup battery leaves CH clear and the chip reports a stale but
 * perfectly plausible date, which no range check can catch. Comparing against
 * a value the firmware itself wrote is the only detector available.
 *
 * With every backend disabled or absent the clock stays at the epoch and every
 * stamp legitimately reads "1970-01-01 00:00:0X" with quality 'U' (uptime
 * only) — the flag tells a log reader exactly how much to trust the column.
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
    TIME_SOURCE_RTC,        /* DS1307 battery-backed RTC */
    TIME_SOURCE_NVS,        /* restored from the persisted floor, free-running */
    TIME_SOURCE_NTP,        /* network sync */
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

/* Install the timezone, bump the boot counter, read the persisted floor, and
 * bring up the DS1307 if one is configured. Safe to call once, early in boot
 * (needs NVS and the shared I2C bus, which it initializes if necessary).
 *
 * Returns ESP_OK even when no clock could be established: a missing RTC is a
 * degraded mode, not a boot failure. Check time_source_quality() for what the
 * clock is actually worth. */
esp_err_t time_source_init(void);

/* Hand a known-good epoch to the module: sets the system clock, records the
 * source, and persists the new floor. This is the seam the DS1307 driver and
 * SNTP plug into — the ONLY way the clock is ever set.
 *
 * Rejects an epoch below TIME_SOURCE_EPOCH_MIN (ESP_ERR_INVALID_ARG) and a
 * source that would lower the current quality within this boot session
 * (ESP_ERR_INVALID_STATE), so the floor can never clobber a live backend. */
esp_err_t time_source_set(time_t epoch, time_source_kind_t kind);

/* Periodic upkeep: re-persists the epoch floor, and drives the network sync
 * state machine when CONFIG_APP_TIME_SYNC_ENABLE is on. Cheap enough to call
 * every second; runs from the energy-meter poll tick — deliberately no task of
 * its own.
 *
 * The floor is written every CONFIG_APP_TIME_NVS_SAVE_PERIOD_S while the clock
 * is only an estimate, and once an hour once a real backend is driving it. */
void time_source_service(void);

/* Force the epoch floor to NVS now. Call before any controlled reboot so the
 * restored floor is as fresh as possible. No-op while the clock is unset. */
esp_err_t time_source_flush(void);

/* True when a DS1307 answered at boot and is usable. */
bool time_source_rtc_present(void);

/* Read/write the DS1307 directly, bypassing the system clock. For the console
 * "rtc" command and the LCD clock screen; ordinary consumers want
 * time_source_now(). Both return ESP_ERR_NOT_SUPPORTED when no RTC is
 * compiled in and ESP_ERR_INVALID_STATE when none answered at boot.
 *
 * time_source_rtc_write() also adopts the value as the system clock, so a
 * manual set takes effect immediately. */
esp_err_t time_source_rtc_read(time_t *epoch);
esp_err_t time_source_rtc_write(time_t epoch);

/* Ask for a network sync at the next service tick, ignoring the resync period.
 * No-op when CONFIG_APP_TIME_SYNC_ENABLE is off. */
void time_source_request_sync(void);

/* Epoch of the last successful network sync, 0 if never (survives reboot). */
time_t time_source_last_sync(void);

/* How many times the clock has been stepped by more than a minute this
 * session. A consumer holding something derived from the wall clock — an open
 * log file named after today's date, a window boundary — compares this against
 * the value it saw last and re-derives when it changes. */
uint32_t time_source_jump_count(void);

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

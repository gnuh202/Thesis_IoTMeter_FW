#pragma once

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SD card manager (SDSPI on the shared SPI bus).
 *
 * A background monitor task watches the card-detect pin. When a card is
 * inserted it initializes SDSPI and mounts a FAT filesystem; when the card is
 * removed it unmounts and frees the host so re-insertion re-mounts cleanly.
 *
 * Logging helpers are safe to call at any time: if no card is mounted they
 * return ESP_ERR_INVALID_STATE instead of blocking, so callers can log
 * opportunistically without tracking card presence.
 */

/* Start card-detect init + the mount/unmount monitor task. Safe to call once. */
esp_err_t sd_card_manager_start(void);

/* True if the card-detect pin reports a card physically present. */
bool sd_card_is_inserted(void);

/* True if a FAT filesystem is currently mounted and ready for logging. */
bool sd_card_is_mounted(void);

/*
 * Append one row to the grid-fault log, /sdcard/EVENTS/FAULTS.CSV.
 *
 * This file records POWER-GRID faults only — over/under voltage, over-current,
 * phase loss, frequency out of band, IC fatal error — never board or firmware
 * events (boot, card mount, network). An operator reading it should see a list
 * of grid incidents with their times, nothing else.
 *
 *   header - column header row (no newline), written automatically whenever
 *            the file is new or empty. May be NULL to skip it.
 *   line   - the data row (no newline).
 *
 * Returns ESP_ERR_INVALID_STATE if no card is mounted, so callers can log
 * opportunistically without tracking card presence.
 */
esp_err_t sd_card_log_fault_csv(const char *header, const char *line);

/* Append a pre-formatted line to the energy log. Adds a newline. Returns
 * ESP_ERR_INVALID_STATE if no card is mounted. */
esp_err_t sd_card_log_energy(const char *line);

/*
 * Append a CSV row to the energy log, with schema header and size rotation.
 *
 *   header  - column header row (no newline). Written automatically whenever
 *             the target file is new or empty, so a fresh card, a rotated file
 *             and a card formatted by the user all end up with a readable CSV.
 *             May be NULL to skip the header.
 *   line    - the data row (no newline).
 *   max_kb  - rotate when the file reaches this many KiB; 0 disables rotation.
 *             ENERGY.CSV -> ENERGY.001 -> ENERGY.002 -> ENERGY.003 -> deleted,
 *             so the card keeps the newest four generations and never fills.
 *
 * Rotation is size-based rather than date-based even now that a DS1307 is
 * fitted: a dead backup battery or an unsynced clock would make a date-named
 * file land in the wrong place or collide with an existing one, and a size cap
 * is the only bound that also guarantees the card cannot fill.
 */
esp_err_t sd_card_log_energy_csv(const char *header, const char *line, uint32_t max_kb);

/* Calibration backup helpers */
typedef struct {
    char filename[32];  /* basename only, e.g. "calib_3W_01.bin" */
    uint8_t mode;       /* 0=3P4W, 1=3P3W, parsed from filename */
    uint8_t num;        /* sequence number parsed from filename */
} sd_calib_entry_t;

/* Ensure /sdcard/calib directory exists (called during mount). */
esp_err_t sd_card_ensure_calib_dir(void);

/* Export current calibration (calls calib_backup_pack_single).
 * Creates calib_3W_NN.bin or calib_4W_NN.bin based on header.wiring_mode
 * metadata tag (filename = display / sort key, not slot semantics).
 * Returns full path via path_out. ESP_ERR_INVALID_STATE if not mounted. */
esp_err_t sd_card_calib_export_current(char *path_out, size_t path_cap);

/* List calibration files in /sdcard/calib, sorted by (mode, number) descending (newest first).
 * Returns actual count via *count. Returns ESP_ERR_INVALID_STATE if not mounted. */
esp_err_t sd_card_calib_list(sd_calib_entry_t *out, size_t max, size_t *count);

/* Read and unpack a calibration backup file. Calls calib_backup_unpack internally.
 * Applies file gains to the active wiring_mode (wiring_mode in header is metadata
 * only — phase gains apply to the common single profile regardless). Saves NVS.
 * Returns ESP_ERR_INVALID_STATE if not mounted. */
esp_err_t sd_card_calib_import(const char *filename);

/* Kept for backward compatibility with earlier detect-only callers. */
esp_err_t sd_card_detect_init(void);

#ifdef __cplusplus
}
#endif

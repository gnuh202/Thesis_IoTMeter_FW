#pragma once

#include <stdarg.h>
#include <stdbool.h>
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

/* Append a line to the events log (failures/events). Adds a newline. Returns
 * ESP_ERR_INVALID_STATE if no card is mounted. */
esp_err_t sd_card_log_event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Append a pre-formatted line to the energy log. Adds a newline. Returns
 * ESP_ERR_INVALID_STATE if no card is mounted. */
esp_err_t sd_card_log_energy(const char *line);

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

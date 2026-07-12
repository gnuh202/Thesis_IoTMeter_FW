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

/* Kept for backward compatibility with earlier detect-only callers. */
esp_err_t sd_card_detect_init(void);

#ifdef __cplusplus
}
#endif

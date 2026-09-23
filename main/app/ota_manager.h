#pragma once

/*
 * OTA manager — check a GitHub release manifest, download the image into the
 * spare app slot, and commit or roll back the result.
 *
 * Three rules shape this module:
 *
 *  - The running image's app descriptor is the ONLY authority on "what version
 *    am I". Nothing is mirrored into NVS: a mirror can disagree with the image
 *    that is actually executing, and a version that can lie is worse than no
 *    version at all. Tag a release `v1.2.0` and IDF's `git describe` puts
 *    exactly that string in the descriptor, where MQTT, the LCD and Modbus all
 *    read it from.
 *
 *  - The manifest says a new release EXISTS. It never decides what gets
 *    installed: before committing, the descriptor embedded in the downloaded
 *    image is compared against the running one. The manifest lives on the
 *    network; the descriptor travels inside the thing being installed.
 *
 *  - A new image must prove it works. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
 *    the bootloader marks it PENDING_VERIFY and reverts on the next reset unless
 *    this module commits it (see the self-test in ota_manager_init).
 *
 * All the long operations run on a short-lived worker task, so the caller — the
 * LCD menu, the console, the web handler, the MQTT callback — never blocks on
 * the network. Callers poll ota_manager_get_state().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_VERSION_MAX 32
#define OTA_URL_MAX     224
#define OTA_NOTES_MAX   48
#define OTA_ERR_MAX     40

typedef enum {
    OTA_STATE_IDLE = 0,        /* nothing has been asked for yet */
    OTA_STATE_CHECKING,        /* fetching the manifest */
    OTA_STATE_CHECK_DONE,      /* manifest read; see ota_manager_get_release() */
    OTA_STATE_DOWNLOADING,     /* writing the image; percent is meaningful */
    OTA_STATE_REBOOT_PENDING,  /* image written and verified; reboot to run it */
    OTA_STATE_FAILED,          /* see the err string from ota_manager_get_state() */
} ota_state_t;

typedef struct {
    bool available;                 /* latest is strictly newer than running */
    char running[OTA_VERSION_MAX];  /* from the app descriptor */
    char latest[OTA_VERSION_MAX];   /* from the manifest */
    char url[OTA_URL_MAX];          /* image asset to download */
    char notes[OTA_NOTES_MAX];      /* one-line release note, may be empty */
} ota_release_t;

/* Wire up state and handle a PENDING_VERIFY boot. Safe to call when OTA is
 * disabled in Kconfig (the whole module compiles out in that case). */
esp_err_t ota_manager_init(void);

/* The running image's version string, never NULL. */
const char *ota_manager_running_version(void);

/* Kick off a manifest check. Returns ESP_ERR_INVALID_STATE if the worker is
 * already busy. The result lands in ota_manager_get_release(). */
esp_err_t ota_manager_request_check(void);

/* Kick off a download+install. url may be NULL to use the URL from the last
 * successful check. */
esp_err_t ota_manager_request_update(const char *url);

/* Current state. percent and err may be NULL. percent is 0..100 and only
 * meaningful while DOWNLOADING; err is set only in the FAILED state. */
ota_state_t ota_manager_get_state(int *percent, char *err, size_t err_len);

/* Copy out the last check result. Returns false if no check has completed. */
bool ota_manager_get_release(ota_release_t *out);

/* True while the worker task is running. */
bool ota_manager_busy(void);

/* True when the running image is on probation and has not committed yet. */
bool ota_manager_pending_verify(void);

/* Commit the running image immediately, bypassing the self-test window. */
esp_err_t ota_manager_mark_valid(void);

/* Boot the other slot on the next reset. Fails when there is no valid image
 * there — a device flashed only once has nothing to go back to. */
esp_err_t ota_manager_rollback(void);

/*
 * Compare two "1.2.3" / "v1.2.3" strings. Returns <0, 0 or >0 like strcmp.
 *
 * Only major.minor.patch is compared. A build made between tags describes as
 * `v1.2.0-5-gabc1234`; the suffix is ignored, so such a build is treated as
 * equal to v1.2.0 and will not offer itself an update it already contains.
 * A string that does not parse sorts below everything, so a device running an
 * untagged hash build always sees the newest release as an upgrade.
 */
int ota_manager_version_compare(const char *a, const char *b);

/*
 * The same version packed into one 16-bit word as major<<8 | minor, for the
 * Modbus input register that has room for nothing else. Each field saturates
 * at 255. Returns 0 when the string does not parse — an untagged development
 * build reports "unknown" rather than a number it made up.
 */
uint16_t ota_manager_version_word(const char *s);

#ifdef __cplusplus
}
#endif

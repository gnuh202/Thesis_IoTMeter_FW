#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Certificate store on internal flash (Feature 13B).
 *
 * Mounts the "storage" FAT partition (see partitions.csv) at /flash and owns the
 * PEM files the MQTT TLS runtime loads by path. No SD card is involved and no PEM
 * content is ever written to NVS: NVS only ever holds the *paths*, inside the
 * Configuration Manager snapshot.
 *
 * Files are per MQTT profile, because the three profiles are independent brokers
 * that generally do not share a CA — pointing them all at one ca.pem would mean
 * switching profiles silently re-used the wrong CA. Each profile owns a
 * ca/cert/key triple named by its index:
 *
 *   profile 0 -> /flash/ca0.pem  /flash/cert0.pem  /flash/key0.pem
 *   profile 1 -> /flash/ca1.pem  /flash/cert1.pem  /flash/key1.pem
 *   profile 2 -> /flash/ca2.pem  /flash/cert2.pem  /flash/key2.pem
 *
 * The names stay inside 8.3 because this project sets CONFIG_FATFS_LFN_NONE.
 *
 * SECURITY: the client private key is stored as plaintext in flash and can be
 * recovered with `esptool read_flash`. Accepted for the current stage; a
 * production build must enable flash encryption. Nothing here reads a stored PEM
 * back out to a caller — only presence, size and a short SHA-256 fingerprint are
 * exposed, so no protocol can be used to exfiltrate a key.
 */

#define CERT_STORE_MOUNT_POINT "/flash"

/* Same upper bound the MQTT manager applies when loading a PEM, so a file that
 * this module accepts can always be loaded back. */
#define CERT_STORE_PEM_MAX 8192

/* Must match CONFIG_MANAGER_MQTT_PROFILE_COUNT; cert_store.c asserts that. */
#define CERT_STORE_PROFILE_COUNT 3

/* Longest path this module produces, e.g. "/flash/cert0.pem". */
#define CERT_STORE_PATH_MAX 24

typedef enum {
    CERT_SLOT_CA = 0,   /* ca<N>.pem   — broker CA (CA_ONLY and MUTUAL) */
    CERT_SLOT_CERT,     /* cert<N>.pem — client certificate (MUTUAL) */
    CERT_SLOT_KEY,      /* key<N>.pem  — client private key (MUTUAL) */
    CERT_SLOT_COUNT,
} cert_slot_t;

typedef struct {
    bool present;
    size_t size;
    /* First 8 bytes of the file's SHA-256, hex. Enough to tell "the file I
     * uploaded" from "some other file" without disclosing any content. */
    char fingerprint[17];
} cert_slot_info_t;

/* Mount the FAT partition, formatting it on first boot. Idempotent. Must run
 * before the MQTT manager starts, otherwise TLS paths under /flash cannot be
 * opened. */
esp_err_t cert_store_init(void);

/* True once the partition is mounted. */
bool cert_store_ready(void);

/* "ca" | "cert" | "key" */
const char *cert_store_slot_name(cert_slot_t slot);

/* Absolute path of a profile's slot, e.g. "/flash/ca0.pem", written into out.
 * Valid for every profile/slot pair even when no file has been uploaded yet —
 * this is what goes into the profile's ca_path / cert_path / key_path.
 * Returns out for convenient use as a printf argument; on a bad profile or slot
 * out becomes an empty string. */
const char *cert_store_slot_path(int profile, cert_slot_t slot, char *out, size_t out_len);

/* Map "ca"/"cert"/"key" to a slot. Returns false for anything else. */
bool cert_store_slot_from_name(const char *name, cert_slot_t *out);

/* Replace the slot's file with pem[0..len). Rejects an empty body, a body over
 * CERT_STORE_PEM_MAX, and anything without a PEM BEGIN/END envelope. Writes to a
 * temporary file and renames, so a failed upload never leaves a truncated PEM
 * where the MQTT runtime would try to use it. */
esp_err_t cert_store_write(int profile, cert_slot_t slot, const char *pem, size_t len);

/* Remove the slot's file. Returns ESP_OK when it was already absent. */
esp_err_t cert_store_delete(int profile, cert_slot_t slot);

/* Presence, size and short fingerprint. Never returns file content. */
esp_err_t cert_store_stat(int profile, cert_slot_t slot, cert_slot_info_t *out);

#ifdef __cplusplus
}
#endif

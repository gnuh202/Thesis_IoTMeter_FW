#include "cert_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config_manager.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "mbedtls/sha256.h"

/*
 * Certificate store backed by the "storage" FAT partition on internal flash.
 *
 * The partition is 1 MB (label "storage" in partitions.csv). Wear levelling is
 * enabled (esp_vfs_fat_spiflash_mount_rw_wl) because certificates are rewritten
 * in place whenever the operator uploads a new one.
 *
 * Files are named "<slot><profile>.pem" so each MQTT profile has its own triple:
 * ca0/cert0/key0 for profile 0, ca1/... for profile 1, and so on. Three profiles
 * are three independent brokers, so a shared CA file would mean switching the
 * active profile silently verified against the previous broker's CA.
 *
 * CONFIG_FATFS_LFN_NONE is set in this project, so filenames must stay inside
 * 8.3. The longest base name is "cert0" (5 chars), well inside the limit.
 */

/* The profile index in a filename comes straight from the config manager's
 * profile array, so the two counts have to agree. */
_Static_assert(CERT_STORE_PROFILE_COUNT == CONFIG_MANAGER_MQTT_PROFILE_COUNT,
               "cert store profile count must match the MQTT profile count");

/* Paths written here go into config_mqtt_profile_t.ca_path and friends, so they
 * must fit that field too. */
_Static_assert(CERT_STORE_PATH_MAX <= CONFIG_MANAGER_MQTT_PATH_LEN,
               "cert store paths must fit the profile's path fields");

static const char *TAG = "cert_store";

#define CERT_STORE_PARTITION_LABEL "storage"

/* One temporary name reused by every slot: writes are serialised by the single
 * httpd task that calls in, and a leftover temp file is overwritten anyway. */
#define CERT_STORE_TMP_PATH CERT_STORE_MOUNT_POINT "/upload.tmp"

static wl_handle_t s_wl_handle = WL_INVALID_HANDLE;
static bool s_mounted;

static const char *const s_slot_names[CERT_SLOT_COUNT] = {
    "ca", "cert", "key",
};

static bool slot_valid(int profile, cert_slot_t slot)
{
    return profile >= 0 && profile < CERT_STORE_PROFILE_COUNT &&
           slot >= 0 && slot < CERT_SLOT_COUNT;
}

/* Build "/flash/<slot><profile>.pem". Every path in this module goes through
 * here, so the naming scheme lives in exactly one place. */
static const char *slot_path(int profile, cert_slot_t slot, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return "";
    }
    if (!slot_valid(profile, slot)) {
        out[0] = '\0';
        return out;
    }
    snprintf(out, out_len, CERT_STORE_MOUNT_POINT "/%s%d.pem", s_slot_names[slot], profile);
    return out;
}

esp_err_t cert_store_init(void)
{
    if (s_mounted) {
        return ESP_OK;
    }

    const esp_vfs_fat_mount_config_t mount_cfg = {
        /* First boot ships an unformatted partition; formatting it here is the
         * only way the operator can upload anything at all. */
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };

    esp_err_t ret = esp_vfs_fat_spiflash_mount_rw_wl(CERT_STORE_MOUNT_POINT,
                                                     CERT_STORE_PARTITION_LABEL,
                                                     &mount_cfg, &s_wl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount \"%s\" at %s failed: %s", CERT_STORE_PARTITION_LABEL,
                 CERT_STORE_MOUNT_POINT, esp_err_to_name(ret));
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "certificate store mounted at %s (partition \"%s\")",
             CERT_STORE_MOUNT_POINT, CERT_STORE_PARTITION_LABEL);

    /* Log presence only — never content. Helps diagnose a TLS profile that
     * refuses to connect because its file was never uploaded. Absent slots are
     * skipped: with 3 profiles x 3 slots, listing every empty one would bury the
     * boot log for what is normally a one-CA setup. */
    for (int prof = 0; prof < CERT_STORE_PROFILE_COUNT; prof++) {
        for (int i = 0; i < CERT_SLOT_COUNT; i++) {
            cert_slot_info_t info;
            char path[CERT_STORE_PATH_MAX];
            if (cert_store_stat(prof, (cert_slot_t)i, &info) == ESP_OK && info.present) {
                ESP_LOGI(TAG, "  profile %d %-4s %s (%u bytes)", prof, s_slot_names[i],
                         slot_path(prof, (cert_slot_t)i, path, sizeof(path)),
                         (unsigned)info.size);
            }
        }
    }
    return ESP_OK;
}

bool cert_store_ready(void)
{
    return s_mounted;
}

const char *cert_store_slot_name(cert_slot_t slot)
{
    if (slot < 0 || slot >= CERT_SLOT_COUNT) {
        return "?";
    }
    return s_slot_names[slot];
}

const char *cert_store_slot_path(int profile, cert_slot_t slot, char *out, size_t out_len)
{
    return slot_path(profile, slot, out, out_len);
}

bool cert_store_slot_from_name(const char *name, cert_slot_t *out)
{
    if (name == NULL || out == NULL) {
        return false;
    }
    for (int i = 0; i < CERT_SLOT_COUNT; i++) {
        if (strcmp(name, s_slot_names[i]) == 0) {
            *out = (cert_slot_t)i;
            return true;
        }
    }
    return false;
}

/* Trim leading/trailing whitespace of the uploaded body in place (by moving the
 * start pointer and shrinking the length). Browsers and editors happily add a
 * trailing newline or a UTF-8 BOM; neither belongs in a PEM file. */
static void trim_body(const char **pem, size_t *len)
{
    const unsigned char *p = (const unsigned char *)*pem;
    size_t n = *len;

    /* UTF-8 BOM: mbedtls' PEM parser does not skip it. */
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        p += 3;
        n -= 3;
    }
    while (n > 0 && (p[0] == '\r' || p[0] == '\n' || p[0] == ' ' || p[0] == '\t')) {
        p++;
        n--;
    }
    while (n > 0) {
        unsigned char c = p[n - 1];
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t' && c != '\0') {
            break;
        }
        n--;
    }

    *pem = (const char *)p;
    *len = n;
}

/* Substring search over a body that is not NUL-terminated (raw HTTP payload),
 * so strstr() cannot be used here. */
static bool contains(const char *hay, size_t hay_len, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len == 0 || hay_len < needle_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

/* Reject a body that is not PEM before it reaches the filesystem: a truncated
 * paste or an accidentally uploaded DER/text file would otherwise only surface
 * much later, as an mbedtls parse error during the handshake. */
static bool looks_like_pem(const char *pem, size_t len)
{
    return contains(pem, len, "-----BEGIN ") && contains(pem, len, "-----END ");
}

esp_err_t cert_store_write(int profile, cert_slot_t slot, const char *pem, size_t len)
{
    if (!slot_valid(profile, slot) || pem == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        ESP_LOGE(TAG, "cannot store %s: %s is not mounted",
                 s_slot_names[slot], CERT_STORE_MOUNT_POINT);
        return ESP_ERR_INVALID_STATE;
    }

    char path[CERT_STORE_PATH_MAX];
    slot_path(profile, slot, path, sizeof(path));
    /* A file picked in a browser usually carries a trailing newline, and files
     * saved by a Windows editor can carry a BOM. Drop both before the size and
     * envelope checks so they are judged on the PEM itself. */
    trim_body(&pem, &len);

    if (len == 0) {
        ESP_LOGE(TAG, "refusing to store an empty PEM at %s", path);
        return ESP_ERR_INVALID_SIZE;
    }
    if (len > CERT_STORE_PEM_MAX) {
        ESP_LOGE(TAG, "refusing %s PEM of %u bytes, over the %d byte limit",
                 path, (unsigned)len, CERT_STORE_PEM_MAX);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!looks_like_pem(pem, len)) {
        ESP_LOGE(TAG, "refusing %s upload: no PEM BEGIN/END envelope found", path);
        return ESP_ERR_INVALID_ARG;
    }

    /* Write to a temp file and rename, so an interrupted upload cannot leave a
     * half-written PEM in the path the MQTT runtime loads. */
    FILE *f = fopen(CERT_STORE_TMP_PATH, "wb");
    if (f == NULL) {
        ESP_LOGE(TAG, "open %s for write failed", CERT_STORE_TMP_PATH);
        return ESP_FAIL;
    }

    size_t written = fwrite(pem, 1, len, f);
    /* fclose flushes; a full partition typically fails there, not in fwrite. */
    int close_ret = fclose(f);
    if (written != len || close_ret != 0) {
        ESP_LOGE(TAG, "write %s failed (%u/%u bytes, close=%d)", path,
                 (unsigned)written, (unsigned)len, close_ret);
        unlink(CERT_STORE_TMP_PATH);
        return ESP_FAIL;
    }

    /* FAT rename does not replace an existing target. */
    unlink(path);
    if (rename(CERT_STORE_TMP_PATH, path) != 0) {
        ESP_LOGE(TAG, "rename %s -> %s failed", CERT_STORE_TMP_PATH, path);
        unlink(CERT_STORE_TMP_PATH);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "stored profile %d %s at %s (%u bytes)", profile,
             s_slot_names[slot], path, (unsigned)len);
    return ESP_OK;
}

esp_err_t cert_store_delete(int profile, cert_slot_t slot)
{
    if (!slot_valid(profile, slot)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[CERT_STORE_PATH_MAX];
    slot_path(profile, slot, path, sizeof(path));

    if (unlink(path) != 0) {
        struct stat st;
        if (stat(path, &st) != 0) {
            /* Already gone: the caller's intent is satisfied. */
            return ESP_OK;
        }
        ESP_LOGE(TAG, "delete %s failed", path);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "deleted %s", path);
    return ESP_OK;
}

/* Hash the file in chunks so an 8 KB PEM never needs a second full-size heap
 * buffer, and so no complete PEM is ever held for the sake of reporting. */
static esp_err_t file_fingerprint(const char *path, char *out, size_t out_len)
{
    out[0] = '\0';

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_FAIL;
    }

    unsigned char *chunk = malloc(512);
    if (chunk == NULL) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    int rc = mbedtls_sha256_starts(&ctx, 0);

    size_t got;
    while (rc == 0 && (got = fread(chunk, 1, 512, f)) > 0) {
        rc = mbedtls_sha256_update(&ctx, chunk, got);
    }

    unsigned char digest[32];
    if (rc == 0) {
        rc = mbedtls_sha256_finish(&ctx, digest);
    }
    mbedtls_sha256_free(&ctx);
    free(chunk);
    fclose(f);

    if (rc != 0) {
        return ESP_FAIL;
    }

    /* 8 bytes = 16 hex chars, plus the terminator. */
    if (out_len < 17) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (int i = 0; i < 8; i++) {
        snprintf(out + i * 2, out_len - i * 2, "%02x", digest[i]);
    }
    return ESP_OK;
}

esp_err_t cert_store_stat(int profile, cert_slot_t slot, cert_slot_info_t *out)
{
    if (!slot_valid(profile, slot) || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    if (!s_mounted) {
        return ESP_ERR_INVALID_STATE;
    }

    char path[CERT_STORE_PATH_MAX];
    slot_path(profile, slot, path, sizeof(path));

    struct stat st;
    if (stat(path, &st) != 0) {
        return ESP_OK; /* absent, not an error */
    }

    out->present = true;
    out->size = (size_t)st.st_size;

    /* A fingerprint failure is not worth failing the whole query: presence and
     * size are still useful, so report them with an empty fingerprint. */
    if (file_fingerprint(path, out->fingerprint, sizeof(out->fingerprint)) != ESP_OK) {
        ESP_LOGW(TAG, "fingerprint %s failed", path);
    }
    return ESP_OK;
}

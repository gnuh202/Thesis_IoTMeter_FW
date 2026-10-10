/*
 * OTA manager. See ota_manager.h for the three rules this module is built on.
 */

#include "ota_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_app_desc.h"
#include "esp_log.h"

static const char *TAG = "ota";

/*
 * Version comparison is pure and dependency-free, so it stays outside the
 * feature gate: the Modbus slave uses it to derive its version register whether
 * or not OTA itself is built.
 */
static bool parse_semver(const char *s, unsigned *maj, unsigned *min, unsigned *pat)
{
    if (s == NULL) {
        return false;
    }
    while (*s == 'v' || *s == 'V' || *s == ' ') {
        s++;
    }

    char *end = NULL;
    unsigned long a = strtoul(s, &end, 10);
    if (end == s || *end != '.') {
        return false;
    }
    s = end + 1;
    unsigned long b = strtoul(s, &end, 10);
    if (end == s) {
        return false;
    }
    unsigned long c = 0;
    if (*end == '.') {
        s = end + 1;
        c = strtoul(s, &end, 10);
        if (end == s) {
            return false;
        }
    }

    *maj = (unsigned)a;
    *min = (unsigned)b;
    *pat = (unsigned)c;
    return true;
}

int ota_manager_version_compare(const char *a, const char *b)
{
    unsigned am, an, ap, bm, bn, bp;
    bool a_ok = parse_semver(a, &am, &an, &ap);
    bool b_ok = parse_semver(b, &bm, &bn, &bp);

    /* An unparseable string is "older than anything". A device running an
     * untagged hash build therefore always sees a tagged release as newer,
     * which is the behaviour a developer flashing a dev build then pointing it
     * at production would expect. */
    if (!a_ok && !b_ok) return 0;
    if (!a_ok) return -1;
    if (!b_ok) return 1;

    if (am != bm) return (am < bm) ? -1 : 1;
    if (an != bn) return (an < bn) ? -1 : 1;
    if (ap != bp) return (ap < bp) ? -1 : 1;
    return 0;
}

const char *ota_manager_running_version(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    return (desc != NULL && desc->version[0] != '\0') ? desc->version : "?";
}

uint16_t ota_manager_version_word(const char *s)
{
    unsigned maj, min, pat;
    if (!parse_semver(s, &maj, &min, &pat)) {
        return 0;
    }
    if (maj > 255) maj = 255;
    if (min > 255) min = 255;
    return (uint16_t)((maj << 8) | min);
}

#if CONFIG_APP_OTA_ENABLE

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "network_manager.h"
#include "system_status.h"

/* The manifest is a handful of short fields; anything larger is not ours. */
#define OTA_MANIFEST_MAX 1024

/* How long the "image written" screen stays up before the reboot that runs it.
 * Long enough for an operator standing at the LCD to read it, short enough
 * that an unattended MQTT-triggered update does not look hung. */
#define OTA_REBOOT_DELAY_MS 3000

/* Self-test poll period. The gate itself is CONFIG_APP_OTA_SELFTEST_DELAY_S;
 * this is only how often the gate is re-examined. */
#define OTA_SELFTEST_TICK_US (5 * 1000 * 1000)

typedef enum {
    OTA_REQ_CHECK = 0,
    OTA_REQ_UPDATE,
    OTA_REQ_UPDATE_VERSION,
} ota_req_t;

static SemaphoreHandle_t s_lock;
static ota_state_t   s_state = OTA_STATE_IDLE;
static int           s_percent;
static char          s_err[OTA_ERR_MAX];
static bool          s_busy;
static bool          s_have_release;
static ota_release_t s_release;
static ota_req_t     s_req;
static char          s_req_url[OTA_URL_MAX];
static char          s_req_version[OTA_VERSION_MAX];

static bool                s_pending_verify;
static esp_timer_handle_t  s_selftest_timer;

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void set_failed(const char *reason)
{
    lock();
    s_state = OTA_STATE_FAILED;
    strlcpy(s_err, reason, sizeof(s_err));
    unlock();
    ESP_LOGE(TAG, "%s", reason);
}

/* ---------------- manifest fetch ---------------- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
} http_sink_t;

static esp_err_t http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->user_data == NULL) {
        return ESP_OK;
    }
    /* A 30x from GitHub carries a short HTML body before the redirect is
     * followed, and an error page carries a long one. Only the body of the
     * final 200 is the manifest. */
    if (esp_http_client_get_status_code(evt->client) != 200) {
        return ESP_OK;
    }

    http_sink_t *sink = (http_sink_t *)evt->user_data;
    size_t room = (sink->len + 1 < sink->cap) ? (sink->cap - 1 - sink->len) : 0;
    size_t n = ((size_t)evt->data_len < room) ? (size_t)evt->data_len : room;
    if (n > 0) {
        memcpy(sink->buf + sink->len, evt->data, n);
        sink->len += n;
        sink->buf[sink->len] = '\0';
    }
    return ESP_OK;
}

static esp_err_t fetch_text(const char *url, char *buf, size_t cap)
{
    http_sink_t sink = { .buf = buf, .cap = cap, .len = 0 };
    buf[0] = '\0';

    /*
     * GitHub asset URLs answer with a 302 whose Location is a signed CDN URL of
     * roughly 1.4 KB. Following it means *sending* "GET <1.4 KB path> HTTP/1.1",
     * and that request line is built in the transmit buffer -- 512 bytes by
     * default, which is what "HTTP_CLIENT: Out of buffer" reports. buffer_size
     * alone does not cover it: esp_http_client maps it to buffer_size_rx only,
     * so the redirect kept failing until buffer_size_tx was raised as well.
     */
    esp_http_client_config_t cfg = {
        .url               = url,
        .timeout_ms        = CONFIG_APP_OTA_HTTP_TIMEOUT_MS,
        .buffer_size       = 4096,
        .buffer_size_tx    = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler     = http_event,
        .user_data         = &sink,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* GitHub answers plain requests without one, but a named agent keeps the
     * device identifiable in their logs and out of generic-bot throttling. */
    esp_http_client_set_header(client, "User-Agent", "esp32-iotmeter-ota");

    esp_err_t ret = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "fetch %s failed: %s", url, esp_err_to_name(ret));
        return ret;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP %d from %s", status, url);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (sink.len == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t parse_manifest(const char *body, ota_release_t *out)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    esp_err_t ret = ESP_ERR_INVALID_RESPONSE;
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *url     = cJSON_GetObjectItemCaseSensitive(root, "url");
    const cJSON *notes   = cJSON_GetObjectItemCaseSensitive(root, "notes");

    if (cJSON_IsString(version) && version->valuestring != NULL &&
        cJSON_IsString(url) && url->valuestring != NULL) {
        strlcpy(out->latest, version->valuestring, sizeof(out->latest));
        strlcpy(out->url, url->valuestring, sizeof(out->url));
        if (cJSON_IsString(notes) && notes->valuestring != NULL) {
            strlcpy(out->notes, notes->valuestring, sizeof(out->notes));
        } else {
            out->notes[0] = '\0';
        }
        ret = ESP_OK;
    }

    cJSON_Delete(root);
    return ret;
}

static void do_check(void)
{
    network_status_t net = {0};
    if (network_manager_get_status(&net) != ESP_OK || !net.has_ip) {
        set_failed("No network");
        return;
    }

    char *body = malloc(OTA_MANIFEST_MAX);
    if (body == NULL) {
        set_failed("Out of memory");
        return;
    }

    esp_err_t ret = fetch_text(CONFIG_APP_OTA_MANIFEST_URL, body, OTA_MANIFEST_MAX);
    if (ret != ESP_OK) {
        free(body);
        set_failed(ret == ESP_ERR_INVALID_RESPONSE ? "Bad server reply" : "Download failed");
        return;
    }

    ota_release_t rel = {0};
    strlcpy(rel.running, ota_manager_running_version(), sizeof(rel.running));
    ret = parse_manifest(body, &rel);
    free(body);

    if (ret != ESP_OK) {
        set_failed("Bad manifest");
        return;
    }

    rel.available = ota_manager_version_compare(rel.latest, rel.running) > 0;
    ESP_LOGI(TAG, "running %s, latest %s -> %s",
             rel.running, rel.latest, rel.available ? "update available" : "up to date");

    lock();
    s_release = rel;
    s_have_release = true;
    s_state = OTA_STATE_CHECK_DONE;
    unlock();
}

/* ---------------- download + install ---------------- */

static void do_update(const char *url)
{
    network_status_t net = {0};
    if (network_manager_get_status(&net) != ESP_OK || !net.has_ip) {
        set_failed("No network");
        return;
    }

    /* Both entry paths land here once the image URL is known. The state moves
     * to DOWNLOADING only now: an install-by-version spends its first seconds
     * in CHECKING while it reads that release's manifest. */
    lock();
    s_state = OTA_STATE_DOWNLOADING;
    unlock();

    ESP_LOGI(TAG, "downloading %s", url);

    /* Same signed-CDN redirect as the manifest fetch: both buffers, because the
     * 1.4 KB request line is built in the transmit one. */
    esp_http_client_config_t http_cfg = {
        .url               = url,
        .timeout_ms        = CONFIG_APP_OTA_HTTP_TIMEOUT_MS,
        .buffer_size       = 4096,
        .buffer_size_tx    = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t ret = esp_https_ota_begin(&ota_cfg, &handle);
    if (ret != ESP_OK || handle == NULL) {
        set_failed("Connect failed");
        return;
    }

    /*
     * Decide from the image, not from the manifest. The manifest arrived over
     * the network and could name any version; the descriptor below is read out
     * of the bytes that are about to be written to flash.
     */
    esp_app_desc_t incoming = {0};
    ret = esp_https_ota_get_img_desc(handle, &incoming);
    if (ret != ESP_OK) {
        esp_https_ota_abort(handle);
        set_failed("Bad image header");
        return;
    }
    if (ota_manager_version_compare(incoming.version, ota_manager_running_version()) == 0) {
        esp_https_ota_abort(handle);
        set_failed("Same version");
        return;
    }
    ESP_LOGI(TAG, "incoming image %s built %s %s",
             incoming.version, incoming.date, incoming.time);

    int total = esp_https_ota_get_image_size(handle);
    int last_logged = -10;

    while ((ret = esp_https_ota_perform(handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        int done = esp_https_ota_get_image_len_read(handle);
        int pct = (total > 0) ? (int)((int64_t)done * 100 / total) : 0;
        lock();
        s_percent = pct;
        unlock();
        if (pct >= last_logged + 10) {
            ESP_LOGI(TAG, "%d%% (%d/%d bytes)", pct, done, total);
            last_logged = pct;
        }
    }

    if (ret != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        esp_https_ota_abort(handle);
        set_failed("Transfer failed");
        return;
    }

    /* finish() validates the image and flips the boot partition. It owns the
     * handle from here, so there is no abort on this path. */
    ret = esp_https_ota_finish(handle);
    if (ret != ESP_OK) {
        set_failed(ret == ESP_ERR_OTA_VALIDATE_FAILED ? "Image invalid" : "Install failed");
        return;
    }

    lock();
    s_percent = 100;
    s_state = OTA_STATE_REBOOT_PENDING;
    unlock();

    /*
     * Reboot here rather than leaving it to whoever asked. An update triggered
     * over MQTT has no operator to press a key, and an image that is written
     * but not running is the one state where the device disagrees with itself
     * about what version it is. One behaviour, every trigger.
     */
    ESP_LOGW(TAG, "update installed; rebooting into the new image");
    vTaskDelay(pdMS_TO_TICKS(OTA_REBOOT_DELAY_MS));
    esp_restart();
}

/* ---------------- install a named release ---------------- */

/*
 * Everything here derives from the one manifest Kconfig: the default points at
 * `<repo>/releases/latest/download/manifest.json`, and the same repo serves
 * every tagged release at `<repo>/releases/download/<tag>/manifest.json` —
 * release.yml publishes both assets for each v* tag, so the pattern holds for
 * every released version without a second URL to configure.
 */
static esp_err_t releases_base(char *out, size_t cap)
{
    const char *mark = strstr(CONFIG_APP_OTA_MANIFEST_URL, "/releases/");
    if (mark == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t base_len = (size_t)(mark - CONFIG_APP_OTA_MANIFEST_URL);
    if (base_len == 0 || base_len + 1 > cap) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(out, CONFIG_APP_OTA_MANIFEST_URL, base_len);
    out[base_len] = '\0';
    return ESP_OK;
}

/*
 * Install one specific release: read THAT release's manifest, make sure it is
 * the release that was asked for, and hand its asset URL to the ordinary
 * install path. Downgrades pass; the only version refused is the one already
 * running, and it is refused here before a byte is downloaded -- do_update()
 * re-checks the same rule against the image descriptor afterwards.
 */
static void do_update_version(const char *manifest_url, const char *want)
{
    network_status_t net = {0};
    if (network_manager_get_status(&net) != ESP_OK || !net.has_ip) {
        set_failed("No network");
        return;
    }

    char *body = malloc(OTA_MANIFEST_MAX);
    if (body == NULL) {
        set_failed("Out of memory");
        return;
    }

    esp_err_t ret = fetch_text(manifest_url, body, OTA_MANIFEST_MAX);
    if (ret != ESP_OK) {
        free(body);
        /* The fetch layer reports HTTP != 200 (a missing release's 404 among
         * them) as ESP_ERR_INVALID_RESPONSE, so the console shows one honest
         * "bad server reply" rather than a guess between 404 and a corrupt
         * body. */
        set_failed(ret == ESP_ERR_INVALID_RESPONSE ? "Bad server reply" : "Download failed");
        return;
    }

    ota_release_t rel = {0};
    strlcpy(rel.running, ota_manager_running_version(), sizeof(rel.running));
    ret = parse_manifest(body, &rel);
    free(body);
    if (ret != ESP_OK) {
        set_failed("Bad manifest");
        return;
    }
    if (ota_manager_version_compare(rel.latest, want) != 0) {
        set_failed("Wrong release");
        return;
    }
    if (ota_manager_version_compare(rel.latest, rel.running) == 0) {
        set_failed("Same version");
        return;
    }
    ESP_LOGI(TAG, "release %s -> %s", rel.latest, rel.url);
    do_update(rel.url);
}

/* ---------------- release list ---------------- */

/*
 * `ota list` needs the set of tags that can be installed. The GitHub API's
 * tag list answers as one small JSON array (unlike the releases endpoint,
 * whose per-release objects would dwarf the buffer), so that is what is
 * fetched — filtered to parseable v* tags and sorted newest first. The
 * unauthenticated API allows 60 requests per hour per IP; an operator who
 * needs more than that should be scripting against the repo, not the device.
 */
#define OTA_TAGS_MAX      8192   /* 30 tags of GitHub tag JSON fits with room */
#define OTA_TAGS_PER_PAGE 30

typedef struct {
    char  *out;   /* caller's buffer, written once the fetch completes */
    size_t cap;
} list_job_t;

/* qsort comparator: newest (largest semver) first. */
static int tag_cmp(const void *a, const void *b)
{
    const char *const *ta = a;
    const char *const *tb = b;
    return ota_manager_version_compare((*tb), (*ta));
}

static void fetch_tags(list_job_t *job)
{
    job->out[0] = '\0';

    network_status_t net = {0};
    if (network_manager_get_status(&net) != ESP_OK || !net.has_ip) {
        return;
    }

    char base[OTA_URL_MAX];
    if (releases_base(base, sizeof(base)) != ESP_OK) {
        return;
    }

    /* api.github.com requires the repository as owner/name. A mirror host in
     * the manifest Kconfig therefore gets no list; installs still work. */
    const char *gh = strstr(base, "github.com/");
    if (gh == NULL) {
        return;
    }

    char url[OTA_URL_MAX];
    int n = snprintf(url, sizeof(url), "https://api.github.com/repos/%s/tags?per_page=%d",
                     gh + strlen("github.com/"), OTA_TAGS_PER_PAGE);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        return;
    }

    char *body = malloc(OTA_TAGS_MAX);
    if (body == NULL) {
        return;
    }

    esp_err_t ret = fetch_text(url, body, OTA_TAGS_MAX);
    if (ret != ESP_OK) {
        free(body);
        return;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL || !cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return;
    }

    const char *names[OTA_TAGS_PER_PAGE] = {0};
    int count = 0;
    const cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, root) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(entry, "name");
        if (cJSON_IsString(name) && name->valuestring != NULL &&
            count < OTA_TAGS_PER_PAGE &&
            ota_manager_version_compare(name->valuestring, "0") > 0) {
            names[count++] = name->valuestring;
            if (count == OTA_TAGS_PER_PAGE) {
                break;
            }
        }
    }

    qsort(names, count, sizeof(names[0]), tag_cmp);

    size_t len = 0;
    for (int i = 0; i < count; i++) {
        size_t room = (len + 1 < job->cap) ? (job->cap - 1 - len) : 0;
        int written = snprintf(job->out + len, room + 1, "%s\n", names[i]);
        if (written < 0 || (size_t)written > room) {
            break;   /* buffer full; the newest tags made it in */
        }
        len += (size_t)written;
    }

    cJSON_Delete(root);
}

static void list_task(void *arg)
{
    fetch_tags((list_job_t *)arg);
    free(arg);

    lock();
    s_busy = false;
    unlock();
    vTaskDelete(NULL);
}

/* ---------------- worker dispatch ---------------- */

static void ota_task(void *arg)
{
    (void)arg;

    lock();
    ota_req_t req = s_req;
    char url[OTA_URL_MAX];
    strlcpy(url, s_req_url, sizeof(url));
    char version[OTA_VERSION_MAX];
    strlcpy(version, s_req_version, sizeof(version));
    unlock();

    if (req == OTA_REQ_CHECK) {
        do_check();
    } else if (req == OTA_REQ_UPDATE_VERSION) {
        do_update_version(url, version);
    } else {
        do_update(url);
    }

    lock();
    s_busy = false;
    unlock();
    vTaskDelete(NULL);
}

static esp_err_t spawn(ota_req_t req, const char *url, const char *version)
{
    lock();
    if (s_busy) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    s_req = req;
    strlcpy(s_req_url, url != NULL ? url : "", sizeof(s_req_url));
    strlcpy(s_req_version, version != NULL ? version : "", sizeof(s_req_version));
    s_state = (req == OTA_REQ_UPDATE) ? OTA_STATE_DOWNLOADING : OTA_STATE_CHECKING;
    s_percent = 0;
    s_err[0] = '\0';
    unlock();

    BaseType_t ok = xTaskCreate(ota_task, "ota",
                                CONFIG_APP_OTA_TASK_STACK_SIZE, NULL,
                                CONFIG_APP_OTA_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        lock();
        s_busy = false;
        unlock();
        set_failed("Out of memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_manager_request_check(void)
{
    return spawn(OTA_REQ_CHECK, NULL, NULL);
}

esp_err_t ota_manager_request_update(const char *url)
{
    char chosen[OTA_URL_MAX];

    if (url != NULL && url[0] != '\0') {
        strlcpy(chosen, url, sizeof(chosen));
    } else {
        lock();
        bool have = s_have_release && s_release.url[0] != '\0';
        if (have) {
            strlcpy(chosen, s_release.url, sizeof(chosen));
        }
        unlock();
        if (!have) {
            return ESP_ERR_INVALID_ARG;   /* nothing checked, nothing given */
        }
    }
    return spawn(OTA_REQ_UPDATE, chosen, NULL);
}

esp_err_t ota_manager_request_update_version(const char *version)
{
    if (version == NULL || version[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    /* Accept "1.0.1" as "v1.0.1": every release tag starts with v, and an
     * operator who just saw the list should not have to remember that. */
    char tag[OTA_VERSION_MAX];
    if (version[0] == 'v' || version[0] == 'V') {
        strlcpy(tag, version, sizeof(tag));
    } else {
        int n = snprintf(tag, sizeof(tag), "v%s", version);
        if (n < 0 || (size_t)n >= sizeof(tag)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    char manifest_url[OTA_URL_MAX];
    char base[OTA_URL_MAX];
    esp_err_t ret = releases_base(base, sizeof(base));
    if (ret != ESP_OK) {
        return ret;
    }
    int n = snprintf(manifest_url, sizeof(manifest_url),
                     "%s/releases/download/%s/manifest.json", base, tag);
    if (n < 0 || (size_t)n >= sizeof(manifest_url)) {
        return ESP_ERR_INVALID_ARG;
    }
    return spawn(OTA_REQ_UPDATE_VERSION, manifest_url, tag);
}

esp_err_t ota_manager_request_list(char *out, size_t cap)
{
    if (out == NULL || cap < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    lock();
    if (s_busy) {
        unlock();
        return ESP_ERR_INVALID_STATE;
    }
    s_busy = true;
    s_err[0] = '\0';
    unlock();

    out[0] = '\0';
    list_job_t *job = malloc(sizeof(list_job_t));
    if (job == NULL) {
        lock();
        s_busy = false;
        unlock();
        return ESP_ERR_NO_MEM;
    }
    job->out = out;
    job->cap = cap;

    BaseType_t ok = xTaskCreate(list_task, "ota_list",
                                CONFIG_APP_OTA_TASK_STACK_SIZE, job,
                                CONFIG_APP_OTA_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        free(job);
        lock();
        s_busy = false;
        unlock();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

ota_state_t ota_manager_get_state(int *percent, char *err, size_t err_len)
{
    lock();
    ota_state_t st = s_state;
    if (percent != NULL) {
        *percent = s_percent;
    }
    if (err != NULL && err_len > 0) {
        strlcpy(err, s_err, err_len);
    }
    unlock();
    return st;
}

bool ota_manager_get_release(ota_release_t *out)
{
    if (out == NULL) {
        return false;
    }
    lock();
    bool have = s_have_release;
    if (have) {
        *out = s_release;
    }
    unlock();
    return have;
}

bool ota_manager_busy(void)
{
    lock();
    bool busy = s_busy;
    unlock();
    return busy;
}

bool ota_manager_is_downloading(void)
{
    lock();
    bool dl = (s_state == OTA_STATE_DOWNLOADING);
    unlock();
    return dl;
}

/* ---------------- rollback / self-test ---------------- */

bool ota_manager_pending_verify(void)
{
    return s_pending_verify;
}

esp_err_t ota_manager_mark_valid(void)
{
    esp_err_t ret = esp_ota_mark_app_valid_cancel_rollback();
    if (ret == ESP_OK) {
        s_pending_verify = false;
        if (s_selftest_timer != NULL) {
            esp_timer_stop(s_selftest_timer);
        }
    }
    return ret;
}

esp_err_t ota_manager_rollback(void)
{
    /* Does not return on success: it reboots into the other slot. */
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}

static void selftest_tick(void *arg)
{
    (void)arg;

    if (!s_pending_verify) {
        return;
    }
    if (esp_timer_get_time() < (int64_t)CONFIG_APP_OTA_SELFTEST_DELAY_S * 1000000) {
        return;
    }

    /*
     * The gate is deliberately local: the image is up, and the metering chip —
     * the one peripheral this product exists to read — is answering. Network
     * state is NOT part of it. Requiring a link would make an unplugged cable
     * indistinguishable from a bad build, and would roll a perfectly good image
     * back every time the site's switch was down.
     */
    if (system_status_get(SYS_MODULE_ATM90) != SYS_STATUS_READY) {
        ESP_LOGW(TAG, "self-test waiting: metering chip not ready");
        return;
    }

    esp_err_t ret = ota_manager_mark_valid();
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "self-test passed; image committed, rollback cancelled");
    } else {
        ESP_LOGE(TAG, "could not commit image: %s", esp_err_to_name(ret));
    }
}

esp_err_t ota_manager_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running %s from %s", ota_manager_running_version(),
             running != NULL ? running->label : "?");

    esp_ota_img_states_t img_state;
    if (running == NULL || esp_ota_get_state_partition(running, &img_state) != ESP_OK) {
        return ESP_OK;   /* factory/USB-flashed image with no otadata entry */
    }
    if (img_state != ESP_OTA_IMG_PENDING_VERIFY) {
        return ESP_OK;
    }

    s_pending_verify = true;
    ESP_LOGW(TAG, "image is on probation; committing after %ds if the meter stays up",
             CONFIG_APP_OTA_SELFTEST_DELAY_S);

    const esp_timer_create_args_t args = {
        .callback = selftest_tick,
        .name     = "ota_selftest",
    };
    esp_err_t ret = esp_timer_create(&args, &s_selftest_timer);
    if (ret != ESP_OK) {
        /* Without the timer the image can never commit and would roll back on
         * the next reset. Commit now rather than strand a working device. */
        ESP_LOGE(TAG, "self-test timer failed (%s); committing immediately",
                 esp_err_to_name(ret));
        return ota_manager_mark_valid();
    }
    return esp_timer_start_periodic(s_selftest_timer, OTA_SELFTEST_TICK_US);
}

#else /* !CONFIG_APP_OTA_ENABLE */

esp_err_t   ota_manager_init(void)                       { return ESP_OK; }
esp_err_t   ota_manager_request_check(void)              { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t   ota_manager_request_update(const char *url)  { (void)url; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t   ota_manager_request_update_version(const char *v)
                                                         { (void)v; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t   ota_manager_request_list(char *out, size_t cap)
                                                         { (void)out; (void)cap; return ESP_ERR_NOT_SUPPORTED; }
bool        ota_manager_get_release(ota_release_t *out)  { (void)out; return false; }
bool        ota_manager_busy(void)                       { return false; }
bool        ota_manager_is_downloading(void)             { return false; }
bool        ota_manager_pending_verify(void)             { return false; }
esp_err_t   ota_manager_mark_valid(void)                 { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t   ota_manager_rollback(void)                   { return ESP_ERR_NOT_SUPPORTED; }

ota_state_t ota_manager_get_state(int *percent, char *err, size_t err_len)
{
    if (percent != NULL) *percent = 0;
    if (err != NULL && err_len > 0) err[0] = '\0';
    return OTA_STATE_IDLE;
}

#endif /* CONFIG_APP_OTA_ENABLE */

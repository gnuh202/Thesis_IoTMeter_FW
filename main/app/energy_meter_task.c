#include "energy_meter_task.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atm90e32as.h"
#include "alarm_manager.h"
#include "config_manager.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "measurement_data.h"
#include "modbus_master_task.h"
#include "network_manager.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sd_card.h"
#include "sdkconfig.h"
#include "spi_bus_shared.h"
#include "system_status.h"
#include "time_source.h"
#include "esp_crc.h"

#ifndef CONFIG_APP_ATM90E32AS_DEFAULT_MODE_3P3W
#define CONFIG_APP_ATM90E32AS_DEFAULT_MODE_3P3W 0
#endif

#ifndef CONFIG_APP_METER_NOLOAD_CURRENT_MA
#define CONFIG_APP_METER_NOLOAD_CURRENT_MA 50
#endif

/* Energy persistence / SD logging knobs (Kconfig "Energy persistence and
 * logging"); fallbacks keep the file building against an older sdkconfig. */
#ifndef CONFIG_APP_ENERGY_PERSIST_THRESHOLD_WH
#define CONFIG_APP_ENERGY_PERSIST_THRESHOLD_WH 50
#endif
#ifndef CONFIG_APP_ENERGY_PERSIST_PERIOD_S
#define CONFIG_APP_ENERGY_PERSIST_PERIOD_S 600
#endif
#ifndef CONFIG_APP_ENERGY_SD_LOG_PERIOD_S
#define CONFIG_APP_ENERGY_SD_LOG_PERIOD_S 300
#endif
#ifndef CONFIG_APP_ENERGY_SD_LOG_ENABLE
#define CONFIG_APP_ENERGY_SD_LOG_ENABLE 1
#endif
#ifndef CONFIG_APP_ENERGY_SD_LOG_MAX_KB
#define CONFIG_APP_ENERGY_SD_LOG_MAX_KB 8192
#endif

/* Fallbacks if sdkconfig not yet regenerated after Kconfig change. */
#ifndef CONFIG_APP_ATM90E32AS_DEFAULT_PGA_4X
#ifndef CONFIG_APP_ATM90E32AS_DEFAULT_PGA_2X
#ifndef CONFIG_APP_ATM90E32AS_DEFAULT_PGA_1X
#define CONFIG_APP_ATM90E32AS_DEFAULT_PGA_1X 1
#endif
#endif
#endif

#ifndef CONFIG_APP_ATM90E32AS_DEFAULT_LINE_FREQ_60HZ
#ifndef CONFIG_APP_ATM90E32AS_DEFAULT_LINE_FREQ_50HZ
#define CONFIG_APP_ATM90E32AS_DEFAULT_LINE_FREQ_50HZ 1
#endif
#endif

#ifndef CONFIG_APP_ATM90E32AS_R_BURDEN_MOHM
#define CONFIG_APP_ATM90E32AS_R_BURDEN_MOHM 4400
#endif
#ifndef CONFIG_APP_ATM90E32AS_CT_RATIO
#define CONFIG_APP_ATM90E32AS_CT_RATIO 2000
#endif
#ifndef CONFIG_APP_ATM90E32AS_I_RATED_A
#define CONFIG_APP_ATM90E32AS_I_RATED_A 100
#endif
#ifndef CONFIG_APP_ATM90E32AS_I_EXPECTED_A
#define CONFIG_APP_ATM90E32AS_I_EXPECTED_A 75
#endif

/* CT / PGA auto-select constants (locked product design). */
#define ENERGY_METER_VADC_LIMIT_V   0.72f          /* 720 mVrms full-scale target */
#define ENERGY_METER_FACTORY_GAIN   0x8000U        /* IC default UGAIN/IGAIN */
#define ENERGY_METER_R_BURDEN_OHM \
    ((float)CONFIG_APP_ATM90E32AS_R_BURDEN_MOHM / 1000.0f)

static const char *TAG = "energy_meter";
static atm90e32as_handle_t s_meter;
static SemaphoreHandle_t s_meter_mutex;
static SemaphoreHandle_t s_measurements_mutex;
static atm90e32as_calib_t s_current_calib;
static atm90e32as_calib_t s_applied_calib;
static atm90e32as_measurements_t s_latest_measurements;
static bool s_measurements_valid;

/* Energy accumulators (Wh / varh) protected by s_measurements_mutex. */
static double s_active_import_wh;
static double s_active_export_wh;
static double s_reactive_import_varh;
static double s_reactive_export_varh;

/* Demand: moving average of total active power over a configurable window.
 * Integrated over real elapsed time (P*dt) rather than counted in samples, so
 * a paused or failed tick shortens the data the average is built from instead
 * of silently stretching a "15 minute" window into 25 real minutes. */
static double s_demand_accum_ws;     /* watt-seconds accumulated this window */
static double s_demand_elapsed_s;    /* seconds of real data in this window */
static int64_t s_demand_last_us;     /* esp_timer stamp of the previous sample */
static float s_demand_value_w;
static float s_demand_max_w;
static uint16_t s_demand_window_min = 15;

/* Energy persistence bookkeeping (s_measurements_mutex for the counters,
 * plain statics for the task-local timers). */
static double s_persist_saved_total_wh;  /* sum of all four counters at last save */
static int64_t s_persist_last_save_us;
static bool s_persist_dirty;

#define ENERGY_METER_CALIB_MAGIC 0x9032CA1BU
/* Single calibration profile (no 3W/4W slots — wiring switch only drives relay,
 * phase gains remain common across modes). Magic kept for blob identification
 * on NVS load; legacy v2/v3 blobs (with version + profile[2]) are rejected on
 * size mismatch — user erases flash whenever the NVS layout changes. */
#define ENERGY_METER_NVS_NAMESPACE "atm90e32as"
#define ENERGY_METER_NVS_CALIB_KEY "profiles_v2"
#define ENERGY_METER_RELAY_SETTLE_MS 20
#define ENERGY_METER_MEASUREMENT_SETTLE_MS 200

typedef struct {
    uint32_t magic;
    atm90e32as_calib_t calib;
} energy_meter_calib_blob_t;

/* Single calibration profile — phase gains + chip-wide stamps (pga, line_freq,
 * wiring_mode). The same struct is used for the active calibration in RAM, the
 * NVS blob, and the SD CALB payload source. Switching wiring mode only
 * changes the chip mode bit / relay; phase gains are never touched. */
static atm90e32as_calib_t s_calib;

static esp_err_t energy_meter_apply_locked(const atm90e32as_calib_t *target);
static void energy_meter_stamp_chipwide_locked(atm90e32as_pga_gain_t pga,
                                              atm90e32as_line_freq_t freq);

/* ---------------------------------------------------------------------------
 * Energy persistence
 *
 * The ATM90E32AS total-energy registers are read-to-clear (datasheet Table-11,
 * type R/C): the chip stores nothing, so the firmware's RAM accumulators ARE
 * the meter reading. Without this they reset to zero on every reboot, OTA or
 * brownout — unacceptable for a meter.
 *
 * Two alternating slots + CRC32 make a write atomic in practice: power lost
 * mid-write leaves the previous slot intact, and the loader simply picks the
 * highest sequence number that passes CRC. Writes are triggered by accumulated
 * energy rather than by a timer, so flash wear scales with actual consumption.
 * ------------------------------------------------------------------------- */
#define ENERGY_ACCUM_NVS_NAMESPACE "energy"
#define ENERGY_ACCUM_KEY_A         "accum_a"
#define ENERGY_ACCUM_KEY_B         "accum_b"
#define ENERGY_ACCUM_MAGIC         0xE4E59001U
#define ENERGY_ACCUM_VERSION       2U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t seq;                 /* newer slot wins; wraps harmlessly */
    double   active_import_wh;
    double   active_export_wh;
    double   reactive_import_varh;
    double   reactive_export_varh;
    float    demand_max_w;
    uint16_t demand_window_min;
    uint16_t reserved;
    uint32_t crc32;               /* over everything above */
} energy_accum_blob_t;

/* The struct is 8-byte aligned because of the doubles, so it carries four bytes
 * of TRAILING padding after crc32. That makes "sizeof(blob) - sizeof(crc32)"
 * land four bytes past the start of crc32 instead of on it — the checksummed
 * region would then include the checksum field itself, which is zero while
 * saving and non-zero while loading, so every single load would fail CRC and
 * silently reset the meter to zero. The CRC must be taken over exactly the
 * bytes BEFORE crc32; offsetof() is the only expression that says that.
 * The assert pins the layout so no future field insertion can reintroduce
 * padding inside the checksummed region. */
_Static_assert(offsetof(energy_accum_blob_t, crc32) ==
                   sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                   4 * sizeof(double) + sizeof(float) + 2 * sizeof(uint16_t),
               "unexpected padding inside energy_accum_blob_t");

static uint16_t s_persist_seq;

static uint32_t energy_accum_crc(const energy_accum_blob_t *b)
{
    return esp_crc32_le(0, (const uint8_t *)b,
                        offsetof(energy_accum_blob_t, crc32));
}

/* Load the newest valid slot. Missing or corrupt on both slots is not an
 * error: a first boot (or a flash erase) legitimately starts from zero. */
static void energy_accum_load(void)
{
    nvs_handle_t nvs;
    if (nvs_open(ENERGY_ACCUM_NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        ESP_LOGI(TAG, "no persisted energy yet; counters start at zero");
        return;
    }

    const char *keys[2] = { ENERGY_ACCUM_KEY_A, ENERGY_ACCUM_KEY_B };
    energy_accum_blob_t best;
    bool have_best = false;

    for (int i = 0; i < 2; i++) {
        energy_accum_blob_t blob;
        size_t len = sizeof(blob);
        if (nvs_get_blob(nvs, keys[i], &blob, &len) != ESP_OK || len != sizeof(blob)) {
            continue;
        }
        if (blob.magic != ENERGY_ACCUM_MAGIC || blob.version != ENERGY_ACCUM_VERSION) {
            continue;
        }
        if (blob.crc32 != energy_accum_crc(&blob)) {
            ESP_LOGW(TAG, "energy slot %s failed CRC; ignoring", keys[i]);
            continue;
        }
        /* Signed comparison of the difference handles seq wrap-around. */
        if (!have_best || (int16_t)(blob.seq - best.seq) > 0) {
            best = blob;
            have_best = true;
        }
    }
    nvs_close(nvs);

    if (!have_best) {
        ESP_LOGI(TAG, "no valid persisted energy; counters start at zero");
        return;
    }

    s_active_import_wh = best.active_import_wh;
    s_active_export_wh = best.active_export_wh;
    s_reactive_import_varh = best.reactive_import_varh;
    s_reactive_export_varh = best.reactive_export_varh;
    s_demand_max_w = best.demand_max_w;
    if (best.demand_window_min > 0) {
        s_demand_window_min = best.demand_window_min;
    }
    s_persist_seq = best.seq;
    s_persist_saved_total_wh = s_active_import_wh + s_active_export_wh +
                               s_reactive_import_varh + s_reactive_export_varh;

    ESP_LOGI(TAG, "energy restored: %.3f kWh import, %.3f kWh export, "
                  "%.3f kvarh import, %.3f kvarh export (seq %u)",
             s_active_import_wh / 1000.0, s_active_export_wh / 1000.0,
             s_reactive_import_varh / 1000.0, s_reactive_export_varh / 1000.0,
             (unsigned)best.seq);
}

/* Write the counters to the slot NOT holding the current newest copy, so a
 * failed write never destroys the last good value. */
static esp_err_t energy_accum_save(void)
{
    if (s_measurements_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    energy_accum_blob_t blob = {
        .magic = ENERGY_ACCUM_MAGIC,
        .version = ENERGY_ACCUM_VERSION,
    };

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    blob.active_import_wh = s_active_import_wh;
    blob.active_export_wh = s_active_export_wh;
    blob.reactive_import_varh = s_reactive_import_varh;
    blob.reactive_export_varh = s_reactive_export_varh;
    blob.demand_max_w = s_demand_max_w;
    blob.demand_window_min = s_demand_window_min;
    double total = s_active_import_wh + s_active_export_wh +
                   s_reactive_import_varh + s_reactive_export_varh;
    xSemaphoreGive(s_measurements_mutex);

    blob.seq = (uint16_t)(s_persist_seq + 1);
    blob.crc32 = energy_accum_crc(&blob);

    /* Odd sequence -> slot A, even -> slot B. Alternating on every write keeps
     * the previous copy readable for the whole duration of this one. */
    const char *key = (blob.seq & 1U) ? ENERGY_ACCUM_KEY_A : ENERGY_ACCUM_KEY_B;

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(ENERGY_ACCUM_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_blob(nvs, key, &blob, sizeof(blob));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (ret == ESP_OK) {
        s_persist_seq = blob.seq;
        s_persist_saved_total_wh = total;
        s_persist_last_save_us = esp_timer_get_time();
        s_persist_dirty = false;
        ESP_LOGD(TAG, "energy persisted to %s (seq %u, %.3f kWh)",
                 key, (unsigned)blob.seq, blob.active_import_wh / 1000.0);
    }
    return ret;
}

/* Save when enough energy has accumulated to be worth a flash write, or when
 * the background period expires on a lightly loaded meter. */
static void energy_accum_service(void)
{
    if (s_measurements_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    double total = s_active_import_wh + s_active_export_wh +
                   s_reactive_import_varh + s_reactive_export_varh;
    xSemaphoreGive(s_measurements_mutex);

    double delta = total - s_persist_saved_total_wh;
    if (delta < 0.0) {
        delta = -delta;    /* a reset moved the counters backwards */
    }
    if (delta > 0.0) {
        s_persist_dirty = true;
    }

    bool by_energy = delta >= (double)CONFIG_APP_ENERGY_PERSIST_THRESHOLD_WH;
    bool by_time = s_persist_dirty &&
                   (esp_timer_get_time() - s_persist_last_save_us) >=
                       ((int64_t)CONFIG_APP_ENERGY_PERSIST_PERIOD_S * 1000000LL);

    if (by_energy || by_time) {
        esp_err_t ret = energy_accum_save();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "energy persist failed: %s", esp_err_to_name(ret));
        }
    }
}

/* Kconfig "ATM90E32AS Parameters" — PGA fixed at 4× per thesis requirement. */
static atm90e32as_pga_gain_t energy_meter_kconfig_default_pga(void)
{
    return ATM90E32AS_PGA_GAIN_4X;  /* Fixed per CONFIG_APP_ATM90E32AS_FIXED_PGA_4X */
}

static atm90e32as_line_freq_t energy_meter_kconfig_default_line_freq(void)
{
#if CONFIG_APP_ATM90E32AS_DEFAULT_LINE_FREQ_60HZ
    return ATM90E32AS_LINE_FREQ_60HZ;
#else
    return ATM90E32AS_LINE_FREQ_50HZ;
#endif
}

/* Convert PGA enum to the integer × multiplier used by the Ilim formula.
 * Public so LCD / console code can format PGA as "×N". */
unsigned energy_meter_pga_mult(atm90e32as_pga_gain_t pga)
{
    switch (pga) {
    case ATM90E32AS_PGA_GAIN_2X: return 2U;
    case ATM90E32AS_PGA_GAIN_4X: return 4U;
    case ATM90E32AS_PGA_GAIN_1X:
    default: return 1U;
    }
}

/* config_manager.pga stores the × multiplier (1/2/4). 0 = unset. */
static atm90e32as_pga_gain_t energy_meter_pga_from_config_u8(uint8_t pga)
{
    switch (pga) {
    case 4U: return ATM90E32AS_PGA_GAIN_4X;
    case 2U: return ATM90E32AS_PGA_GAIN_2X;
    case 1U:
    default: return ATM90E32AS_PGA_GAIN_1X;
    }
}

static uint8_t energy_meter_pga_to_config_u8(atm90e32as_pga_gain_t pga)
{
    return (uint8_t)energy_meter_pga_mult(pga);
}

/* Persist system PGA into config snapshot (does not apply chip). */
static esp_err_t energy_meter_persist_pga_to_config(atm90e32as_pga_gain_t pga)
{
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = config_manager_get(cfg);
    if (ret == ESP_OK) {
        uint8_t want = energy_meter_pga_to_config_u8(pga);
        if (cfg->pga != want) {
            cfg->pga = want;
            ret = config_manager_update(cfg);
            if (ret == ESP_OK) {
                ret = config_manager_save();
            }
        }
    }
    free(cfg);
    return ret;
}

/* Resolve system PGA: config (1/2/4) → else migrate from calib/Kconfig once. */
static atm90e32as_pga_gain_t energy_meter_resolve_system_pga(atm90e32as_pga_gain_t calib_hint,
                                                            bool *out_need_persist)
{
    if (out_need_persist) {
        *out_need_persist = false;
    }
    (void)calib_hint;
    return ATM90E32AS_PGA_GAIN_4X;  /* Fixed; CT ratio changes rescale digitally */
}

/* Ilim(pga) = VADC_limit * NCT / (R_burden * pga_mult)  [primary amps] */
static float energy_meter_ilim_a(uint16_t ct_ratio, unsigned pga_mult)
{
    float r = ENERGY_METER_R_BURDEN_OHM;
    if (r <= 0.0f || pga_mult == 0U || ct_ratio == 0U) {
        return 0.0f;
    }
    return (ENERGY_METER_VADC_LIMIT_V * (float)ct_ratio) / (r * (float)pga_mult);
}

/*
 * PGA auto-select (locked):
 *   R_BURDEN is Kconfig-only; VADC_LIMIT = 720 mVrms.
 *   Ilim(PGA) = 0.72 * NCT / (R_Burden * PGA).
 *
 *   Roles:
 *     - I_Expected: must always be covered by the chosen PGA. If a higher PGA
 *       would clip I_Expected, we fall back to the lower one.
 *     - I_Rated: the nameplate headroom. If a higher PGA clips I_Rated but
 *       still covers I_Expected, we accept the trade-off (operator warning,
 *       but PGA stays higher for accuracy at the operating point).
 *
 *   Walk 1X → 2X → 4X:
 *     - If Ilim(1) < I_Expected → PGA=1, range warning (cannot go lower),
 *                                  I_Expected is left unchanged in the
 *                                  result (operator must lower Expected).
 *     - Else if Ilim(1) < I_Rated → PGA=1 (cannot go higher without
 *                                    sacrificing Rated; stop here).
 *     - Else try 2X:
 *         - If Ilim(2) < I_Expected → PGA=1 (2X would clip Expected).
 *         - Else if Ilim(2) < I_Rated → PGA=2 (2X clips Rated, stop here).
 *         - Else try 4X:
 *             - If Ilim(4) < I_Expected → PGA=2 (4X would clip Expected).
 *             - Else                    → PGA=4 (4X clips Rated but Expected
 *                                          still fits; trade-off accepted).
 *
 *   Trade-off case (e.g. 100A Rated + 75A Expected + PGA=4 → Ilim=81.8A)
 *   sets rated_truncated=true so the LCD / console can warn the operator
 *   that headroom above I_Rated was sacrificed for accuracy at I_Expected.
 */
esp_err_t energy_meter_ct_select_pga(uint16_t ct_ratio, uint16_t i_rated_a,
                                     uint16_t i_expected_a,
                                     energy_meter_ct_apply_result_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    /* NCT: 1000..6000, multiples of 100 (product LCD step). */
    ESP_RETURN_ON_FALSE(ct_ratio >= 1000U && ct_ratio <= 6000U && (ct_ratio % 100U) == 0U &&
                        i_rated_a >= 1U && i_expected_a >= 1U,
                        ESP_ERR_INVALID_ARG, TAG, "invalid CT params");

    memset(out, 0, sizeof(*out));
    out->ct_ratio = ct_ratio;
    out->i_rated_a = i_rated_a;
    out->i_expected_a = i_expected_a;
    out->pga = ATM90E32AS_PGA_GAIN_4X;  /* Fixed */

    /* Ilim at PGA=4: 0.72 * NCT / (R_burden * 4) */
    out->ilim_a = energy_meter_ilim_a(ct_ratio, 4U);

    /* Range warnings if Ilim4 cannot cover the operator's requirements.
     * No fallback to lower PGA — digital rescale absorbs CT changes. */
    if (out->ilim_a + 1e-6f < (float)i_expected_a) {
        out->expected_clamped = true;
    }
    if (out->ilim_a + 1e-6f < (float)i_rated_a) {
        out->rated_truncated = true;
    }
    return ESP_OK;
}

esp_err_t energy_meter_ct_apply(uint16_t ct_ratio, uint16_t i_rated_a,
                                uint16_t i_expected_a,
                                bool save_calib_nvs,
                                energy_meter_ct_apply_result_t *out)
{
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    energy_meter_ct_apply_result_t local;
    energy_meter_ct_apply_result_t *r = out ? out : &local;
    esp_err_t ret = energy_meter_ct_select_pga(ct_ratio, i_rated_a, i_expected_a, r);
    if (ret != ESP_OK) {
        return ret;
    }

    /* CT is a ratio scaling factor. With fixed PGA=4×, Igain survives CT swaps;
     * measurement path rescales by (NCT_current / NCT_calib). Stamping PGA=4
     * here is idempotent (already 4), kept for uniformity with line_freq stamp. */
    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    s_current_calib.pga_gain = r->pga;
    /* PGA is chip-wide runtime stamp only; authoritative store is config_manager. */
    energy_meter_stamp_chipwide_locked(r->pga, s_current_calib.line_freq);
    s_calib = s_current_calib;
    ret = energy_meter_apply_locked(&s_current_calib);
    xSemaphoreGive(s_meter_mutex);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CT apply failed: 0x%x", ret);
        return ret;
    }

    /* System PGA lives in config snapshot (with CT params saved by caller). */
    esp_err_t pga_cfg_ret = energy_meter_persist_pga_to_config(r->pga);
    if (pga_cfg_ret != ESP_OK) {
        ESP_LOGW(TAG, "CT apply: PGA chip OK but config persist failed: %s",
                 esp_err_to_name(pga_cfg_ret));
    }

    ESP_LOGI(TAG,
             "CT apply: NCT=%u Rated=%uA Expected=%uA PGA=%ux Ilim=%.1fA%s%s (Igain kept)",
             (unsigned)r->ct_ratio, (unsigned)r->i_rated_a, (unsigned)r->i_expected_a,
             energy_meter_pga_mult(r->pga), (double)r->ilim_a,
             r->expected_clamped ? " [exp range warning]" : "",
             r->rated_truncated ? " [rated truncated]" : "");

    if (save_calib_nvs) {
        ret = energy_meter_save_calibration();
    }
    return ret;
}

static void energy_meter_default_profiles(void)
{
    /* Phase-gain factory defaults only. System PGA is stamped later from
     * config_manager (or one-shot migrate). Do not recompute CT→PGA here. */
    atm90e32as_pga_gain_t pga = energy_meter_kconfig_default_pga();
    atm90e32as_line_freq_t freq = energy_meter_kconfig_default_line_freq();

    atm90e32as_get_default_calib(&s_calib);
    s_calib.pga_gain = pga;
    s_calib.line_freq = freq;
}

static esp_err_t energy_meter_nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "erase NVS failed");
        ret = nvs_flash_init();
    }
    return ret == ESP_ERR_INVALID_STATE ? ESP_OK : ret;
}

static esp_err_t energy_meter_load_calibration_from_nvs(atm90e32as_calib_t *calib)
{
    ESP_RETURN_ON_ERROR(energy_meter_nvs_init(), TAG, "init NVS failed");

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(ENERGY_METER_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (ret != ESP_OK) return ret;

    energy_meter_calib_blob_t blob;
    size_t size = sizeof(blob);
    ret = nvs_get_blob(nvs, ENERGY_METER_NVS_CALIB_KEY, &blob, &size);
    nvs_close(nvs);
    if (ret != ESP_OK) return ret;
    /* Single-profile layout. Legacy v2/v3 blobs (with version + profile[2])
     * fail this size check and fall back to defaults — user erases flash on
     * layout changes anyway. */
    if (size != sizeof(blob) || blob.magic != ENERGY_METER_CALIB_MAGIC ||
        atm90e32as_validate_calibration(&blob.calib) != ESP_OK) {
        return ESP_ERR_INVALID_VERSION;
    }

    s_calib = blob.calib;
    *calib = s_calib;
    return ESP_OK;
}

static esp_err_t energy_meter_save_calibration_to_nvs(const atm90e32as_calib_t *calib)
{
    ESP_RETURN_ON_ERROR(energy_meter_nvs_init(), TAG, "init NVS failed");

    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(ENERGY_METER_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open calibration NVS failed");

    s_calib = *calib;
    energy_meter_calib_blob_t blob = {
        .magic = ENERGY_METER_CALIB_MAGIC,
        .calib = s_calib,
    };

    esp_err_t ret = nvs_set_blob(nvs, ENERGY_METER_NVS_CALIB_KEY, &blob, sizeof(blob));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);

    /* Stamp ct_ratio_calib when saving a fresh calibration: this CT's NCT
     * becomes the rescale baseline. Future CT swaps rescale digitally. */
    if (ret == ESP_OK) {
        config_manager_t *cfg = malloc(sizeof(*cfg));
        if (cfg != NULL && config_manager_get(cfg) == ESP_OK) {
            if (cfg->ct_ratio >= 1000U && cfg->ct_ratio_calib != cfg->ct_ratio) {
                cfg->ct_ratio_calib = cfg->ct_ratio;
                esp_err_t cfg_ret = config_manager_update(cfg);
                if (cfg_ret == ESP_OK) {
                    cfg_ret = config_manager_save();
                }
                if (cfg_ret == ESP_OK) {
                    ESP_LOGI(TAG, "stamped ct_ratio_calib=%u (rescale baseline)",
                             (unsigned)cfg->ct_ratio);
                } else {
                    ESP_LOGW(TAG, "calib saved but ct_ratio_calib stamp failed: %s",
                             esp_err_to_name(cfg_ret));
                }
            }
        }
        free(cfg);
    }
    return ret;
}

static esp_err_t energy_meter_gpio_init(void)
{
    gpio_config_t input_config = {
        .pin_bit_mask = (1ULL << CONFIG_APP_ATM90E32AS_IRQ1_GPIO) |
                        (1ULL << CONFIG_APP_ATM90E32AS_IRQ2_GPIO) |
                        (1ULL << CONFIG_APP_ATM90E32AS_WARN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&input_config), TAG, "configure ATM90E32AS status GPIOs failed");

    gpio_config_t mode_sel_config = {
        .pin_bit_mask = 1ULL << CONFIG_APP_ATM90E32AS_MODE_SEL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&mode_sel_config), TAG, "configure mode select GPIO failed");
    /* Relay level is driven from the resolved wiring mode (NVS or default) in
     * energy_meter_init(), after calibration is loaded. Do not set it from the
     * compile-time Kconfig default here, or a saved 3P3W setup would boot the
     * relay into the wrong wiring. */

    return ESP_OK;
}

/* Drive the wiring-mode relay to match the given wiring mode. */
static void energy_meter_set_wiring_relay(atm90e32as_wiring_mode_t wiring_mode)
{
    gpio_set_level(CONFIG_APP_ATM90E32AS_MODE_SEL_GPIO,
                   wiring_mode == ATM90E32AS_WIRING_3P3W ? 1 : 0);
}

static esp_err_t energy_meter_init(void)
{
    ESP_RETURN_ON_ERROR(energy_meter_gpio_init(), TAG, "init energy meter GPIO failed");
    ESP_RETURN_ON_ERROR(spi_bus_shared_init(), TAG, "init shared SPI failed");
    ESP_RETURN_ON_ERROR(sd_card_detect_init(), TAG, "init SD detect failed");

    if (s_measurements_mutex == NULL) {
        s_measurements_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create measurements mutex failed");
    }

    if (s_meter_mutex == NULL) {
        s_meter_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_meter_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create meter mutex failed");
    }

    ESP_RETURN_ON_ERROR(measurement_data_init(), TAG, "init measurement data model failed");

    energy_meter_default_profiles();
    /* Pick default wiring mode (Kconfig) for first-boot bring-up. */
    s_calib.wiring_mode = CONFIG_APP_ATM90E32AS_DEFAULT_MODE_3P3W ? ATM90E32AS_WIRING_3P3W
                                                                   : ATM90E32AS_WIRING_3P4W;
    atm90e32as_calib_t calib = s_calib;

    esp_err_t calib_ret = energy_meter_load_calibration_from_nvs(&calib);
    if (calib_ret == ESP_OK) {
        ESP_LOGI(TAG, "loaded ATM90E32AS calibration from NVS (active %s)",
                 calib.wiring_mode == ATM90E32AS_WIRING_3P3W ? "3P3W" : "3P4W");
    } else {
        ESP_LOGW(TAG, "using ATM90E32AS bring-up defaults; no saved calibration: %s", esp_err_to_name(calib_ret));
    }

    /* System PGA: fixed 4× per thesis requirement (no recal when CT swaps). */
    atm90e32as_pga_gain_t sys_pga = ATM90E32AS_PGA_GAIN_4X;

    calib.pga_gain = sys_pga;
    energy_meter_stamp_chipwide_locked(sys_pga, calib.line_freq);
    esp_err_t pr = energy_meter_persist_pga_to_config(sys_pga);
    if (pr == ESP_OK) {
        ESP_LOGI(TAG, "persisted fixed PGA=4× into config snapshot");
    } else {
        ESP_LOGW(TAG, "PGA persist to config failed: %s", esp_err_to_name(pr));
    }

    s_current_calib = calib;
    s_applied_calib = calib;

    /* Switch the wiring-mode relay to the resolved mode right after MCU power-up,
     * so a saved 3P3W/3P4W setup takes effect on boot without any user action. */
    energy_meter_set_wiring_relay(calib.wiring_mode);

    atm90e32as_config_t config = {
        .cs_gpio = CONFIG_APP_ATM90E32AS_CS_GPIO,
        .spi_clock_hz = CONFIG_APP_ATM90E32AS_SPI_CLOCK_HZ,
        .calib = calib,
    };

    ESP_RETURN_ON_ERROR(atm90e32as_create(&config, &s_meter), TAG, "create ATM90E32AS failed");
    ESP_RETURN_ON_ERROR(atm90e32as_init(s_meter), TAG, "init ATM90E32AS failed");

    if (calib_ret != ESP_OK) {
        ESP_LOGW(TAG, "ATM90E32AS uses bring-up calibration defaults. Replace with measured board calibration before thesis measurements.");
    }
    ESP_LOGI(TAG, "SD card inserted=%d", sd_card_is_inserted());
    return ESP_OK;
}

/* Noise-floor cleanup, applied once per poll BEFORE the snapshot is published
 * anywhere (LCD, Modbus slave, MQTT, demand accumulator). A de-energised or
 * idling meter still shows sub-LSB chip noise, but |PF| < 0.1 and
 * |P|/|Q|/|S| < 1 (W/var/VA) are not physical values in any real installation,
 * so they are reported as exactly 0. Calibration paths never go through here —
 * they must see the raw chip truth. */
#define ENERGY_METER_PF_NOISE_FLOOR 0.1f
#define ENERGY_METER_POWER_NOISE_FLOOR 1.0f
#define ENERGY_METER_NOLOAD_CURRENT_A ((float)CONFIG_APP_METER_NOLOAD_CURRENT_MA / 1000.0f)
#define ENERGY_METER_NOLOAD_RELEASE_A (ENERGY_METER_NOLOAD_CURRENT_A * 0.8f)

/* Per-phase no-load gate, applied before the value floors below. CTs mounted
 * close together in the cabinet cross-couple: a loaded phase induces a small
 * coherent 50 Hz current on the idle CTs (measured ~16 mA with a 7 A adjacent
 * load), and the P/PF computed from that pickup are meaningless. Hysteresis
 * (enter at the threshold, release at 80% of it) keeps a current hovering at
 * the threshold from flapping the displayed values. */
static bool s_phase_loaded[ATM90E32AS_PHASE_COUNT];

static bool energy_meter_phase_is_loaded(float current_a, bool was_loaded)
{
    return was_loaded ? (current_a >= ENERGY_METER_NOLOAD_RELEASE_A)
                      : (current_a >= ENERGY_METER_NOLOAD_CURRENT_A);
}

static void energy_meter_apply_noise_floor(atm90e32as_measurements_t *m)
{
    for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
        if (!energy_meter_phase_is_loaded(m->current[i], s_phase_loaded[i])) {
            /* No real load on this phase: the current is crosstalk pickup,
             * so every value derived from it is meaningless — report a
             * clean no-load state. */
            s_phase_loaded[i] = false;
            m->current[i] = 0.0f;
            m->power_factor[i] = 0.0f;
            m->active_power[i] = 0.0f;
            m->reactive_power[i] = 0.0f;
            m->apparent_power[i] = 0.0f;
            continue;
        }
        s_phase_loaded[i] = true;
        if (fabsf(m->power_factor[i]) < ENERGY_METER_PF_NOISE_FLOOR) {
            m->power_factor[i] = 0.0f;
        }
        if (fabsf(m->active_power[i]) < ENERGY_METER_POWER_NOISE_FLOOR) {
            m->active_power[i] = 0.0f;
        }
        if (fabsf(m->reactive_power[i]) < ENERGY_METER_POWER_NOISE_FLOOR) {
            m->reactive_power[i] = 0.0f;
        }
        /* S follows P/Q: once both are noise-zeroed, a leftover apparent value
         * would contradict them on every display. */
        if (fabsf(m->apparent_power[i]) < ENERGY_METER_POWER_NOISE_FLOOR) {
            m->apparent_power[i] = 0.0f;
        }
    }
    if (fabsf(m->total_power_factor) < ENERGY_METER_PF_NOISE_FLOOR) {
        m->total_power_factor = 0.0f;
    }
    if (fabsf(m->total_active_power) < ENERGY_METER_POWER_NOISE_FLOOR) {
        m->total_active_power = 0.0f;
    }
    if (fabsf(m->total_reactive_power) < ENERGY_METER_POWER_NOISE_FLOOR) {
        m->total_reactive_power = 0.0f;
    }
    if (fabsf(m->total_apparent_power) < ENERGY_METER_POWER_NOISE_FLOOR) {
        m->total_apparent_power = 0.0f;
    }
}

/* Fold one read-to-clear energy sample into the running counters.
 *
 * Only the fields whose valid_mask bit is set are accumulated: a register that
 * failed to read was never cleared, so its energy is still in the chip and
 * arrives with the next sample instead of being lost.
 *
 * nct_scale is the CT-ratio rescale factor, applied here for the same reason
 * it is applied to power: the chip measured through the calibration CT, and
 * the operator may since have fitted a different one. Without it the kWh on
 * screen would contradict the kW beside it. */
static void energy_meter_accumulate(const atm90e32as_energy_counts_t *counts, float nct_scale)
{
    if (s_measurements_mutex == NULL) {
        return;
    }
    const double k = (double)ATM90E32AS_ENERGY_COUNT_TO_WH * (double)nct_scale;

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    if (counts->valid_mask & ATM90E32AS_ENERGY_VALID_ACTIVE_IMPORT) {
        s_active_import_wh += counts->active_import * k;
    }
    if (counts->valid_mask & ATM90E32AS_ENERGY_VALID_ACTIVE_EXPORT) {
        s_active_export_wh += counts->active_export * k;
    }
    if (counts->valid_mask & ATM90E32AS_ENERGY_VALID_REACTIVE_IMPORT) {
        s_reactive_import_varh += counts->reactive_import * k;
    }
    if (counts->valid_mask & ATM90E32AS_ENERGY_VALID_REACTIVE_EXPORT) {
        s_reactive_export_varh += counts->reactive_export * k;
    }
    xSemaphoreGive(s_measurements_mutex);
}

#if CONFIG_APP_ENERGY_SD_LOG_ENABLE
/* Append one CSV row to the SD energy log.
 *
 * Column layout (see docs/energy_logging.md — do not reorder, the PC-side
 * tooling reads by position):
 *   timestamp,tq,boot,uptime_s,imp_kwh,exp_kwh,imp_kvarh,exp_kvarh,
 *   dmd_w,dmd_max_w,p_kw,pf,freq
 *
 * `tq` is the time-quality flag from time_source: 'S' with a trusted RTC or a
 * network sync, 'E' on the NVS floor, 'U' when nothing answered — in which case
 * `timestamp` reads 1970. `boot` and `uptime_s` are the trustworthy time axis
 * whenever tq is not 'S': they separate sessions and order rows within one even
 * though every row may show the same date.
 *
 * Grid faults are NOT a column here: they are edges, not a periodic quantity,
 * and a 5-minute sample would miss short ones entirely. They go to
 * /sdcard/EVENTS/FAULTS.CSV with their own timestamps instead.
 *
 * Called with NO mutex held: a FAT write can take tens of milliseconds and
 * holding s_measurements_mutex across it would stall MQTT, Modbus and the LCD.
 * The caller passes a snapshot taken under the mutex instead. */
#define ENERGY_CSV_HEADER \
    "timestamp,tq,boot,uptime_s,imp_kwh,exp_kwh,imp_kvarh,exp_kvarh," \
    "dmd_w,dmd_max_w,p_kw,pf,freq"

static void energy_meter_log_to_sd(const energy_meter_energy_t *e,
                                   const energy_meter_demand_t *d,
                                   float p_total_w, float pf, float freq)
{
    /* The card is hot-pluggable, so "not mounted" is a normal state, not a
     * fault. Log the transition once per episode rather than every period. */
    static bool s_sd_missing_logged;

    if (!sd_card_is_mounted()) {
        if (!s_sd_missing_logged) {
            s_sd_missing_logged = true;
            ESP_LOGI(TAG, "no SD card mounted; energy CSV logging paused");
        }
        return;
    }

    char stamp[TIME_SOURCE_STAMP_LEN];
    time_source_format_stamp(stamp, sizeof(stamp));

    char line[192];
    snprintf(line, sizeof(line),
             "%s,%c,%u,%u,%.3f,%.3f,%.3f,%.3f,%.1f,%.1f,%.3f,%.3f,%.2f",
             stamp,
             time_source_quality_char(),
             (unsigned)time_source_boot_count(),
             (unsigned)time_source_uptime_s(),
             e->active_import_kwh, e->active_export_kwh,
             e->reactive_import_kvarh, e->reactive_export_kvarh,
             d->active_power_demand_w, d->active_power_demand_max_w,
             p_total_w / 1000.0f, pf, freq);

    esp_err_t ret = sd_card_log_energy_csv(ENERGY_CSV_HEADER, line,
                                           CONFIG_APP_ENERGY_SD_LOG_MAX_KB);
    if (ret == ESP_OK) {
        if (s_sd_missing_logged) {
            s_sd_missing_logged = false;
            ESP_LOGI(TAG, "SD card back; energy CSV logging resumed");
        }
    } else {
        ESP_LOGW(TAG, "energy CSV write failed: %s", esp_err_to_name(ret));
    }
}
#endif /* CONFIG_APP_ENERGY_SD_LOG_ENABLE */

static void energy_meter_task(void *arg)
{
    atm90e32as_measurements_t measurements;
    atm90e32as_energy_counts_t energy_counts;

    /* Restore the persisted counters before the first read so nothing is
     * published as zero and then jumps. */
    energy_accum_load();
    s_persist_last_save_us = esp_timer_get_time();
    s_demand_last_us = esp_timer_get_time();

#if CONFIG_APP_ENERGY_SD_LOG_ENABLE
    int64_t sd_log_last_us = esp_timer_get_time();
#endif
    /* While the config portal is open the poll body is skipped, but the
     * read-to-clear energy registers keep filling: a uint16 count caps at
     * 204.8 Wh, so a 2 kW load overflows them in about 6 minutes of portal
     * time and the energy is lost silently. Drain them on this slower tick. */
    int64_t config_mode_drain_us = 0;

    while (1) {
        /* Config portal active: the operator is doing settings, so pause the
         * measurement poll (cooperative; resumes on the tick after it closes).
         * Energy counts are still drained periodically — see above. */
        if (network_manager_is_config_mode()) {
            int64_t now_us = esp_timer_get_time();
            if ((now_us - config_mode_drain_us) >= 10LL * 1000000LL) {
                config_mode_drain_us = now_us;
                xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
                esp_err_t drain_ret = atm90e32as_read_energy_counts(s_meter, &energy_counts);
                xSemaphoreGive(s_meter_mutex);
                if (drain_ret == ESP_OK) {
                    energy_meter_accumulate(&energy_counts, 1.0f);
                }
            }
            /* The demand window must not count portal time as measured data. */
            s_demand_last_us = 0;
            vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_ENERGY_METER_POLL_PERIOD_MS));
            continue;
        }
        config_mode_drain_us = 0;

        xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
        esp_err_t ret = atm90e32as_read_measurements(s_meter, &measurements);
        /* Independent of the measurement read: these registers are
         * read-to-clear, so skipping them because an unrelated read failed
         * only lets them overflow. */
        esp_err_t energy_ret = atm90e32as_read_energy_counts(s_meter, &energy_counts);
        xSemaphoreGive(s_meter_mutex);

        /* CT ratio rescale: if the operator swapped CT since calibration,
         * rescale digitally. PGA=4 fixed → Igain stays valid; only NCT
         * changes. ct_ratio_calib=0 (legacy/unset) → no rescale (1.0×).
         * Energy deltas get the SAME factor as power, otherwise the kWh on
         * screen would contradict the kW right next to it. */
        float nct_scale = 1.0f;
        uint16_t ct_ratio = 0, ct_ratio_calib = 0;
        if (config_manager_get_ct_ratios(&ct_ratio, &ct_ratio_calib) == ESP_OK &&
            ct_ratio_calib >= 1000U && ct_ratio >= 1000U && ct_ratio != ct_ratio_calib) {
            nct_scale = (float)ct_ratio / (float)ct_ratio_calib;
        }

        if (energy_ret == ESP_OK) {
            energy_meter_accumulate(&energy_counts, nct_scale);
        }

        if (ret == ESP_OK) {
            if (nct_scale != 1.0f) {
                for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
                    measurements.current[i] *= nct_scale;
                    measurements.current_peak[i] *= nct_scale;
                    /* Power rescale: P=V×I, I rescaled → P rescaled */
                    measurements.active_power[i] *= nct_scale;
                    measurements.reactive_power[i] *= nct_scale;
                    measurements.apparent_power[i] *= nct_scale;
                }
                measurements.total_active_power *= nct_scale;
                measurements.total_reactive_power *= nct_scale;
                measurements.total_apparent_power *= nct_scale;
            }

            /* Clean the noise floor before anything consumes this snapshot. */
            energy_meter_apply_noise_floor(&measurements);

            xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
            s_latest_measurements = measurements;
            s_measurements_valid = true;

            /* Demand: average power over real elapsed time. Integrating P*dt
             * (rather than averaging samples) keeps a "15 minute" window 15
             * real minutes long even when ticks are missed. A gap longer than
             * twice the poll period is discarded rather than extrapolated —
             * no data is better than invented data. */
            int64_t now_us = esp_timer_get_time();
            if (s_demand_last_us != 0) {
                double dt_s = (double)(now_us - s_demand_last_us) / 1000000.0;
                if (dt_s > 0.0 &&
                    dt_s <= (2.0 * CONFIG_APP_ENERGY_METER_POLL_PERIOD_MS / 1000.0)) {
                    s_demand_accum_ws += (double)measurements.total_active_power * dt_s;
                    s_demand_elapsed_s += dt_s;
                }
            }
            s_demand_last_us = now_us;

            double window_s = (double)(s_demand_window_min ? s_demand_window_min : 1) * 60.0;
            if (s_demand_elapsed_s >= window_s) {
                s_demand_value_w = (float)(s_demand_accum_ws / s_demand_elapsed_s);
                if (s_demand_value_w > s_demand_max_w) {
                    s_demand_max_w = s_demand_value_w;
                }
                s_demand_accum_ws = 0.0;
                s_demand_elapsed_s = 0.0;
            }

            /* Feed the central data model (pure copy; no hardware access). */
            measurement_data_t md = {0};
            int valid_voltage_count = 0;
            for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
                if (measurements.voltage_valid[i]) {
                    md.voltage_avg += measurements.voltage[i];
                    md.voltage_valid_mask |= (uint8_t)(1U << i);
                    valid_voltage_count++;
                }
                md.current_avg += measurements.current[i];
            }
            if (valid_voltage_count > 0) md.voltage_avg /= valid_voltage_count;
            md.current_avg /= ATM90E32AS_PHASE_COUNT;
            md.wiring_mode = (uint8_t)measurements.wiring_mode;
            md.voltage_l1 = measurements.voltage[ATM90E32AS_PHASE_A];
            md.voltage_l2 = measurements.voltage_valid[ATM90E32AS_PHASE_B]
                                ? measurements.voltage[ATM90E32AS_PHASE_B] : 0.0f;
            md.voltage_l3 = measurements.voltage[ATM90E32AS_PHASE_C];
            if (measurements.wiring_mode == ATM90E32AS_WIRING_3P3W) {
                md.voltage_uab = measurements.voltage[ATM90E32AS_PHASE_A];
                md.voltage_ucb = measurements.voltage[ATM90E32AS_PHASE_C];
            }
            md.current_l1 = measurements.current[ATM90E32AS_PHASE_A];
            md.current_l2 = measurements.current[ATM90E32AS_PHASE_B];
            md.current_l3 = measurements.current[ATM90E32AS_PHASE_C];
            md.current_neutral = measurements.current_neutral;
            md.frequency = measurements.frequency;
            md.p1 = measurements.active_power[ATM90E32AS_PHASE_A];
            md.p2 = measurements.active_power[ATM90E32AS_PHASE_B];
            md.p3 = measurements.active_power[ATM90E32AS_PHASE_C];
            md.p_total = measurements.total_active_power;
            md.q1 = measurements.reactive_power[ATM90E32AS_PHASE_A];
            md.q2 = measurements.reactive_power[ATM90E32AS_PHASE_B];
            md.q3 = measurements.reactive_power[ATM90E32AS_PHASE_C];
            md.q_total = measurements.total_reactive_power;
            md.s1 = measurements.apparent_power[ATM90E32AS_PHASE_A];
            md.s2 = measurements.apparent_power[ATM90E32AS_PHASE_B];
            md.s3 = measurements.apparent_power[ATM90E32AS_PHASE_C];
            md.s_total = measurements.total_apparent_power;
            md.pf1 = measurements.power_factor[ATM90E32AS_PHASE_A];
            md.pf2 = measurements.power_factor[ATM90E32AS_PHASE_B];
            md.pf3 = measurements.power_factor[ATM90E32AS_PHASE_C];
            md.pf_total = measurements.total_power_factor;
            md.energy_import = (float)(s_active_import_wh / 1000.0);
            md.energy_export = (float)(s_active_export_wh / 1000.0);
            md.energy_reactive_import = (float)(s_reactive_import_varh / 1000.0);
            md.energy_reactive_export = (float)(s_reactive_export_varh / 1000.0);
            md.temp_atm90 = measurements.temperature;
            md.last_update_us = (uint64_t)esp_timer_get_time();
            xSemaphoreGive(s_measurements_mutex);

            measurement_data_update(&md);

            /* First successful read after init or error recovery -> READY. */
            system_status_set(SYS_MODULE_ATM90, SYS_STATUS_READY);

            /* Alarm: the IC does the detection, this task owns the SPI link.
             * Threshold writes only happen when an apply is queued (or the
             * over-current anchor is still waiting for real load current);
             * the evaluation below is pure decode of sys_status0/1. */
            if (alarm_manager_ic_access_pending()) {
                config_manager_t *acfg = malloc(sizeof(*acfg));
                if (acfg != NULL && config_manager_get(acfg) == ESP_OK) {
                    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
                    alarm_manager_apply_ic(s_meter, &s_applied_calib, acfg, &measurements);
                    xSemaphoreGive(s_meter_mutex);
                }
                free(acfg);
            }
            alarm_manager_service(&measurements);

#if CONFIG_APP_ENERGY_METER_LOG_EACH_SAMPLE
            ESP_LOGI(TAG,
                     "VA=%.2fV IA=%.3fA VB=%.2fV IB=%.3fA VC=%.2fV IC=%.3fA P=%.2fW F=%.2fHz PF=%.3f ST0=0x%04X ST1=0x%04X",
                     measurements.voltage[ATM90E32AS_PHASE_A],
                     measurements.current[ATM90E32AS_PHASE_A],
                     measurements.voltage[ATM90E32AS_PHASE_B],
                     measurements.current[ATM90E32AS_PHASE_B],
                     measurements.voltage[ATM90E32AS_PHASE_C],
                     measurements.current[ATM90E32AS_PHASE_C],
                     measurements.total_active_power,
                     measurements.frequency,
                     measurements.total_power_factor,
                     measurements.sys_status0,
                     measurements.sys_status1);
#endif
        } else {
            ESP_LOGE(TAG, "read ATM90E32AS measurements failed: %s", esp_err_to_name(ret));
            system_status_set(SYS_MODULE_ATM90, SYS_STATUS_ERROR);
        }

        /* Mirror the SD card's real runtime state onto the status registry. The
         * sd_card component cannot include system_status.h (it would need to
         * depend on main, which already depends on sd_card), so the debounced
         * insert/mount result from its monitor task is sampled here on the
         * existing poll tick — no extra task or timer. system_status_set() is
         * idempotent, so repeating the same state costs nothing. */
        system_status_set(SYS_MODULE_SD_CARD,
                          sd_card_is_mounted() ? SYS_STATUS_READY
                          : (sd_card_is_inserted() ? SYS_STATUS_ERROR : SYS_STATUS_OFFLINE));

        /* Persist the counters (energy-triggered, with a slow backstop) and
         * refresh the epoch floor. Both no-op on most ticks. */
        energy_accum_service();
        time_source_service();

#if CONFIG_APP_ENERGY_SD_LOG_ENABLE
        if (ret == ESP_OK) {
            int64_t now_us = esp_timer_get_time();
            if ((now_us - sd_log_last_us) >=
                ((int64_t)CONFIG_APP_ENERGY_SD_LOG_PERIOD_S * 1000000LL)) {
                sd_log_last_us = now_us;

                /* Snapshot under the mutex, write outside it: a FAT append can
                 * take tens of ms and must not block the other consumers. */
                energy_meter_energy_t e;
                energy_meter_demand_t d;
                xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
                e.active_import_kwh = (float)(s_active_import_wh / 1000.0);
                e.active_export_kwh = (float)(s_active_export_wh / 1000.0);
                e.reactive_import_kvarh = (float)(s_reactive_import_varh / 1000.0);
                e.reactive_export_kvarh = (float)(s_reactive_export_varh / 1000.0);
                d.active_power_demand_w = s_demand_value_w;
                d.active_power_demand_max_w = s_demand_max_w;
                xSemaphoreGive(s_measurements_mutex);

                energy_meter_log_to_sd(&e, &d,
                                       measurements.total_active_power,
                                       measurements.total_power_factor,
                                       measurements.frequency);
            }
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_ENERGY_METER_POLL_PERIOD_MS));
    }
}

esp_err_t energy_meter_get_energy(energy_meter_energy_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    out->active_import_kwh = (float)(s_active_import_wh / 1000.0);
    out->active_export_kwh = (float)(s_active_export_wh / 1000.0);
    out->reactive_import_kvarh = (float)(s_reactive_import_varh / 1000.0);
    out->reactive_export_kvarh = (float)(s_reactive_export_varh / 1000.0);
    xSemaphoreGive(s_measurements_mutex);

    return ESP_OK;
}

esp_err_t energy_meter_get_demand(energy_meter_demand_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    out->active_power_demand_w = s_demand_value_w;
    out->active_power_demand_max_w = s_demand_max_w;
    xSemaphoreGive(s_measurements_mutex);

    return ESP_OK;
}

esp_err_t energy_meter_reset_energy(void)
{
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    s_active_import_wh = 0.0;
    s_active_export_wh = 0.0;
    s_reactive_import_varh = 0.0;
    s_reactive_export_varh = 0.0;
    xSemaphoreGive(s_measurements_mutex);

    /* Commit immediately: an operator who clears the meter and pulls the power
     * must not find the old reading back on the next boot. */
    s_persist_dirty = true;
    esp_err_t ret = energy_accum_save();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "energy reset not persisted: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "energy counters reset by user");
    return ESP_OK;
}

esp_err_t energy_meter_reset_demand(void)
{
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    s_demand_accum_ws = 0.0;
    s_demand_elapsed_s = 0.0;
    s_demand_last_us = 0;
    s_demand_value_w = 0.0f;
    s_demand_max_w = 0.0f;
    xSemaphoreGive(s_measurements_mutex);

    /* demand_max lives in the same blob as the energy counters. */
    s_persist_dirty = true;
    esp_err_t ret = energy_accum_save();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "demand reset not persisted: %s", esp_err_to_name(ret));
    }
    ESP_LOGI(TAG, "demand reset by user");
    return ESP_OK;
}

esp_err_t energy_meter_flush_persist(void)
{
    if (s_measurements_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    time_source_flush();
    return energy_accum_save();
}

esp_err_t energy_meter_set_demand_window_minutes(uint16_t minutes)
{
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");
    ESP_RETURN_ON_FALSE(minutes > 0, ESP_ERR_INVALID_ARG, TAG, "window must be > 0");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    s_demand_window_min = minutes;
    s_demand_accum_ws = 0.0;
    s_demand_elapsed_s = 0.0;
    s_demand_last_us = 0;
    xSemaphoreGive(s_measurements_mutex);

    s_persist_dirty = true;
    return ESP_OK;
}

esp_err_t energy_meter_get_demand_window_minutes(uint16_t *out_minutes)
{
    ESP_RETURN_ON_FALSE(out_minutes != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    *out_minutes = s_demand_window_min;
    xSemaphoreGive(s_measurements_mutex);

    return ESP_OK;
}

esp_err_t energy_meter_get_latest(atm90e32as_measurements_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_measurements_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "measurements mutex not initialized");

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    bool valid = s_measurements_valid;
    if (valid) {
        *out = s_latest_measurements;
    }
    xSemaphoreGive(s_measurements_mutex);

    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

bool energy_meter_has_latest(void)
{
    if (s_measurements_mutex == NULL) {
        return false;
    }

    xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
    bool valid = s_measurements_valid;
    xSemaphoreGive(s_measurements_mutex);

    return valid;
}

esp_err_t energy_meter_read_register(uint16_t reg, uint16_t *value)
{
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "value is NULL");
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    esp_err_t ret = atm90e32as_read_register(s_meter, reg, value);
    xSemaphoreGive(s_meter_mutex);

    return ret;
}

esp_err_t energy_meter_write_register(uint16_t reg, uint16_t value)
{
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    esp_err_t ret = atm90e32as_write_register(s_meter, reg, value);
    xSemaphoreGive(s_meter_mutex);

    return ret;
}

esp_err_t energy_meter_get_calibration(atm90e32as_calib_t *calib)
{
    ESP_RETURN_ON_FALSE(calib != NULL, ESP_ERR_INVALID_ARG, TAG, "calib is NULL");
    ESP_RETURN_ON_FALSE(s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    *calib = s_current_calib;
    xSemaphoreGive(s_meter_mutex);

    return ESP_OK;
}

/* Stamp chip-wide PGA + line frequency onto the active calibration. */
static void energy_meter_stamp_chipwide_locked(atm90e32as_pga_gain_t pga,
                                               atm90e32as_line_freq_t freq)
{
    s_calib.pga_gain = pga;
    s_calib.line_freq = freq;
    s_current_calib.pga_gain = pga;
    s_current_calib.line_freq = freq;
}

/* Keep config_manager.line_freq as a RO mirror for alarms/registers. Never the
 * other way around — config must not own the chip value. */
static void energy_meter_sync_line_freq_mirror(atm90e32as_line_freq_t freq)
{
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        return;
    }
    if (config_manager_get(cfg) == ESP_OK) {
        uint8_t want = (freq == ATM90E32AS_LINE_FREQ_60HZ) ? 1U : 0U;
        if (cfg->line_freq != want) {
            cfg->line_freq = want;
            cfg->alarm_nominal_frequency_hz = want ? 60U : 50U;
            cfg->alarm_frequency_low_hz = want ? 57.0f : 47.0f;
            cfg->alarm_frequency_high_hz = want ? 63.0f : 53.0f;
            (void)config_manager_update(cfg);
        }
    }
    free(cfg);
}

esp_err_t energy_meter_set_calibration(const atm90e32as_calib_t *calib)
{
    ESP_RETURN_ON_FALSE(calib != NULL, ESP_ERR_INVALID_ARG, TAG, "calib is NULL");
    ESP_RETURN_ON_FALSE(s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    /* PGA and line_freq are console/Kconfig-owned. Callers (gain set, portal
     * auto-cal, SD import path via set) cannot smuggle a new value here. */
    atm90e32as_pga_gain_t pga = s_current_calib.pga_gain;
    atm90e32as_line_freq_t freq = s_current_calib.line_freq;
    atm90e32as_calib_t next = *calib;
    next.pga_gain = pga;
    next.line_freq = freq;
    s_calib = next;
    s_current_calib = next;
    energy_meter_stamp_chipwide_locked(pga, freq);
    xSemaphoreGive(s_meter_mutex);

    return ESP_OK;
}

esp_err_t energy_meter_set_pga_gain(atm90e32as_pga_gain_t pga, bool apply)
{
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "meter not initialized");

    /* PGA is fixed at 4× per thesis requirement. Console path kept for legacy
     * script compatibility; 4× request is idempotent no-op, others rejected. */
    if (pga != ATM90E32AS_PGA_GAIN_4X) {
        ESP_LOGW(TAG, "PGA is fixed at 4×; requested %ux ignored", 1 << pga);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* PGA=4 request is idempotent no-op; config already has 4. */
    (void)apply;
    ESP_LOGI(TAG, "PGA already 4× (fixed)");
    return ESP_OK;
}

esp_err_t energy_meter_set_line_freq(atm90e32as_line_freq_t freq, bool apply)
{
    ESP_RETURN_ON_FALSE(freq == ATM90E32AS_LINE_FREQ_50HZ || freq == ATM90E32AS_LINE_FREQ_60HZ,
                        ESP_ERR_INVALID_ARG, TAG, "invalid line freq");
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    energy_meter_stamp_chipwide_locked(s_current_calib.pga_gain, freq);
    esp_err_t ret = ESP_OK;
    if (apply) {
        ret = energy_meter_apply_locked(&s_current_calib);
    }
    xSemaphoreGive(s_meter_mutex);
    if (ret == ESP_OK) {
        energy_meter_sync_line_freq_mirror(freq);
        ESP_LOGI(TAG, "line_freq set to %s%s",
                 freq == ATM90E32AS_LINE_FREQ_60HZ ? "60Hz" : "50Hz",
                 apply ? " (applied)" : "");
    }
    return ret;
}

static esp_err_t energy_meter_apply_locked(const atm90e32as_calib_t *target)
{
    ESP_RETURN_ON_ERROR(atm90e32as_validate_calibration(target), TAG, "invalid calibration");
    atm90e32as_calib_t previous = s_applied_calib;
    bool wiring_changed = previous.wiring_mode != target->wiring_mode;

    if (wiring_changed) {
        ESP_LOGI(TAG, "Wiring mode change: %s -> %s",
                 previous.wiring_mode == ATM90E32AS_WIRING_3P4W ? "3P4W" : "3P3W",
                 target->wiring_mode == ATM90E32AS_WIRING_3P4W ? "3P4W" : "3P3W");
        xSemaphoreTake(s_measurements_mutex, portMAX_DELAY);
        s_measurements_valid = false;
        xSemaphoreGive(s_measurements_mutex);
        energy_meter_set_wiring_relay(target->wiring_mode);
        vTaskDelay(pdMS_TO_TICKS(ENERGY_METER_RELAY_SETTLE_MS));
    }

    esp_err_t ret = atm90e32as_apply_calibration(s_meter, target);
    if (ret != ESP_OK && wiring_changed) {
        energy_meter_set_wiring_relay(previous.wiring_mode);
        vTaskDelay(pdMS_TO_TICKS(ENERGY_METER_RELAY_SETTLE_MS));
        esp_err_t rollback = atm90e32as_apply_calibration(s_meter, &previous);
        ESP_LOGE(TAG, "apply failed (%s), rollback=%s", esp_err_to_name(ret), esp_err_to_name(rollback));
        return ret;
    }
    if (ret == ESP_OK) {
        s_current_calib = *target;
        s_applied_calib = *target;
        s_calib = *target;
        /* Gains just moved, so every anchored IC threshold is scaled against a
         * stale gain. Re-anchor or the comparators silently judge against the
         * old calibration. */
        alarm_manager_request_apply();
        if (wiring_changed) vTaskDelay(pdMS_TO_TICKS(ENERGY_METER_MEASUREMENT_SETTLE_MS));
    }
    return ret;
}

esp_err_t energy_meter_set_wiring_mode(atm90e32as_wiring_mode_t mode, bool apply)
{
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    /* Phase gains are SHARED across wiring modes — only the chip mode bit and
     * the relay change. PGA / line_freq stay at their current values. */
    s_current_calib.wiring_mode = mode;
    s_calib.wiring_mode = mode;
    esp_err_t ret = ESP_OK;
    if (apply) {
        ret = energy_meter_apply_locked(&s_current_calib);
    }
    xSemaphoreGive(s_meter_mutex);

    return ret;
}

esp_err_t energy_meter_apply_calibration(void)
{
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_calib_t target = s_calib;
    target.pga_gain = s_current_calib.pga_gain;
    target.line_freq = s_current_calib.line_freq;
    target.wiring_mode = s_current_calib.wiring_mode;
    esp_err_t ret = energy_meter_apply_locked(&target);
    xSemaphoreGive(s_meter_mutex);

    return ret;
}

esp_err_t energy_meter_save_calibration(void)
{
    ESP_RETURN_ON_FALSE(s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_calib_t calib = s_current_calib;
    xSemaphoreGive(s_meter_mutex);

    return energy_meter_save_calibration_to_nvs(&calib);
}

esp_err_t energy_meter_load_calibration(bool apply)
{
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    atm90e32as_calib_t calib;
    ESP_RETURN_ON_ERROR(energy_meter_load_calibration_from_nvs(&calib), TAG, "load calibration from NVS failed");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    s_current_calib = calib;
    esp_err_t ret = ESP_OK;
    if (apply) {
        ret = energy_meter_apply_locked(&s_current_calib);
    }
    xSemaphoreGive(s_meter_mutex);

    return ret;
}

/* Factory-reset entry point: drop the persisted calibration blob so the next
 * boot falls back to the bring-up defaults in energy_meter_init(). Runtime
 * registers are left alone on purpose — the caller reboots after a reset. */
esp_err_t energy_meter_erase_calibration(void)
{
    ESP_RETURN_ON_ERROR(energy_meter_nvs_init(), TAG, "init NVS failed");

    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(ENERGY_METER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;  /* nothing persisted yet */
    }
    ESP_RETURN_ON_ERROR(ret, TAG, "open calibration NVS failed");

    ret = nvs_erase_all(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ret = ESP_OK;
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return ret;
}

esp_err_t energy_meter_export_blob(uint8_t *out, size_t out_cap, size_t *out_len)
{
    ESP_RETURN_ON_FALSE(out != NULL && out_len != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    energy_meter_calib_blob_t blob = {
        .magic = ENERGY_METER_CALIB_MAGIC,
    };

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    blob.calib = s_calib;
    xSemaphoreGive(s_meter_mutex);

    size_t blob_size = sizeof(blob);
    ESP_RETURN_ON_FALSE(out_cap >= blob_size, ESP_ERR_INVALID_SIZE, TAG, "output buffer too small");

    memcpy(out, &blob, blob_size);
    *out_len = blob_size;
    ESP_LOGI(TAG, "exported %zu-byte blob (active=%s)", blob_size,
             blob.calib.wiring_mode == ATM90E32AS_WIRING_3P3W ? "3P3W" : "3P4W");
    return ESP_OK;
}

esp_err_t energy_meter_import_blob(const uint8_t *in, size_t len, bool apply, bool save_nvs)
{
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    energy_meter_calib_blob_t blob;
    ESP_RETURN_ON_FALSE(len == sizeof(blob), ESP_ERR_INVALID_SIZE, TAG, "blob size mismatch");
    memcpy(&blob, in, sizeof(blob));

    /* Validate magic; legacy v2/v3 blobs (with version + profile[2]) fail this
     * size check above and fall back to defaults. */
    if (blob.magic != ENERGY_METER_CALIB_MAGIC) {
        ESP_LOGE(TAG, "import failed: bad magic 0x%08lx", (unsigned long)blob.magic);
        return ESP_ERR_INVALID_VERSION;
    }
    esp_err_t valid = atm90e32as_validate_calibration(&blob.calib);
    if (valid != ESP_OK) {
        ESP_LOGE(TAG, "import failed: validation error 0x%x", valid);
        return valid;
    }

    /* Install to RAM. PGA/line_freq stay at the running console/Kconfig values —
     * backup blobs must not override chip-wide parameters. */
    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_pga_gain_t keep_pga = s_current_calib.pga_gain;
    atm90e32as_line_freq_t keep_freq = s_current_calib.line_freq;
    s_calib = blob.calib;
    s_current_calib = s_calib;
    /* Preserve the running chip-wide stamps; only wiring_mode + phase cal change. */
    s_current_calib.pga_gain = keep_pga;
    s_current_calib.line_freq = keep_freq;
    energy_meter_stamp_chipwide_locked(keep_pga, keep_freq);
    xSemaphoreGive(s_meter_mutex);

    ESP_LOGI(TAG, "imported blob: active=%s (PGA/freq preserved)",
             s_current_calib.wiring_mode == ATM90E32AS_WIRING_3P3W ? "3P3W" : "3P4W");

    /* Apply to hardware if requested */
    if (apply) {
        esp_err_t ret = energy_meter_apply_calibration();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "import apply failed: 0x%x", ret);
            return ret;
        }
    }

    /* Save to NVS if requested - must save the full blob */
    if (save_nvs) {
        esp_err_t ret = energy_meter_nvs_init();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "import NVS init failed: 0x%x", ret);
            return ret;
        }

        nvs_handle_t nvs;
        ret = nvs_open(ENERGY_METER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "import NVS open failed: 0x%x", ret);
            return ret;
        }

        energy_meter_calib_blob_t save_blob = {
            .magic = ENERGY_METER_CALIB_MAGIC,
            .calib = s_calib,
        };
        xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
        save_blob.calib = s_calib;
        xSemaphoreGive(s_meter_mutex);

        ret = nvs_set_blob(nvs, ENERGY_METER_NVS_CALIB_KEY, &save_blob, sizeof(save_blob));
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);

        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "import NVS write failed: 0x%x", ret);
            return ret;
        }
    }

    return ESP_OK;
}

/* Calibration backup file format wrapper
 *
 * Single-profile format (no file_version field):
 *   Ugain, Igain, Uoffset, Ioffset, phase_comp, Poffset, Qoffset,
 *   pq_gain, fundamental_power_gain  × 3 phases in payload.
 * Header carries wiring_mode as metadata tag for filename / display only.
 * PGA / line_freq / CT / refs are intentionally NOT in the bin (system-owned).
 *
 * Legacy (handled for read-back compatibility):
 *   v1/v2 files have a file_version field that shifts the header layout.
 *   We detect them by payload size and hand off to the legacy blob import. */
#define CALIB_FILE_MAGIC 0x424C4143U  /* 'CALB' */
#define CALIB_FILE_HEADER_LEN 18

typedef struct __attribute__((packed)) {
    uint32_t file_magic;
    uint16_t header_len;
    uint16_t wiring_mode;   /* 0=3P4W, 1=3P3W — metadata tag only */
    uint16_t reserved;
    uint32_t payload_len;
    uint32_t crc32;
} calib_file_header_t;

/* Packed phase cal fields only — no floats, no PGA/freq. */
typedef struct __attribute__((packed)) {
    uint16_t voltage_gain;
    uint16_t current_gain;
    int16_t voltage_offset;
    int16_t current_offset;
    int16_t active_power_offset;
    int16_t reactive_power_offset;
    uint16_t pq_gain;
    int16_t phase_comp;
    uint16_t fundamental_power_gain;
} calib_phase_pack_t;

typedef struct __attribute__((packed)) {
    calib_phase_pack_t phase[ATM90E32AS_PHASE_COUNT];
} calib_format_c_payload_t;

static void calib_phase_to_pack(calib_phase_pack_t *dst, const atm90e32as_phase_calib_t *src)
{
    dst->voltage_gain = src->voltage_gain;
    dst->current_gain = src->current_gain;
    dst->voltage_offset = src->voltage_offset;
    dst->current_offset = src->current_offset;
    dst->active_power_offset = src->active_power_offset;
    dst->reactive_power_offset = src->reactive_power_offset;
    dst->pq_gain = src->pq_gain;
    dst->phase_comp = src->phase_comp;
    dst->fundamental_power_gain = src->fundamental_power_gain;
}

static void calib_phase_from_pack(atm90e32as_phase_calib_t *dst, const calib_phase_pack_t *src)
{
    dst->voltage_gain = src->voltage_gain;
    dst->current_gain = src->current_gain;
    dst->voltage_offset = src->voltage_offset;
    dst->current_offset = src->current_offset;
    dst->active_power_offset = src->active_power_offset;
    dst->reactive_power_offset = src->reactive_power_offset;
    dst->pq_gain = src->pq_gain;
    dst->phase_comp = src->phase_comp;
    dst->fundamental_power_gain = src->fundamental_power_gain;
    dst->_abi_pad = 0;
    dst->_abi_reserved[0] = 0;
    dst->_abi_reserved[1] = 0;
}

static esp_err_t calib_backup_install_phase_cal(const atm90e32as_calib_t *src_phases,
                                                atm90e32as_wiring_mode_t file_mode,
                                                bool apply, bool save_nvs)
{
    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_wiring_mode_t current_mode = s_current_calib.wiring_mode;
    const char *current_mode_str = (current_mode == ATM90E32AS_WIRING_3P3W) ? "3W" : "4W";
    const char *file_mode_str = (file_mode == ATM90E32AS_WIRING_3P3W) ? "3W" : "4W";
    /* PGA/freq are system-owned (config / runtime), never taken from the file. */
    atm90e32as_pga_gain_t keep_pga = s_current_calib.pga_gain;
    atm90e32as_line_freq_t keep_freq = s_current_calib.line_freq;

    atm90e32as_calib_t calib = *src_phases;
    calib.pga_gain = keep_pga;
    calib.line_freq = keep_freq;

    esp_err_t apply_ret = ESP_OK;
    if (apply) {
        /* Single-profile mode: phase gains apply to the common profile. */
        calib.wiring_mode = current_mode;
        s_calib = calib;
        s_current_calib = calib;
        energy_meter_stamp_chipwide_locked(keep_pga, keep_freq);
        apply_ret = energy_meter_apply_locked(&s_current_calib);
        ESP_LOGI(TAG, "applied imported %s cal to current %s (PGA/freq/CT kept)",
                 file_mode_str, current_mode_str);
    } else {
        /* Save only — keep current wiring, just stash the new phase gains
         * into the single profile. */
        calib.wiring_mode = file_mode;
        s_calib = calib;
        energy_meter_stamp_chipwide_locked(keep_pga, keep_freq);
        ESP_LOGI(TAG, "imported %s cal (not applied, PGA/freq/CT kept)",
                 file_mode_str);
    }
    xSemaphoreGive(s_meter_mutex);

    if (apply_ret != ESP_OK) {
        return apply_ret;
    }

    if (save_nvs) {
        esp_err_t ret = energy_meter_nvs_init();
        if (ret != ESP_OK) {
            return ret;
        }
        nvs_handle_t nvs;
        ret = nvs_open(ENERGY_METER_NVS_NAMESPACE, NVS_READWRITE, &nvs);
        if (ret != ESP_OK) {
            return ret;
        }
        energy_meter_calib_blob_t save_blob = {
            .magic = ENERGY_METER_CALIB_MAGIC,
            .calib = s_calib,
        };
        ret = nvs_set_blob(nvs, ENERGY_METER_NVS_CALIB_KEY, &save_blob, sizeof(save_blob));
        if (ret == ESP_OK) {
            ret = nvs_commit(nvs);
        }
        nvs_close(nvs);
        return ret;
    }
    return ESP_OK;
}

esp_err_t calib_backup_pack(uint8_t *file_out, size_t cap, size_t *file_len)
{
    ESP_RETURN_ON_FALSE(file_out != NULL && file_len != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_calib_t calib = s_current_calib;
    xSemaphoreGive(s_meter_mutex);

    calib_format_c_payload_t payload;
    memset(&payload, 0, sizeof(payload));
    for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
        calib_phase_to_pack(&payload.phase[i], &calib.phase[i]);
    }

    size_t payload_len = sizeof(payload);
    size_t total = CALIB_FILE_HEADER_LEN + payload_len;
    ESP_RETURN_ON_FALSE(cap >= total, ESP_ERR_INVALID_SIZE, TAG, "output buffer too small");

    uint32_t crc = esp_crc32_le(0, (const uint8_t *)&payload, payload_len);
    calib_file_header_t header = {
        .file_magic = CALIB_FILE_MAGIC,
        .header_len = CALIB_FILE_HEADER_LEN,
        .wiring_mode = (uint16_t)calib.wiring_mode,
        .reserved = 0,
        .payload_len = (uint32_t)payload_len,
        .crc32 = crc,
    };

    memcpy(file_out, &header, sizeof(header));
    memcpy(file_out + CALIB_FILE_HEADER_LEN, &payload, payload_len);
    *file_len = total;

    const char *mode_str = (calib.wiring_mode == ATM90E32AS_WIRING_3P3W) ? "3W" : "4W";
    ESP_LOGI(TAG, "packed calibration file: %zu bytes (%s, crc=0x%08lx)",
             total, mode_str, (unsigned long)crc);
    return ESP_OK;
}

esp_err_t calib_backup_pack_single(uint8_t *file_out, size_t cap, size_t *file_len)
{
    return calib_backup_pack(file_out, cap, file_len);
}

esp_err_t calib_backup_unpack(const uint8_t *file_in, size_t file_len, bool apply, bool save_nvs)
{
    ESP_RETURN_ON_FALSE(file_in != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(file_len >= CALIB_FILE_HEADER_LEN, ESP_ERR_INVALID_SIZE, TAG, "file too small");
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    /* The header is the same packed struct regardless of legacy vs new layout
     * (the legacy file_version field was removed in the new format and is no
     * longer present). The header_len byte tells us how much to skip to reach
     * the payload. Detect legacy v1 (full dual-profile blob) and v2 (full
     * atm90e32as_calib_t) by payload size — both are still accepted for
     * read-back, but we always write the new single-profile format. */
    uint32_t file_magic;
    memcpy(&file_magic, file_in, sizeof(file_magic));
    if (file_magic != CALIB_FILE_MAGIC) {
        ESP_LOGE(TAG, "unpack failed: bad file magic 0x%08lx", (unsigned long)file_magic);
        return ESP_ERR_INVALID_VERSION;
    }

    uint16_t header_len;
    memcpy(&header_len, file_in + sizeof(uint32_t), sizeof(header_len));

    /* Reject legacy v1/v2 headers (they include a file_version field at offset
     * 6 = sizeof(magic)+sizeof(version_field); we detect by checking the
     * version_field value is 1..3 OR the layout doesn't match the new header
     * length). New format: header_len = 18. Legacy: 20. */
    uint16_t file_version_legacy = 0;
    if (header_len == 20) {
        memcpy(&file_version_legacy, file_in + sizeof(uint32_t) + sizeof(uint16_t), sizeof(uint16_t));
    }

    if (header_len != CALIB_FILE_HEADER_LEN && header_len != 20) {
        ESP_LOGE(TAG, "unpack failed: unexpected header_len %u", header_len);
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t wiring_mode_raw;
    uint32_t payload_len_raw;
    uint32_t crc32_stored;
    if (header_len == CALIB_FILE_HEADER_LEN) {
        /* New layout: magic(4) header_len(2) wiring_mode(2) reserved(2) payload_len(4) crc32(4) = 18 */
        memcpy(&wiring_mode_raw,  file_in + 6,  sizeof(uint16_t));
        memcpy(&payload_len_raw,  file_in + 10, sizeof(uint32_t));
        memcpy(&crc32_stored,     file_in + 14, sizeof(uint32_t));
    } else {
        /* Legacy layout: magic(4) version(2) header_len(2) wiring_mode(2) reserved(2) payload_len(4) crc32(4) = 20 */
        memcpy(&wiring_mode_raw,  file_in + 8,  sizeof(uint16_t));
        memcpy(&payload_len_raw,  file_in + 12, sizeof(uint32_t));
        memcpy(&crc32_stored,     file_in + 16, sizeof(uint32_t));
        if (file_version_legacy < 1 || file_version_legacy > 3) {
            ESP_LOGE(TAG, "unpack failed: unsupported legacy file version %u", file_version_legacy);
            return ESP_ERR_INVALID_VERSION;
        }
    }

    size_t expected_total = header_len + payload_len_raw;
    if (file_len != expected_total) {
        ESP_LOGE(TAG, "unpack failed: file_len=%zu, expected=%zu", file_len, expected_total);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *payload = file_in + header_len;
    uint32_t computed_crc = esp_crc32_le(0, payload, payload_len_raw);
    if (computed_crc != crc32_stored) {
        ESP_LOGE(TAG, "unpack failed: CRC mismatch (computed=0x%08lx, expected=0x%08lx)",
                 (unsigned long)computed_crc, (unsigned long)crc32_stored);
        return ESP_ERR_INVALID_CRC;
    }

    atm90e32as_wiring_mode_t file_mode = (atm90e32as_wiring_mode_t)wiring_mode_raw;
    if (file_mode != ATM90E32AS_WIRING_3P4W && file_mode != ATM90E32AS_WIRING_3P3W) {
        /* v1 used 0xFFFF for full blob — fall through by payload size. */
        file_mode = ATM90E32AS_WIRING_3P4W;
    }

    /* Legacy v1: full dual-profile blob payload */
    if (header_len == 20 && (file_version_legacy == 1 ||
                             payload_len_raw == sizeof(energy_meter_calib_blob_t))) {
        ESP_LOGI(TAG, "unpacking legacy v1 full blob: %zu bytes", file_len);
        return energy_meter_import_blob(payload, payload_len_raw, apply, save_nvs);
    }

    atm90e32as_calib_t calib;
    memset(&calib, 0, sizeof(calib));
    calib.wiring_mode = file_mode;

    /* New format (or legacy v3) — single-profile phase-cal payload. */
    if (payload_len_raw == sizeof(calib_format_c_payload_t)) {
        if (payload_len_raw != sizeof(calib_format_c_payload_t)) {
            ESP_LOGE(TAG, "unpack failed: payload_len=%lu expected=%zu",
                     (unsigned long)payload_len_raw, sizeof(calib_format_c_payload_t));
            return ESP_ERR_INVALID_SIZE;
        }
        calib_format_c_payload_t cpay;
        memcpy(&cpay, payload, sizeof(cpay));
        for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
            if (cpay.phase[i].voltage_gain == 0 || cpay.phase[i].current_gain == 0) {
                ESP_LOGE(TAG, "unpack: phase %d has zero gain", i);
                return ESP_ERR_INVALID_ARG;
            }
            calib_phase_from_pack(&calib.phase[i], &cpay.phase[i]);
        }
        ESP_LOGI(TAG, "unpacking phase-cal payload: %zu bytes", file_len);
        return calib_backup_install_phase_cal(&calib, file_mode, apply, save_nvs);
    }

    /* Legacy v2: full atm90e32as_calib_t — take phase fields only; drop PGA/freq. */
    if (payload_len_raw != sizeof(atm90e32as_calib_t)) {
        ESP_LOGE(TAG, "unpack failed: payload_len=%lu expected=%zu",
                 (unsigned long)payload_len_raw, sizeof(atm90e32as_calib_t));
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(&calib, payload, sizeof(calib));
    if (calib.wiring_mode != file_mode &&
        (file_mode == ATM90E32AS_WIRING_3P4W || file_mode == ATM90E32AS_WIRING_3P3W)) {
        calib.wiring_mode = file_mode;
    }
    /* Validate gains only; PGA/freq in file are ignored by install. */
    for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
        if (calib.phase[i].voltage_gain == 0 || calib.phase[i].current_gain == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    ESP_LOGI(TAG, "unpacking legacy v2 full calib (phase fields only): %zu bytes", file_len);
    return calib_backup_install_phase_cal(&calib, file_mode, apply, save_nvs);
}

esp_err_t calib_backup_json(char *json_out, size_t cap, size_t *json_len)
{
    ESP_RETURN_ON_FALSE(json_out != NULL && json_len != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_calib_t calib = s_current_calib;
    xSemaphoreGive(s_meter_mutex);

    /* CSV companion is display-only: environment/config context for the bin.
     * PGA / CT / freq come from config_manager (system), phase rows from calib. */
    const char *mode_str = (calib.wiring_mode == ATM90E32AS_WIRING_3P4W) ? "3P4W" : "3P3W";
    const char *freq_str = (calib.line_freq == ATM90E32AS_LINE_FREQ_60HZ) ? "60Hz" : "50Hz";
    unsigned pga_x = energy_meter_pga_mult(calib.pga_gain);

    uint16_t nct = (uint16_t)CONFIG_APP_ATM90E32AS_CT_RATIO;
    uint16_t i_rated = (uint16_t)CONFIG_APP_ATM90E32AS_I_RATED_A;
    uint16_t i_exp = (uint16_t)CONFIG_APP_ATM90E32AS_I_EXPECTED_A;
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg != NULL && config_manager_get(cfg) == ESP_OK) {
        if (cfg->ct_ratio >= 1U) {
            nct = cfg->ct_ratio;
        }
        if (cfg->i_rated_a >= 1U) {
            i_rated = cfg->i_rated_a;
        }
        if (cfg->i_expected_a >= 1U) {
            i_exp = cfg->i_expected_a;
        }
        if (cfg->pga == 1U || cfg->pga == 2U || cfg->pga == 4U) {
            pga_x = cfg->pga;
        }
        if (cfg->line_freq == 1U) {
            freq_str = "60Hz";
        } else if (cfg->line_freq == 0U) {
            freq_str = "50Hz";
        }
        if (cfg->wiring_mode == 1U) {
            mode_str = "3P3W";
        } else if (cfg->wiring_mode == 0U) {
            mode_str = "3P4W";
        }
    }
    free(cfg);

    /* CSV companion (display-only): system PGA/freq/CT + phase cal snapshot. */
    int len = snprintf(json_out, cap,
        "# Calib meta (bin is authoritative for phase gains)\n"
        "# wiring=%s freq=%s PGA=%ux Rburden=%.2fOhm\n"
        "# CT NCT=%u I_Rated=%uA I_Expected=%uA VADC=720mV\n"
        "# Phase,Vg,Ig,Vo,Io,Po,Qo,PQ,Ph,Fg\n"
        "A,%u,%u,%d,%d,%d,%d,%u,%d,%u\n"
        "B,%u,%u,%d,%d,%d,%d,%u,%d,%u\n"
        "C,%u,%u,%d,%d,%d,%d,%u,%d,%u\n",
        mode_str, freq_str, pga_x, (double)ENERGY_METER_R_BURDEN_OHM,
        (unsigned)nct, (unsigned)i_rated, (unsigned)i_exp,
        calib.phase[0].voltage_gain, calib.phase[0].current_gain,
        calib.phase[0].voltage_offset, calib.phase[0].current_offset,
        calib.phase[0].active_power_offset, calib.phase[0].reactive_power_offset,
        calib.phase[0].pq_gain, calib.phase[0].phase_comp, calib.phase[0].fundamental_power_gain,
        calib.phase[1].voltage_gain, calib.phase[1].current_gain,
        calib.phase[1].voltage_offset, calib.phase[1].current_offset,
        calib.phase[1].active_power_offset, calib.phase[1].reactive_power_offset,
        calib.phase[1].pq_gain, calib.phase[1].phase_comp, calib.phase[1].fundamental_power_gain,
        calib.phase[2].voltage_gain, calib.phase[2].current_gain,
        calib.phase[2].voltage_offset, calib.phase[2].current_offset,
        calib.phase[2].active_power_offset, calib.phase[2].reactive_power_offset,
        calib.phase[2].pq_gain, calib.phase[2].phase_comp, calib.phase[2].fundamental_power_gain
    );

    if (len < 0 || (size_t)len >= cap) {
        ESP_LOGE(TAG, "CSV buffer too small (need ~%d, have %zu)", len, cap);
        return ESP_ERR_NO_MEM;
    }

    *json_len = (size_t)len;
    return ESP_OK;
}

esp_err_t energy_meter_reset_calibration_defaults(bool apply)
{
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    /* Factory phase gains only. Keep system PGA from config (do not recompute
     * CT→PGA here — that is CT Apply's job). If config PGA unset, fall back
     * to Kconfig enum default. */
    atm90e32as_pga_gain_t pga = energy_meter_kconfig_default_pga();
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg != NULL && config_manager_get(cfg) == ESP_OK &&
        (cfg->pga == 1U || cfg->pga == 2U || cfg->pga == 4U)) {
        pga = energy_meter_pga_from_config_u8(cfg->pga);
    }
    free(cfg);
    atm90e32as_line_freq_t freq = energy_meter_kconfig_default_line_freq();

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    /* Keep active wiring; factory U/I = 0x8000 via atm90e32as_get_default_calib. */
    atm90e32as_wiring_mode_t wiring_mode = s_current_calib.wiring_mode;
    atm90e32as_get_default_calib(&s_current_calib);
    s_current_calib.wiring_mode = wiring_mode;
    s_current_calib.pga_gain = pga;
    s_current_calib.line_freq = freq;
    s_calib = s_current_calib;
    energy_meter_stamp_chipwide_locked(pga, freq);
    esp_err_t ret = ESP_OK;
    if (apply) {
        ret = energy_meter_apply_locked(&s_current_calib);
    }
    xSemaphoreGive(s_meter_mutex);
    if (ret == ESP_OK) {
        energy_meter_sync_line_freq_mirror(freq);
    }

    return ret;
}

/* Resolve the calibration reference for every selected phase.
 *   MANUAL   : one shared scalar — a single source wired to all channels.
 *   EXTERNAL : ONE Modbus poll, then the per-phase value from the reference meter. */
static esp_err_t energy_meter_multi_reference_value(const energy_meter_multi_calib_request_t *request,
                                                    uint8_t mask, float *ref)
{
    if (request->source == ENERGY_METER_CALIB_REFERENCE_MANUAL) {
        ESP_RETURN_ON_FALSE(isfinite(request->manual_reference) && request->manual_reference > 0.0f,
                            ESP_ERR_INVALID_ARG, TAG, "invalid manual reference");
        for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
            if (mask & (1u << p)) ref[p] = request->manual_reference;
        }
        return ESP_OK;
    }

    modbus_master_status_t status;
    meter_readings_t readings;
    ESP_RETURN_ON_ERROR(modbus_master_get_status(&status), TAG, "external meter status unavailable");
    ESP_RETURN_ON_FALSE(status.enabled && status.online_count > 0,
                        ESP_ERR_INVALID_STATE, TAG, "external meter is not online");
    ESP_RETURN_ON_ERROR(modbus_master_get_readings(&readings), TAG, "external reading unavailable");
    for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
        if (!(mask & (1u << p))) continue;
        float value = request->current ? readings.current[p] : readings.voltage[p];
        ESP_RETURN_ON_FALSE(isfinite(value) && value > 0.0f, ESP_ERR_INVALID_RESPONSE, TAG,
                            "external reference invalid for phase %d", p);
        ref[p] = value;
    }
    ESP_LOGI(TAG, "calibration reference from %s", modbus_meters_device_name(status.device));
    return ESP_OK;
}

/* Average one measurement field for every selected phase. Each sample reads the
 * chip ONCE and accumulates all masked phases from that same snapshot, so the
 * phases stay time-aligned (the whole point of calibrating them together).
 * noload=false rejects non-positive readings (gain path with a live reference);
 * noload=true accepts zero, since trending to zero is the goal of offset calib. */
static esp_err_t energy_meter_average_measurement_masked(const energy_meter_multi_calib_request_t *request,
                                                         uint8_t mask, bool noload, float *average)
{
    uint16_t samples = request->samples ? request->samples : 20;
    double sum[ATM90E32AS_PHASE_COUNT] = {0.0, 0.0, 0.0};
    for (uint16_t n = 0; n < samples; n++) {
        atm90e32as_measurements_t measurement;
        ESP_RETURN_ON_ERROR(atm90e32as_read_measurements(s_meter, &measurement), TAG,
                            "calibration sample failed");
        for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
            if (!(mask & (1u << p))) continue;
            if (!request->current && !measurement.voltage_valid[p]) {
                ESP_LOGE(TAG, "voltage not valid on phase %d during calibration", p);
                return ESP_ERR_NOT_SUPPORTED;
            }
            float value = request->current ? measurement.current[p] : measurement.voltage[p];
            if (!isfinite(value) || (!noload && value <= 0.0f)) {
                ESP_LOGE(TAG, "invalid %s sample on phase %d (value=%.6f)",
                         noload ? "offset" : "gain", p, value);
                return ESP_ERR_INVALID_RESPONSE;
            }
            sum[p] += value;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
        if (mask & (1u << p)) average[p] = (float)(sum[p] / samples);
    }
    return ESP_OK;
}

/* Multi-phase U/I gain and offset calibration: one DSP snapshot for every
 * selected phase, one chip write.
 *
 * Rollback rule — the calibration is reverted ONLY when a value could not be
 * computed or could not be applied. A missed tolerance, or a verify sample that
 * cannot be read back, is reported and the written value is KEPT: the computed
 * correction itself was sound, so discarding it would only hide a good result.
 * In those cases the function returns the verify error with rolled_back=false
 * and calibrated_mask still set, so the caller can tell the two apart. */
esp_err_t energy_meter_auto_calibrate_multi(const energy_meter_multi_calib_request_t *request,
                                            energy_meter_multi_calib_result_t *result)
{
    ESP_RETURN_ON_FALSE(request != NULL && result != NULL,
                        ESP_ERR_INVALID_ARG, TAG, "invalid multi calibration request");
    ESP_RETURN_ON_FALSE(request->phase_mask != 0 &&
                        (request->phase_mask & ~(uint8_t)ENERGY_METER_PHASE_MASK_ALL) == 0,
                        ESP_ERR_INVALID_ARG, TAG, "phase_mask must select 1..3 phases");
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    memset(result, 0, sizeof(*result));

    const uint32_t settle_ms = request->settle_ms ? request->settle_ms : ENERGY_METER_MEASUREMENT_SETTLE_MS;
    const float tolerance_percent = request->tolerance_percent > 0.0f ? request->tolerance_percent : 0.2f;
    const uint8_t mask = request->phase_mask;
    const char *kind = request->current ? "I" : "U";

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_calib_t original = s_applied_calib;
    atm90e32as_calib_t working = original;
    result->pga = original.pga_gain;   /* set before any early return */

    /* Calibration needs a neutral, so block it entirely in 3P3W (both U and I).
     * Phase gains are shared across wiring modes, so a 3P4W calibration is
     * already correct for 3P3W — there is nothing to gain by calibrating there. */
    if (original.wiring_mode == ATM90E32AS_WIRING_3P3W) {
        xSemaphoreGive(s_meter_mutex);
        ESP_LOGE(TAG, "%s calib rejected: wiring mode is 3P3W; switch to 3P4W "
                      "(phase gains are shared across wiring modes)", kind);
        return ESP_ERR_NOT_SUPPORTED;
    }

    for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
        if (!(mask & (1u << p))) continue;
        result->phase[p].done = true;
        result->phase[p].old_gain = request->current ? original.phase[p].current_gain
                                                     : original.phase[p].voltage_gain;
        result->phase[p].old_offset = request->current ? original.phase[p].current_offset
                                                       : original.phase[p].voltage_offset;
    }

    /* Reference is needed only by the gain path (offset measures no-load with
     * no reference), so resolve it lazily inside that branch. */
    float ref[ATM90E32AS_PHASE_COUNT] = {0.0f, 0.0f, 0.0f};
    float before[ATM90E32AS_PHASE_COUNT] = {0.0f, 0.0f, 0.0f};
    esp_err_t ret = ESP_OK;
    bool applied = false;   /* true once working has been written to the chip */

    if (request->calibrate_offset) {
        /* Offset calibration: zero every selected offset register first so the
         * residual we read is the true bias, then store its negation. The
         * reported reading is scaled (URMS/100, IRMS/1000); undo that scale to
         * land back in the register's raw LSB domain. */
        for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
            if (!(mask & (1u << p))) continue;
            if (request->current) working.phase[p].current_offset = 0;
            else working.phase[p].voltage_offset = 0;
        }
        applied = true;   /* about to write zeroed offsets; roll back if it fails */
        ret = energy_meter_apply_locked(&working);
        if (ret == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(settle_ms));
            ret = energy_meter_average_measurement_masked(request, mask, true, before);
        }
        if (ret == ESP_OK) {
            /* Range-check every phase BEFORE writing any of them: all-or-nothing. */
            const double scale = request->current ? 1000.0 : 100.0;
            int16_t comp[ATM90E32AS_PHASE_COUNT] = {0, 0, 0};
            for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
                if (!(mask & (1u << p))) continue;
                int64_t raw = llround((double)before[p] * scale);
                if (-raw < INT16_MIN || -raw > INT16_MAX) {
                    ESP_LOGE(TAG, "offset-cal phase=%d out of int16 range (residual=%.6f raw=%lld)",
                             p, before[p], (long long)raw);
                    ret = ESP_ERR_INVALID_SIZE;
                    break;
                }
                comp[p] = (int16_t)(-raw);
            }
            if (ret == ESP_OK) {
                for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
                    if (!(mask & (1u << p))) continue;
                    if (request->current) working.phase[p].current_offset = comp[p];
                    else working.phase[p].voltage_offset = comp[p];
                    result->phase[p].new_offset = comp[p];
                    ESP_LOGI(TAG, "offset-cal phase=%d %s residual=%.6f offset=%d",
                             p, kind, before[p], comp[p]);
                }
                ret = energy_meter_apply_locked(&working);
            }
        }
    } else {
        /* Gain calibration: new_gain = round(old_gain * reference / measured),
         * the datasheet ratio, evaluated in integers inside the driver.
         * Resolve the reference first (MANUAL or EXTERNAL per masked phase). */
        ret = energy_meter_multi_reference_value(request, mask, ref);
        if (ret == ESP_OK) {
            for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
                if (mask & (1u << p)) result->phase[p].reference = ref[p];
            }
        }
        if (ret == ESP_OK) {
            if (request->settle_ms) vTaskDelay(pdMS_TO_TICKS(settle_ms));
            ret = energy_meter_average_measurement_masked(request, mask, false, before);
        }
        if (ret == ESP_OK) {
            for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
                if (!(mask & (1u << p))) continue;
                ESP_LOGI(TAG, "%s calib inputs phase=%d ref=%.6f measured=%.6f old_gain=%u pga=x%d",
                         kind, p, ref[p], before[p], result->phase[p].old_gain,
                         1 << original.pga_gain);
            }
        }
        /* Compute every gain before writing any of them, so a phase that cannot
         * be calibrated leaves the whole set untouched. */
        uint16_t calculated[ATM90E32AS_PHASE_COUNT] = {0, 0, 0};
        for (int p = 0; p < ATM90E32AS_PHASE_COUNT && ret == ESP_OK; p++) {
            if (!(mask & (1u << p))) continue;
            esp_err_t gain_ret = atm90e32as_calculate_gain(result->phase[p].old_gain, ref[p],
                                                           before[p], &calculated[p]);
            if (gain_ret != ESP_OK) {
                ESP_LOGW(TAG, "%s gain phase=%d failed (%s) ref=%.6f meas=%.6f pga=x%d%s",
                         kind, p, esp_err_to_name(gain_ret), ref[p], before[p],
                         1 << original.pga_gain,
                         gain_ret == ESP_ERR_INVALID_SIZE && request->current
                             ? "; set PGA via console (meter-cal set --field pga) then retry"
                             : "; check divider / wiring / that this phase has a load");
                ret = gain_ret;
                break;
            }
        }
        if (ret == ESP_OK) {
            for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
                if (!(mask & (1u << p))) continue;
                if (request->current) working.phase[p].current_gain = calculated[p];
                else working.phase[p].voltage_gain = calculated[p];
                result->phase[p].new_gain = calculated[p];
            }
            /* Never let a stale working copy change PGA/freq. */
            working.pga_gain = original.pga_gain;
            working.line_freq = original.line_freq;
            applied = true;   /* chip write is about to happen (roll back on failure) */
            ret = energy_meter_apply_locked(&working);
        }
    }

    for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
        if (mask & (1u << p)) result->phase[p].measured_before = before[p];
    }

    /* Step: verify. Re-read purely to report the residual; a problem here never
     * reverts a calibration that was already written successfully. */
    bool committed = (ret == ESP_OK);
    if (committed) {
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
        float after[ATM90E32AS_PHASE_COUNT] = {0.0f, 0.0f, 0.0f};
        esp_err_t verify_ret = energy_meter_average_measurement_masked(request, mask,
                                                                      request->calibrate_offset,
                                                                      after);
        if (verify_ret != ESP_OK) {
            ESP_LOGW(TAG, "%s calib written but verify read failed (%s) — NOT rolled back",
                     kind, esp_err_to_name(verify_ret));
            ret = verify_ret;
        } else {
            for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
                if (!(mask & (1u << p))) continue;
                result->phase[p].measured_after = after[p];
                if (!request->calibrate_offset && ref[p] > 0.0f) {
                    float error_percent = fabsf(after[p] - ref[p]) * 100.0f / ref[p];
                    result->phase[p].error_percent = error_percent;
                    if (error_percent > tolerance_percent) {
                        ESP_LOGW(TAG, "%s calib phase=%d residual %.3f%% exceeds tolerance %.3f%% "
                                      "(ref=%.6f after=%.6f) — kept, re-run to refine",
                                 kind, p, error_percent, tolerance_percent, ref[p], after[p]);
                    } else {
                        ESP_LOGI(TAG, "%s calib verified phase=%d after=%.6f error=%.3f%%",
                                 kind, p, after[p], error_percent);
                    }
                } else {
                    ESP_LOGI(TAG, "%s calib verified phase=%d residual_after=%.6f",
                             kind, p, after[p]);
                }
            }
        }
        /* The values are on the chip regardless of how the verify read went. */
        result->calibrated_mask = mask;
        result->offset_calibrated = request->calibrate_offset;
    }

    if (!committed) {
        /* Only restore the chip if a write actually happened. A pre-apply
         * failure (reference/capture/compute) left the chip untouched, so
         * re-writing `original` would be a pointless full image write. */
        if (applied) {
            esp_err_t rollback = energy_meter_apply_locked(&original);
            result->rolled_back = true;
            ESP_LOGE(TAG, "%s calib failed (%s), rollback=%s",
                     kind, esp_err_to_name(ret), esp_err_to_name(rollback));
        } else {
            ESP_LOGE(TAG, "%s calib failed before any chip write (%s)",
                     kind, esp_err_to_name(ret));
        }
    } else {
        for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
            if (!(mask & (1u << p))) continue;
            if (request->calibrate_offset) {
                ESP_LOGI(TAG, "%s offset calibrated phase=%d offset=%d->%d",
                         kind, p, result->phase[p].old_offset, result->phase[p].new_offset);
            } else {
                ESP_LOGI(TAG, "%s gain calibrated phase=%d ref=%.6f before=%.6f after=%.6f gain=%u->%u",
                         kind, p, result->phase[p].reference, result->phase[p].measured_before,
                         result->phase[p].measured_after, result->phase[p].old_gain,
                         result->phase[p].new_gain);
            }
        }
    }
    xSemaphoreGive(s_meter_mutex);
    return ret;
}

static esp_err_t energy_meter_average_power_raw(const energy_meter_power_offset_request_t *request,
                                                int32_t *average)
{
    uint16_t samples = request->samples ? request->samples : 20;
    double sum = 0.0;
    for (uint16_t n = 0; n < samples; n++) {
        int32_t raw = 0;
        ESP_RETURN_ON_ERROR(atm90e32as_read_power_raw(s_meter, request->phase,
                                                       request->type == ENERGY_METER_POWER_OFFSET_REACTIVE,
                                                       &raw), TAG, "power offset sample failed");
        sum += (double)raw;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    double mean = sum / (double)samples;
    ESP_RETURN_ON_FALSE(mean >= (double)INT32_MIN && mean <= (double)INT32_MAX,
                        ESP_ERR_INVALID_SIZE, TAG, "power offset average out of range");
    *average = (int32_t)llround(mean);
    return ESP_OK;
}

esp_err_t energy_meter_auto_calibrate_power_offset(
    const energy_meter_power_offset_request_t *request,
    energy_meter_power_offset_result_t *result)
{
    ESP_RETURN_ON_FALSE(request != NULL && result != NULL &&
                        request->phase < ATM90E32AS_PHASE_COUNT &&
                        request->type <= ENERGY_METER_POWER_OFFSET_REACTIVE,
                        ESP_ERR_INVALID_ARG, TAG, "invalid power offset request");
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "meter not initialized");
    memset(result, 0, sizeof(*result));

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_calib_t original = s_applied_calib;
    atm90e32as_calib_t working = original;
    int16_t *offset = request->type == ENERGY_METER_POWER_OFFSET_REACTIVE
                          ? &working.phase[request->phase].reactive_power_offset
                          : &working.phase[request->phase].active_power_offset;
    result->old_offset = *offset;
    *offset = 0;

    esp_err_t ret = energy_meter_apply_locked(&working);
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(request->settle_ms ? request->settle_ms : ENERGY_METER_MEASUREMENT_SETTLE_MS));
        ret = energy_meter_average_power_raw(request, &result->average_before_counts);
    }
    if (ret == ESP_OK) {
        int64_t correction = -(int64_t)result->average_before_counts;
        if (correction < INT16_MIN || correction > INT16_MAX) {
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            *offset = (int16_t)correction;
            result->new_offset = *offset;
            ret = energy_meter_apply_locked(&working);
        }
    }
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(request->settle_ms ? request->settle_ms : ENERGY_METER_MEASUREMENT_SETTLE_MS));
        ret = energy_meter_average_power_raw(request, &result->average_after_counts);
        if (ret == ESP_OK) {
            int32_t tolerance = request->residual_tolerance_counts > 0
                                    ? request->residual_tolerance_counts : 2;
            if (llabs((long long)result->average_after_counts) > tolerance) {
                ret = ESP_ERR_INVALID_RESPONSE;
            }
        }
    }
    if (ret != ESP_OK) {
        (void)energy_meter_apply_locked(&original);
        result->rolled_back = true;
    } else {
        result->offset_calibrated = true;
    }
    xSemaphoreGive(s_meter_mutex);
    return ret;
}

/* Average active power for one phase, returned in integer milliwatts. Pmean is
 * updated by the chip once every ~16 line cycles, so we sample every 100 ms to
 * capture independent averages. Integer mW keeps the PQGain math deterministic
 * and free of float rounding. */
esp_err_t energy_meter_get_average_active_power(atm90e32as_phase_t phase,
                                                uint16_t samples,
                                                uint16_t interval_ms,
                                                int64_t *average_mw)
{
    if (samples == 0) samples = 1;
    if (interval_ms == 0) interval_ms = 100;
    if (interval_ms > 1000) interval_ms = 1000;  /* Cap at 1s */

    int64_t sum = 0;
    for (uint16_t n = 0; n < samples; n++) {
        atm90e32as_measurements_t measurement;
        /* Read from cached measurements (updated by task every 100ms),
         * not directly from SPI bus — avoids race condition */
        ESP_RETURN_ON_ERROR(energy_meter_get_latest(&measurement), TAG,
                            "active power sample failed");
        float value = measurement.active_power[phase];
        ESP_RETURN_ON_FALSE(isfinite(value), ESP_ERR_INVALID_RESPONSE, TAG,
                            "invalid active power sample");
        sum += llround((double)value * 1000.0);
        if (n < samples - 1) {  /* Don't delay after last sample */
            vTaskDelay(pdMS_TO_TICKS(interval_ms));
        }
    }
    /* Round-half-away-from-zero; safe for a negative sum too. */
    int64_t q = sum / samples;
    int64_t r = sum % samples;
    if (2 * llabs(r) >= samples) q += (sum >= 0 ? 1 : -1);
    *average_mw = q;
    return ESP_OK;
}

esp_err_t energy_meter_auto_calibrate_pq_gain(const energy_meter_pq_gain_request_t *request,
                                              energy_meter_pq_gain_result_t *result)
{
    ESP_RETURN_ON_FALSE(request != NULL && result != NULL &&
                        request->phase < ATM90E32AS_PHASE_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid PQ gain request");
    ESP_RETURN_ON_FALSE(isfinite(request->reference_w) && request->reference_w > 0.0f &&
                        request->reference_w <= 1000000.0f,
                        ESP_ERR_INVALID_ARG, TAG, "P_ref must be > 0 and <= 1 MW");
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    memset(result, 0, sizeof(*result));

    uint16_t samples = request->samples ? request->samples : 3;
    uint32_t settle_ms = request->settle_ms ? request->settle_ms : 700;
    float tolerance_percent = request->tolerance_percent > 0.0f ? request->tolerance_percent
                            : (float)CONFIG_APP_ATM90E32AS_CALIB_TOLERANCE_PERCENT;

    /* Step 1: collect every value needed for the gain calculation before any
     * chip write, using integer milliwatts to avoid float arithmetic drift. */
    int64_t pref_mw = llround((double)request->reference_w * 1000.0);
    ESP_RETURN_ON_FALSE(pref_mw > 0 && pref_mw <= 1000000000LL,
                        ESP_ERR_INVALID_ARG, TAG, "P_ref mW out of range");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_calib_t original = s_applied_calib;
    atm90e32as_calib_t working = original;
    int16_t old_pq = original.phase[request->phase].pq_gain;

    int64_t pchip_mw = 0;
    esp_err_t ret = energy_meter_get_average_active_power(request->phase, samples, 100, &pchip_mw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PQ gain: failed to read P_chip");
    } else if (pchip_mw <= 0) {
        ESP_LOGE(TAG, "PQ gain: P_chip=%lld mW is not positive", (long long)pchip_mw);
        ret = ESP_ERR_INVALID_RESPONSE;
    }

    result->reference = (float)pref_mw / 1000.0f;
    result->measured_before = (float)pchip_mw / 1000.0f;
    result->old_pq_gain = old_pq;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "PQ gain inputs phase=%d P_ref=%lld mW P_chip=%lld mW old_pq_gain=%d tolerance=%.3f%% samples=%u",
                 request->phase, (long long)pref_mw, (long long)pchip_mw, old_pq,
                 tolerance_percent, (unsigned)samples);
    }

    /* Step 2: AN46103 composition formula in signed 16-bit LSB units:
     * new_pq = round((32768 + old_pq) * P_ref_mW / P_chip_mW) - 32768.
     * Numerator is non-negative because old_pq >= INT16_MIN and P_ref_mW > 0. */
    int16_t new_pq = 0;
    if (ret == ESP_OK) {
        int64_t numerator = (32768LL + (int64_t)old_pq) * pref_mw;
        int64_t quotient = numerator / pchip_mw;
        int64_t remainder = numerator % pchip_mw;
        /* Round half-away-from-zero (symmetric for positive and negative numerator) */
        if (2 * llabs(remainder) >= pchip_mw) {
            quotient += (numerator >= 0 ? 1 : -1);
        }

        int64_t new_pq64 = quotient - 32768LL;
        if (new_pq64 < INT16_MIN || new_pq64 > INT16_MAX) {
            ESP_LOGE(TAG, "PQ gain: calculated %lld out of int16 range", (long long)new_pq64);
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            new_pq = (int16_t)new_pq64;
            ESP_LOGI(TAG, "PQ gain calculated new_pq_gain=%d", new_pq);
        }
    }

    /* Step 3: load only the selected phase's PQGain, then let the existing apply
     * path write the calibration image and reload the metering DSP. */
    if (ret == ESP_OK) {
        working.phase[request->phase].pq_gain = new_pq;
        result->new_pq_gain = new_pq;
        ret = energy_meter_apply_locked(&working);
    }

    /* Step 4: verify against the same P_ref/tolerance supplied in step 1. */
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
        int64_t after_mw = 0;
        ret = energy_meter_get_average_active_power(request->phase, samples, 100, &after_mw);
        if (ret == ESP_OK) {
            result->measured_after = (float)after_mw / 1000.0f;

            int64_t error_x100 = llabs(after_mw - pref_mw) * 10000LL / pref_mw;
            int64_t tolerance_x100 = llround((double)tolerance_percent * 100.0);
            if (error_x100 > tolerance_x100) {
                ESP_LOGE(TAG,
                         "PQ gain: residual error %lld.%02lld%% exceeds tolerance %lld.%02lld%%",
                         (long long)(error_x100 / 100), (long long)(error_x100 % 100),
                         (long long)(tolerance_x100 / 100), (long long)(tolerance_x100 % 100));
                ret = ESP_ERR_INVALID_RESPONSE;
            } else {
                ESP_LOGI(TAG,
                         "PQ gain verified P_after=%lld mW error=%lld.%02lld%%",
                         (long long)after_mw, (long long)(error_x100 / 100),
                         (long long)(error_x100 % 100));
            }
        }
    }

    if (ret != ESP_OK) {
        esp_err_t rollback = energy_meter_apply_locked(&original);
        result->rolled_back = true;
        ESP_LOGE(TAG, "PQ gain calibration failed (%s), rollback=%s",
                 esp_err_to_name(ret), esp_err_to_name(rollback));
    } else {
        ESP_LOGI(TAG,
                 "PQ gain calibrated phase=%d P_ref=%.3f W P_before=%.3f W P_after=%.3f W pq_gain=%d->%d",
                 request->phase, result->reference, result->measured_before,
                 result->measured_after, result->old_pq_gain, result->new_pq_gain);
    }
    xSemaphoreGive(s_meter_mutex);
    return ret;
}

esp_err_t energy_meter_auto_calibrate_phase(const energy_meter_phase_calib_request_t *request,
                                            energy_meter_phase_calib_result_t *result)
{
    ESP_RETURN_ON_FALSE(request != NULL && result != NULL &&
                        request->phase < ATM90E32AS_PHASE_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid phase calib request");
    ESP_RETURN_ON_FALSE(isfinite(request->reference_w) && request->reference_w > 0.0f &&
                        request->reference_w <= 1000000.0f,
                        ESP_ERR_INVALID_ARG, TAG, "P_ref must be > 0 and <= 1 MW");
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL,
                        ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    memset(result, 0, sizeof(*result));

    uint16_t samples = request->samples ? request->samples : 3;
    uint32_t settle_ms = request->settle_ms ? request->settle_ms : 700;
    float tolerance_percent = request->tolerance_percent > 0.0f ? request->tolerance_percent
                            : (float)CONFIG_APP_ATM90E32AS_CALIB_TOLERANCE_PERCENT;

    int64_t pref_mw = llround((double)request->reference_w * 1000.0);
    ESP_RETURN_ON_FALSE(pref_mw > 0 && pref_mw <= 1000000000LL,
                        ESP_ERR_INVALID_ARG, TAG, "P_ref mW out of range");

    xSemaphoreTake(s_meter_mutex, portMAX_DELAY);
    atm90e32as_calib_t original = s_applied_calib;
    atm90e32as_calib_t working = original;

    /* AN46103 phase formula assumes Phi=0 while eps_p is measured. Production
     * calibrates once and writes Phi directly, so require the baseline first. */
    int16_t old_phi = original.phase[request->phase].phase_comp;
    if (old_phi != 0) {
        ESP_LOGE(TAG,
                 "phase calib: phase_comp=%d is not at baseline; run 'meter-cal default --field phi' then re-run auto-phi",
                 old_phi);
        xSemaphoreGive(s_meter_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    result->old_phase_comp = old_phi;

    /* Step 1: collect every input before any chip write. Gphase is selected from
     * the line frequency actually applied to the chip. */
    uint32_t gphase_x1000 = (original.line_freq == ATM90E32AS_LINE_FREQ_60HZ) ? 3136449U : 3763739U;
    result->gphase_x1000 = gphase_x1000;

    int64_t pchip_mw = 0;
    esp_err_t ret = energy_meter_get_average_active_power(request->phase, samples, 100, &pchip_mw);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "phase calib: failed to read P_chip");
    } else if (pchip_mw <= 0) {
        ESP_LOGE(TAG, "phase calib: P_chip=%lld mW is not positive", (long long)pchip_mw);
        ret = ESP_ERR_INVALID_RESPONSE;
    }

    result->reference = (float)pref_mw / 1000.0f;
    result->measured_before = (float)pchip_mw / 1000.0f;
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "phase calib inputs phase=%d P_ref=%lld mW P_chip=%lld mW line_freq=%s Gphase=%u tolerance=%.3f%% samples=%u",
                 request->phase, (long long)pref_mw, (long long)pchip_mw,
                 (original.line_freq == ATM90E32AS_LINE_FREQ_60HZ) ? "60Hz" : "50Hz",
                 (unsigned)gphase_x1000, tolerance_percent, (unsigned)samples);
    }

    /* Step 2: Phi = eps_p * Gphase, in signed 2.048MHz delay cycles.
     *   eps_p = (P_chip - P_ref) / P_ref
     *   Phi   = round( diff_mw * (Gphase*1000) / (pref_mw * 1000) )
     * |num| <= 1e6*1000*3763739 ~ 3.8e15 < 2^63; den <= 1e12. */
    int16_t new_phi = 0;
    if (ret == ESP_OK) {
        int64_t diff_mw = pchip_mw - pref_mw;
        int64_t num = diff_mw * (int64_t)gphase_x1000;
        int64_t den = pref_mw * 1000LL;          /* > 0 */
        int64_t q = num / den;
        int64_t r = num % den;
        if (2 * llabs(r) >= den) q += (num >= 0 ? 1 : -1);   /* half away from zero */

        if (q < -255 || q > 255) {
            ESP_LOGE(TAG, "phase calib: computed Phi=%lld out of +/-255 cycle range", (long long)q);
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            new_phi = (int16_t)q;
            ESP_LOGI(TAG, "phase calib computed new_phi=%d", new_phi);
        }
    }

    /* Step 3: load only the selected phase's Phi, then let the existing apply
     * path write the calibration image and reload the metering DSP. */
    if (ret == ESP_OK) {
        working.phase[request->phase].phase_comp = new_phi;
        result->new_phase_comp = new_phi;
        ret = energy_meter_apply_locked(&working);
    }

    /* Step 4: verify residual active-power error at PF=0.5L. PAngle is read for a
     * sanity log only; it never gates PASS/FAIL. */
    if (ret == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(settle_ms));
        int64_t after_mw = 0;
        ret = energy_meter_get_average_active_power(request->phase, samples, 100, &after_mw);
        if (ret == ESP_OK) {
            result->measured_after = (float)after_mw / 1000.0f;

            atm90e32as_measurements_t measurement;
            if (atm90e32as_read_measurements(s_meter, &measurement) == ESP_OK &&
                isfinite(measurement.phase_angle[request->phase])) {
                result->phase_angle_after = measurement.phase_angle[request->phase];
                ESP_LOGI(TAG, "phase calib PAngle=%.1f deg (expect ~60.0 at PF=0.5L)",
                         result->phase_angle_after);
            }

            int64_t error_x100 = llabs(after_mw - pref_mw) * 10000LL / pref_mw;
            int64_t tolerance_x100 = llround((double)tolerance_percent * 100.0);
            if (error_x100 > tolerance_x100) {
                ESP_LOGE(TAG,
                         "phase calib: residual error %lld.%02lld%% exceeds tolerance %lld.%02lld%%",
                         (long long)(error_x100 / 100), (long long)(error_x100 % 100),
                         (long long)(tolerance_x100 / 100), (long long)(tolerance_x100 % 100));
                ret = ESP_ERR_INVALID_RESPONSE;
            } else {
                ESP_LOGI(TAG,
                         "phase calib verified P_after=%lld mW error=%lld.%02lld%%",
                         (long long)after_mw, (long long)(error_x100 / 100),
                         (long long)(error_x100 % 100));
            }
        }
    }

    if (ret != ESP_OK) {
        esp_err_t rollback = energy_meter_apply_locked(&original);
        result->rolled_back = true;
        ESP_LOGE(TAG, "phase calibration failed (%s), rollback=%s",
                 esp_err_to_name(ret), esp_err_to_name(rollback));
    } else {
        ESP_LOGI(TAG,
                 "phase calibrated phase=%d P_ref=%.3f W P_before=%.3f W P_after=%.3f W phi=%d->%d PAngle=%.1f deg",
                 request->phase, result->reference, result->measured_before,
                 result->measured_after, result->old_phase_comp, result->new_phase_comp,
                 result->phase_angle_after);
    }
    xSemaphoreGive(s_meter_mutex);
    return ret;
}

/* Single-phase auto calibration is now a thin wrapper over the multi-phase core:
 * it builds a one-bit mask, runs the shared single-shot path, and maps the result
 * back into the legacy struct. Callers (console_task, web_portal) are unchanged.
 * The 3P3W rejection for voltage-phase-B is subsumed by multi's global 3P3W gate. */
esp_err_t energy_meter_auto_calibrate(const energy_meter_auto_calib_request_t *request,
                                      energy_meter_auto_calib_result_t *result)
{
    ESP_RETURN_ON_FALSE(request != NULL && result != NULL && request->phase < ATM90E32AS_PHASE_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid auto calibration request");
    ESP_RETURN_ON_FALSE(s_meter != NULL && s_meter_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "meter not initialized");

    memset(result, 0, sizeof(*result));

    energy_meter_multi_calib_request_t multi_req = {
        .phase_mask = (uint8_t)(1u << request->phase),
        .current = request->current,
        .calibrate_offset = request->calibrate_offset,
        .source = request->source,
        .manual_reference = request->manual_reference,
        .samples = request->samples,
        .settle_ms = request->settle_ms,
        .tolerance_percent = request->tolerance_percent,
    };
    energy_meter_multi_calib_result_t multi;
    memset(&multi, 0, sizeof(multi));   /* don't rely on multi's internal init order */
    esp_err_t ret = energy_meter_auto_calibrate_multi(&multi_req, &multi);

    const energy_meter_multi_phase_result_t *ph = &multi.phase[request->phase];
    result->reference = ph->reference;
    result->measured_before = ph->measured_before;
    result->measured_after = ph->measured_after;
    result->old_gain = ph->old_gain;
    result->new_gain = ph->new_gain;
    result->old_offset = ph->old_offset;
    result->new_offset = ph->new_offset;
    result->offset_calibrated = multi.offset_calibrated;
    result->old_pga = multi.pga;
    result->new_pga = multi.pga;
    result->iterations = 1;
    result->rolled_back = multi.rolled_back;
    result->pga_increased = false;
    return ret;
}

esp_err_t energy_meter_task_start(void)
{
    ESP_RETURN_ON_ERROR(energy_meter_init(), TAG, "init energy meter failed");

    BaseType_t ret = xTaskCreate(energy_meter_task,
                                 "energy_meter_task",
                                 CONFIG_APP_ENERGY_METER_TASK_STACK_SIZE,
                                 NULL,
                                 CONFIG_APP_ENERGY_METER_TASK_PRIORITY,
                                 NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_FAIL, TAG, "create energy meter task failed");

    return ESP_OK;
}

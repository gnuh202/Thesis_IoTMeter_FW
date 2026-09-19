#include "alarm_manager.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "boot_manager.h"
#include "io_expander.h"
#include "sdkconfig.h"

#define ALARM_TAG "alarm_mgr"

/* EMM status bits (datasheet: EMMState0 0x71 / EMMState1 0x72). */
#define EMM0_OV_SHIFT   10
#define EMM0_OI_SHIFT   13
#define EMM1_SAG_SHIFT  12
#define EMM1_PLOS_SHIFT 8
#define EMM1_FHI_BIT    15
#define EMM1_FLO_BIT    11

/* WarnOut = IC fatal-error pin (pin 29 -> MCU GPIO 41). High = fatal. */
#ifdef CONFIG_APP_ATM90E32AS_WARN_GPIO
#define ALARM_WARN_GPIO CONFIG_APP_ATM90E32AS_WARN_GPIO
#else
#define ALARM_WARN_GPIO 41
#endif

#define ALARM_ANCHOR_V_MIN 50.0f
#define ALARM_ANCHOR_A_MIN 0.5f
#define ALARM_PHASELOSS_RATIO 0.10f
#define ALARM_OC_RETRY_MS 5000U
#define ALARM_TH_GAIN_NUM ((float)M_SQRT2 * 16384.0f)

static portMUX_TYPE s_alarm_mux = portMUX_INITIALIZER_UNLOCKED;
static uint16_t s_latched;
static uint16_t s_active;
static uint32_t s_bit_since[ALARM_BIT_COUNT];
static bool s_apply_pending = true;
static bool s_v_armed;
static bool s_oc_anchored;
static uint32_t s_oc_retry_at;

static bool s_en_ov, s_en_oc, s_en_uv, s_en_pl, s_en_freq;
static uint32_t s_confirm_ms;
static uint8_t s_out_role[2];
static bool s_out_driven[2];

static uint32_t alarm_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int alarm_popcount16(uint16_t v)
{
    int n = 0;
    while (v) {
        v &= (uint16_t)(v - 1);
        n++;
    }
    return n;
}

static float alarm_median3(float a, float b, float c)
{
    float lo = a < b ? a : b;
    float hi = a < b ? b : a;
    if (c < lo) {
        return lo;
    }
    if (c > hi) {
        return hi;
    }
    return c;
}

static uint16_t alarm_th_reg(float scale, float value)
{
    float reg = scale * value;
    if (!(reg > 0.0f)) {
        return 0;
    }
    if (reg > 65535.0f) {
        return 0xFFFF;
    }
    return (uint16_t)(reg + 0.5f);
}

void alarm_manager_apply_ic(atm90e32as_handle_t handle, const atm90e32as_calib_t *calib,
                            const config_manager_t *cfg, const atm90e32as_measurements_t *m)
{
    if (handle == NULL || calib == NULL || cfg == NULL || m == NULL) {
        return;
    }

    uint32_t now = alarm_now_ms();
    bool v_pending, oc_waiting;
    portENTER_CRITICAL(&s_alarm_mux);
    v_pending = s_apply_pending;
    bool oc_due = (!s_oc_anchored) && (int32_t)(now - s_oc_retry_at) >= 0;
    oc_waiting = !s_oc_anchored;
    portEXIT_CRITICAL(&s_alarm_mux);

    /* Fast path: nothing requested and the OC retry is not due yet. */
    if (!v_pending && !(oc_waiting && oc_due)) {
        return;
    }

    /* Cache the firmware-side config first: masks, confirm window and output
     * roles are pure software and must not be held hostage by the SPI anchor
     * below, which can bail out for cycles on end when the grid is down. */
    portENTER_CRITICAL(&s_alarm_mux);
    s_en_ov  = cfg->alarm_voltage_high_enable;
    s_en_uv  = cfg->alarm_voltage_low_enable;
    s_en_oc  = cfg->alarm_over_current_enable;
    s_en_pl  = cfg->alarm_phase_loss_enable;
    s_en_freq = cfg->alarm_frequency_enable;
    s_confirm_ms = (uint32_t)cfg->alarm_trigger_delay_s * 1000U;
    s_out_role[0] = cfg->alarm_out0_role;
    s_out_role[1] = cfg->alarm_out1_role;
    portEXIT_CRITICAL(&s_alarm_mux);

    /* Raw channel values for the ratio anchor (data-space registers). */
    uint16_t urms[3], irms[3];
    if (atm90e32as_read_raw_rms(handle, urms, irms) != ESP_OK) {
        ESP_LOGW(ALARM_TAG, "threshold anchor read failed; retrying next cycle");
        return;
    }

    /* Voltage anchor: ThReg-per-volt from each valid phase (>= 2 needed), then
     * median. Ratio method: raw/measured absorbs the channel LSB; the
     * datasheet sqrt(2)*2^14/gain factor converts the RMS-domain register
     * into the sample-domain comparator value. */
    float scale = 0.0f;
    float scales[3];
    int ns = 0;
    float v_sum = 0.0f;
    for (int p = 0; p < 3; p++) {
        if (m->voltage_valid[p] && m->voltage[p] >= ALARM_ANCHOR_V_MIN && urms[p] > 0) {
            float ug = (float)calib->phase[p].voltage_gain;
            scales[ns++] = (float)urms[p] / m->voltage[p] * (ALARM_TH_GAIN_NUM / ug);
            v_sum += m->voltage[p];
        }
    }
    if (ns < 2) {
        /* No grid yet: stay unarmed (boot false-latch gate). */
        ESP_LOGI(ALARM_TAG, "no valid voltage anchor yet; alarm stays unarmed");
        return;
    }
    float v_med = v_sum / (float)ns;
    if (ns == 2) {
        scale = (scales[0] + scales[1]) * 0.5f;
    } else {
        scale = alarm_median3(scales[0], scales[1], scales[2]);
    }

    /* Over-current anchor needs load current, so it may lag the V anchor. */
    bool write_oi = false;
    uint16_t oi_th = 0;
    if (cfg->alarm_over_current_enable) {
        int best = 0;
        float best_i = m->current[0];
        for (int p = 1; p < 3; p++) {
            if (m->current[p] > best_i) {
                best_i = m->current[p];
                best = p;
            }
        }
        if (best_i >= ALARM_ANCHOR_A_MIN && irms[best] > 0) {
            float ig = (float)calib->phase[best].current_gain;
            float iscale = (float)irms[best] / best_i * (ALARM_TH_GAIN_NUM / ig);
            oi_th = alarm_th_reg(iscale, cfg->alarm_over_current_a);
            write_oi = true;
            ESP_LOGI(ALARM_TAG, "OIth=%u (iscale=%.1f/A from phase %c)",
                     oi_th, iscale, 'A' + best);
        } else if (!s_oc_anchored) {
            s_oc_retry_at = now + ALARM_OC_RETRY_MS;
            ESP_LOGI(ALARM_TAG, "no load current yet; OIth anchor deferred");
        }
    } else {
        s_oc_anchored = true;   /* disabled: nothing to anchor */
    }

    /* All V/freq thresholds are written even when a firmware-level category
     * is disabled: the IC verdicts also show up on Modbus IR_SYS_STATUS. */
    atm90e32as_warning_thresholds_t th = {
        .ov_th  = alarm_th_reg(scale, cfg->alarm_voltage_high_v),
        .sag_th = alarm_th_reg(scale, cfg->alarm_voltage_low_v),
        .phase_loss_th = alarm_th_reg(scale, v_med * ALARM_PHASELOSS_RATIO),
        .freq_lo_th = (uint16_t)(cfg->alarm_frequency_low_hz * 100.0f + 0.5f),
        .freq_hi_th = (uint16_t)(cfg->alarm_frequency_high_hz * 100.0f + 0.5f),
        .oi_th = oi_th,
        .write_oi_th = write_oi,
    };
    esp_err_t err = atm90e32as_write_warning_thresholds(handle, &th);
    if (err != ESP_OK) {
        ESP_LOGW(ALARM_TAG, "threshold write failed (%s); retrying", esp_err_to_name(err));
        return;
    }
    if (write_oi) {
        s_oc_anchored = true;
    }
    ESP_LOGI(ALARM_TAG, "IC thresholds: OVth=%u SagTh=%u PLossTh=%u FreqLo=%u FreqHi=%u "
                        "(scale=%.1f/V, Vnom=%.1f)",
             th.ov_th, th.sag_th, th.phase_loss_th, th.freq_lo_th, th.freq_hi_th,
             scale, v_med);

    /* Thresholds are in the IC: the request is satisfied and V/freq are armed. */
    portENTER_CRITICAL(&s_alarm_mux);
    s_apply_pending = false;
    s_v_armed = true;
    portEXIT_CRITICAL(&s_alarm_mux);
}

/* Evaluate one snapshot: decode, confirm, latch, edge-drive. Runs in the
 * energy-meter task; engineering/calibration mode pauses evaluation so a
 * calibration jump cannot latch an alarm or drive an output. */
void alarm_manager_service(const atm90e32as_measurements_t *m)
{
    if (m == NULL || boot_manager_engineering_mode()) {
        if (m != NULL) {
            portENTER_CRITICAL(&s_alarm_mux);
            memset(s_bit_since, 0, sizeof(s_bit_since));
            portEXIT_CRITICAL(&s_alarm_mux);
        }
        return;
    }

    /* One coherent snapshot of the gates (the writer is apply_ic in this same
     * task, but the getters/HMI also touch this state). */
    bool armed, oc_armed, en_ov, en_oc, en_uv, en_pl, en_freq;
    portENTER_CRITICAL(&s_alarm_mux);
    armed    = s_v_armed;
    oc_armed = s_oc_anchored;
    en_ov = s_en_ov; en_oc = s_en_oc; en_uv = s_en_uv;
    en_pl = s_en_pl; en_freq = s_en_freq;
    portEXIT_CRITICAL(&s_alarm_mux);

    uint16_t active = 0;
    uint16_t st0 = m->sys_status0;
    uint16_t st1 = m->sys_status1;

    /* Armed gate: the IC reset defaults (SagTh 0x1000, PhaseLossTh 0x0400,
     * FreqLoTh 49.00 Hz) already read as "sag + phase loss + freq low" while the
     * grid is absent. Until our own thresholds are really in the IC, its
     * threshold verdicts mean nothing and must not be evaluated. */
    if (armed) {
        if (en_ov) {
            active |= (uint16_t)((st0 >> EMM0_OV_SHIFT) & 0x7U);
        }
        if (en_uv) {
            active |= (uint16_t)(((st1 >> EMM1_SAG_SHIFT) & 0x7U) << ALARM_BIT_UV_A);
        }
        if (en_pl) {
            active |= (uint16_t)(((st1 >> EMM1_PLOS_SHIFT) & 0x7U) << ALARM_BIT_PL_A);
        }
        if (en_freq) {
            if (st1 & (1U << EMM1_FHI_BIT)) {
                active |= (uint16_t)(1U << ALARM_BIT_FREQ_HI);
            }
            if (st1 & (1U << EMM1_FLO_BIT)) {
                active |= (uint16_t)(1U << ALARM_BIT_FREQ_LO);
            }
        }
    }
    /* OIth has its own anchor (it waits for load current), so it gates apart. */
    if (en_oc && oc_armed) {
        active |= (uint16_t)(((st0 >> EMM0_OI_SHIFT) & 0x7U) << ALARM_BIT_OC_A);
    }
    /* WarnOut: fatal IC error (config CRC / internal). Always reportable. */
    if (gpio_get_level(ALARM_WARN_GPIO) > 0) {
        active |= (uint16_t)(1U << ALARM_BIT_IC_ERROR);
    }

    /* Confirm window + latch. */
    uint32_t now = alarm_now_ms();
    uint16_t newly = 0;
    portENTER_CRITICAL(&s_alarm_mux);
    s_active = active;
    for (int b = 0; b < ALARM_BIT_COUNT; b++) {
        uint16_t mask = (uint16_t)(1U << b);
        if (active & mask) {
            if (s_bit_since[b] == 0) {
                s_bit_since[b] = now;
            } else if ((now - s_bit_since[b]) >= s_confirm_ms) {
                if (!(s_latched & mask)) {
                    newly |= mask;
                }
                s_latched |= mask;
            }
        } else {
            s_bit_since[b] = 0;
        }
    }
    portEXIT_CRITICAL(&s_alarm_mux);

    /* Alarm drives outputs ON at the latch edge only - the user keeps full
     * manual authority and can override at any time. */
    if (newly != 0) {
        ESP_LOGW(ALARM_TAG, "alarm latched: 0x%04X (active 0x%04X)", newly, active);
        for (int i = 0; i < 2; i++) {
            if (s_out_role[i] != 0 && !s_out_driven[i]) {
                esp_err_t r = (i == 0) ? io_expander_set_out0(true)
                                       : io_expander_set_out1(true);
                if (r == ESP_OK) {
                    s_out_driven[i] = true;
                    ESP_LOGW(ALARM_TAG, "alarm drove OUT%d ON", i + 1);
                }
            }
        }
    }
}

void alarm_manager_reset_latch(void)
{
    bool d0, d1;
    portENTER_CRITICAL(&s_alarm_mux);
    s_latched = 0;
    memset(s_bit_since, 0, sizeof(s_bit_since));
    d0 = s_out_driven[0];
    d1 = s_out_driven[1];
    s_out_driven[0] = false;
    s_out_driven[1] = false;
    portEXIT_CRITICAL(&s_alarm_mux);
    if (d0) {
        io_expander_set_out0(false);
    }
    if (d1) {
        io_expander_set_out1(false);
    }
    ESP_LOGI(ALARM_TAG, "latch cleared by user; alarm outputs OFF");
}

void alarm_manager_request_apply(void)
{
    uint32_t now = alarm_now_ms();
    portENTER_CRITICAL(&s_alarm_mux);
    s_apply_pending = true;
    /* The limits or the channel gains may have moved, so every threshold now in
     * the IC is stale. Disarm both anchors: evaluation stays masked until the
     * new values are really written, so a new limit can never be judged against
     * the old comparator value. */
    s_v_armed = false;
    s_oc_anchored = false;
    s_oc_retry_at = now;
    portEXIT_CRITICAL(&s_alarm_mux);
}

bool alarm_manager_ic_access_pending(void)
{
    bool pending;
    portENTER_CRITICAL(&s_alarm_mux);
    pending = s_apply_pending || !s_oc_anchored;
    portEXIT_CRITICAL(&s_alarm_mux);
    return pending;
}

int alarm_manager_active_count(void)
{
    int n;
    portENTER_CRITICAL(&s_alarm_mux);
    n = alarm_popcount16(s_latched);
    portEXIT_CRITICAL(&s_alarm_mux);
    return n;
}

uint16_t alarm_manager_latched_bitmap(void)
{
    uint16_t v;
    portENTER_CRITICAL(&s_alarm_mux);
    v = s_latched;
    portEXIT_CRITICAL(&s_alarm_mux);
    return v;
}

uint8_t alarm_manager_warning_byte(void)
{
    uint8_t w = 0;
    portENTER_CRITICAL(&s_alarm_mux);
    if (s_latched & 0x0007U)  w |= 1U << 0;  /* OV any */
    if (s_latched & 0x0038U)  w |= 1U << 2;  /* OC any */
    if (s_latched & 0x01C0U)  w |= 1U << 1;  /* UV/sag any */
    if (s_latched & 0x0E00U)  w |= 1U << 3;  /* phase loss any */
    if (s_latched & (1U << ALARM_BIT_FREQ_HI)) w |= 1U << 4;
    if (s_latched & (1U << ALARM_BIT_FREQ_LO)) w |= 1U << 5;
    if (s_latched & (1U << ALARM_BIT_IC_ERROR)) w |= 1U << 6;
    portEXIT_CRITICAL(&s_alarm_mux);
    return w;
}

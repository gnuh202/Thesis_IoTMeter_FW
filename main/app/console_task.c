#include "console_task.h"

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "argtable3/argtable3.h"
#include "cert_store.h"
#include "config_apply.h"
#include "config_manager.h"
#include "config_store.h"
#include "energy_meter_task.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bus.h"
#include "io_expander.h"
#include "linenoise/linenoise.h"
#include "modbus_master_task.h"
#include "modbus_meters.h"
#include "modbus_slave_task.h"
#include "network_manager.h"
#include "nvs.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"
#if CONFIG_APP_DP_DEBUG
#include "config_manager.h"
#include "register_access.h"
#endif

static const char *TAG = "console_task";

static int parse_phase(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    if (strcmp(name, "a") == 0 || strcmp(name, "A") == 0) {
        return ATM90E32AS_PHASE_A;
    }
    if (strcmp(name, "b") == 0 || strcmp(name, "B") == 0) {
        return ATM90E32AS_PHASE_B;
    }
    if (strcmp(name, "c") == 0 || strcmp(name, "C") == 0) {
        return ATM90E32AS_PHASE_C;
    }
    return -1;
}

/* meter latest */
static int cmd_meter_latest(int argc, char **argv)
{
    atm90e32as_measurements_t m;
    esp_err_t ret = energy_meter_get_latest(&m);
    if (ret != ESP_OK) {
        printf("No measurement available yet (%s)\n", esp_err_to_name(ret));
        return 1;
    }

    for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
        printf("Phase %c: V=%.2f V  I=%.3f A  P=%.2f W  Q=%.2f var  S=%.2f VA  PF=%.3f  angle=%.1f deg\n",
               'A' + i,
               m.voltage[i], m.current[i],
               m.active_power[i], m.reactive_power[i], m.apparent_power[i],
               m.power_factor[i], m.phase_angle[i]);
    }
    printf("Neutral I=%.3f A\n", m.current_neutral);
    printf("Total: P=%.2f W  Q=%.2f var  S=%.2f VA  PF=%.3f\n",
           m.total_active_power, m.total_reactive_power, m.total_apparent_power, m.total_power_factor);
    printf("Frequency=%.2f Hz  Temp=%.1f C\n", m.frequency, m.temperature);
    printf("Status: SYS0=0x%04X SYS1=0x%04X METER0=0x%04X METER1=0x%04X\n",
           m.sys_status0, m.sys_status1, m.meter_status0, m.meter_status1);
    return 0;
}

/* meter reg read/write */
static struct {
    struct arg_str *op;
    struct arg_str *addr;
    struct arg_str *value;
    struct arg_end *end;
} s_reg_args;

static int cmd_meter_reg(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_reg_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_reg_args.end, argv[0]);
        return 1;
    }

    const char *op = s_reg_args.op->sval[0];
    uint16_t addr = (uint16_t)strtol(s_reg_args.addr->sval[0], NULL, 0);

    if (strcmp(op, "read") == 0) {
        uint16_t value = 0;
        esp_err_t ret = energy_meter_read_register(addr, &value);
        if (ret != ESP_OK) {
            printf("read failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("reg[0x%02X] = 0x%04X (%u)\n", addr, value, value);
        return 0;
    }

    if (strcmp(op, "write") == 0) {
        if (s_reg_args.value->count == 0) {
            printf("write requires a value\n");
            return 1;
        }
        uint16_t value = (uint16_t)strtol(s_reg_args.value->sval[0], NULL, 0);
        esp_err_t ret = energy_meter_write_register(addr, value);
        if (ret != ESP_OK) {
            printf("write failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("reg[0x%02X] <= 0x%04X\n", addr, value);
        return 0;
    }

    printf("unknown op '%s' (use read|write)\n", op);
    return 1;
}

static void print_calibration(const atm90e32as_calib_t *c)
{
    printf("line_freq=%s wiring=%s pga=x%d (runtime stamp; system PGA is config-owned)\n",
           c->line_freq == ATM90E32AS_LINE_FREQ_60HZ ? "60Hz" : "50Hz",
           c->wiring_mode == ATM90E32AS_WIRING_3P3W ? "3P3W" : "3P4W",
           1 << c->pga_gain);
    for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
        const atm90e32as_phase_calib_t *p = &c->phase[i];
        printf("Phase %c: ugain=%u igain=%u uoffset=%d ioffset=%d poffset=%d qoffset=%d pqgain=%d phi=%d pgainf=%d\n",
               'A' + i,
               p->voltage_gain, p->current_gain,
               p->voltage_offset, p->current_offset,
               p->active_power_offset, p->reactive_power_offset,
               p->pq_gain, p->phase_comp, p->fundamental_power_gain);
    }
}

/* meter cal ... */
static struct {
    struct arg_str *sub;
    struct arg_str *field;
    struct arg_str *phase;
    struct arg_int *u;
    struct arg_int *i;
    struct arg_int *p;
    struct arg_int *q;
    struct arg_int *phi;
    struct arg_str *value;
    struct arg_dbl *tolerance;
    struct arg_dbl *error;
    struct arg_int *interval;
    struct arg_lit *apply;
    struct arg_end *end;
} s_cal_args;

/* Chip-wide mode fields (pga|wiring|freq) are not per-phase. Returns true if
 * handled (whether success or error), false if the field is not chip-wide. */
static bool cal_set_mode_field(const char *field, bool apply, int *result)
{
    const char *val = s_cal_args.value->count ? s_cal_args.value->sval[0] : NULL;

    if (strcmp(field, "pga") == 0) {
        if (val == NULL) { printf("pga requires --value 1|2|4\n"); *result = 1; return true; }
        atm90e32as_pga_gain_t pga;
        if (strcmp(val, "1") == 0) pga = ATM90E32AS_PGA_GAIN_1X;
        else if (strcmp(val, "2") == 0) pga = ATM90E32AS_PGA_GAIN_2X;
        else if (strcmp(val, "4") == 0) pga = ATM90E32AS_PGA_GAIN_4X;
        else { printf("invalid pga '%s' (use 1|2|4)\n", val); *result = 1; return true; }
        printf("NOTE: PGA is the current-channel gain. Re-calibrate igain after changing it.\n");
        esp_err_t ret = energy_meter_set_pga_gain(pga, apply);
        printf("set pga=%s%s: %s\n", val, apply ? " and applied" : "", esp_err_to_name(ret));
        *result = ret == ESP_OK ? 0 : 1;
        return true;
    }
    if (strcmp(field, "wiring") == 0) {
        if (val == NULL) { printf("wiring requires --value 3p4w|3p3w\n"); *result = 1; return true; }
        atm90e32as_wiring_mode_t mode;
        if (strcmp(val, "3p4w") == 0) mode = ATM90E32AS_WIRING_3P4W;
        else if (strcmp(val, "3p3w") == 0) mode = ATM90E32AS_WIRING_3P3W;
        else { printf("invalid wiring '%s' (use 3p4w|3p3w)\n", val); *result = 1; return true; }
        /* Switching mode selects the destination profile's own gains — do not
         * push the current mode's gains across. Handled by a dedicated call. */
        esp_err_t ret = energy_meter_set_wiring_mode(mode, apply);
        printf("set wiring=%s%s: %s\n", val, apply ? " and applied" : "", esp_err_to_name(ret));
        *result = ret == ESP_OK ? 0 : 1;
        return true;
    }
    if (strcmp(field, "freq") == 0) {
        if (val == NULL) { printf("freq requires --value 50|60\n"); *result = 1; return true; }
        atm90e32as_line_freq_t freq;
        if (strcmp(val, "50") == 0) freq = ATM90E32AS_LINE_FREQ_50HZ;
        else if (strcmp(val, "60") == 0) freq = ATM90E32AS_LINE_FREQ_60HZ;
        else { printf("invalid freq '%s' (use 50|60)\n", val); *result = 1; return true; }
        esp_err_t ret = energy_meter_set_line_freq(freq, apply);
        printf("set freq=%s%s: %s\n", val, apply ? " and applied" : "", esp_err_to_name(ret));
        *result = ret == ESP_OK ? 0 : 1;
        return true;
    }
    return false;
}

static int cmd_meter_cal(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_cal_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_cal_args.end, argv[0]);
        return 1;
    }

    const char *sub = s_cal_args.sub->sval[0];
    bool apply = s_cal_args.apply->count > 0;

    if (strcmp(sub, "show") == 0) {
        atm90e32as_calib_t c;
        if (energy_meter_get_calibration(&c) != ESP_OK) {
            printf("get calibration failed\n");
            return 1;
        }
        print_calibration(&c);
        return 0;
    }

    if (strcmp(sub, "default") == 0) {
        /* No --field: reset the whole calibration image (back-compatible). */
        if (s_cal_args.field->count == 0) {
            esp_err_t ret = energy_meter_reset_calibration_defaults(apply);
            printf("defaults loaded%s: %s\n", apply ? " and applied" : "", esp_err_to_name(ret));
            return ret == ESP_OK ? 0 : 1;
        }

        /* --field <f>: reset only the named field(s). Optional --phase a|b|c
         * limits the reset to one phase; omitted means all three. Used to bring a
         * single field to baseline (e.g. Phi before auto-phi) without disturbing
         * gains already calibrated. */
        const char *field = s_cal_args.field->sval[0];
        int phase_sel = -1;          /* -1 = all phases */
        if (s_cal_args.phase->count > 0) {
            phase_sel = parse_phase(s_cal_args.phase->sval[0]);
            if (phase_sel < 0) {
                printf("invalid phase (use a|b|c)\n");
                return 1;
            }
        }

        atm90e32as_calib_t c;
        if (energy_meter_get_calibration(&c) != ESP_OK) {
            printf("get calibration failed\n");
            return 1;
        }

        for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
            if (phase_sel >= 0 && i != phase_sel) continue;
            atm90e32as_phase_calib_t *pc = &c.phase[i];
            if (strcmp(field, "phi") == 0 || strcmp(field, "phase") == 0) {
                pc->phase_comp = 0;
            } else if (strcmp(field, "pqgain") == 0 || strcmp(field, "pq-gain") == 0) {
                pc->pq_gain = 0;
            } else if (strcmp(field, "uigain") == 0 || strcmp(field, "gain") == 0) {
                pc->voltage_gain = 0x8000U;
                pc->current_gain = 0x8000U;
            } else if (strcmp(field, "uioffset") == 0 || strcmp(field, "offset") == 0) {
                pc->voltage_offset = 0;
                pc->current_offset = 0;
            } else if (strcmp(field, "power-offset") == 0) {
                pc->active_power_offset = 0;
                pc->reactive_power_offset = 0;
            } else if (strcmp(field, "fundamental") == 0 ||
                       strcmp(field, "fundamental-power-gain") == 0) {
                pc->fundamental_power_gain = 0;
            } else if (strcmp(field, "all") == 0) {
                pc->voltage_gain = 0x8000U;
                pc->current_gain = 0x8000U;
                pc->voltage_offset = 0;
                pc->current_offset = 0;
                pc->active_power_offset = 0;
                pc->reactive_power_offset = 0;
                pc->pq_gain = 0;
                pc->phase_comp = 0;
                pc->fundamental_power_gain = 0;
            } else {
                printf("unknown field '%s' (phi|pqgain|uigain|uioffset|power-offset|fundamental|all)\n", field);
                return 1;
            }
        }

        /* Per-phase field resets apply to the chip immediately: the operator is
         * calibrating and needs to see the change right away (e.g. baseline Phi
         * before auto-phi). --apply is accepted but now a no-op here. Only the
         * whole-image `default` (no --field) and chip-wide pga/wiring/freq still
         * need --apply. */
        esp_err_t ret = energy_meter_set_calibration(&c);
        if (ret == ESP_OK) {
            ret = energy_meter_apply_calibration();
        }
        printf("default --field %s (phase %s) applied to chip: %s\n", field,
               phase_sel >= 0 ? s_cal_args.phase->sval[0] : "all", esp_err_to_name(ret));
        return ret == ESP_OK ? 0 : 1;
    }

    if (strcmp(sub, "apply") == 0) {
        esp_err_t ret = energy_meter_apply_calibration();
        printf("apply: %s\n", esp_err_to_name(ret));
        return ret == ESP_OK ? 0 : 1;
    }

    if (strcmp(sub, "save") == 0) {
        esp_err_t ret = energy_meter_save_calibration();
        printf("save: %s\n", esp_err_to_name(ret));
        return ret == ESP_OK ? 0 : 1;
    }

    if (strcmp(sub, "load") == 0) {
        esp_err_t ret = energy_meter_load_calibration(apply);
        printf("load%s: %s\n", apply ? " and applied" : "", esp_err_to_name(ret));
        return ret == ESP_OK ? 0 : 1;
    }

    if (strcmp(sub, "auto-power-offset") == 0) {
        if (s_cal_args.field->count == 0 || s_cal_args.phase->count == 0) {
            printf("auto-power-offset requires --field p|q --phase a|b|c\n");
            return 1;
        }
        int phase = parse_phase(s_cal_args.phase->sval[0]);
        const char *field = s_cal_args.field->sval[0];
        if (phase < 0 || (strcmp(field, "p") != 0 && strcmp(field, "q") != 0)) {
            printf("field must be p or q\n");
            return 1;
        }
        energy_meter_power_offset_request_t request = {
            .phase = (atm90e32as_phase_t)phase,
            .type = strcmp(field, "q") == 0 ? ENERGY_METER_POWER_OFFSET_REACTIVE
                                             : ENERGY_METER_POWER_OFFSET_ACTIVE,
            .samples = 20,
            .settle_ms = 200,
            .residual_tolerance_counts = 2,
        };
        energy_meter_power_offset_result_t result;
        esp_err_t ret = energy_meter_auto_calibrate_power_offset(&request, &result);
        printf("auto-power-offset %s phase %c: %s raw_before=%ld raw_after=%ld offset=%d->%d rollback=%d\n",
               field, 'A' + phase, esp_err_to_name(ret),
               (long)result.average_before_counts, (long)result.average_after_counts,
               result.old_offset, result.new_offset, result.rolled_back);
        if (ret == ESP_OK) printf("verify the result, then run meter-cal save\n");
        return ret == ESP_OK ? 0 : 1;
    }

    if (strcmp(sub, "auto-pq-gain") == 0) {
        if (s_cal_args.phase->count == 0 || s_cal_args.value->count == 0) {
            printf("auto-pq-gain requires --phase a|b|c --value <P_ref_W> [--tolerance <percent>]\n");
            return 1;
        }
        int phase = parse_phase(s_cal_args.phase->sval[0]);
        float ref_w = strtof(s_cal_args.value->sval[0], NULL);
        if (phase < 0 || ref_w <= 0.0f) {
            printf("phase must be a|b|c and P_ref must be > 0\n");
            return 1;
        }
        float tolerance = (s_cal_args.tolerance->count > 0)
                        ? (float)s_cal_args.tolerance->dval[0]
                        : (float)CONFIG_APP_ATM90E32AS_CALIB_TOLERANCE_PERCENT;
        energy_meter_pq_gain_request_t request = {
            .phase = (atm90e32as_phase_t)phase,
            .reference_w = ref_w,
            .samples = 3,
            .settle_ms = 700,
            .tolerance_percent = tolerance,
        };
        energy_meter_pq_gain_result_t result;
        esp_err_t ret = energy_meter_auto_calibrate_pq_gain(&request, &result);
        printf("auto-pq-gain phase %c: %s ref=%.3f W before=%.3f W after=%.3f W pq_gain=%d->%d rollback=%d\n",
               'A' + phase, esp_err_to_name(ret), result.reference,
               result.measured_before, result.measured_after,
               result.old_pq_gain, result.new_pq_gain, result.rolled_back);
        if (ret == ESP_OK) printf("verify the result, then run meter-cal save\n");
        return ret == ESP_OK ? 0 : 1;
    }

    if (strcmp(sub, "auto-phi") == 0) {
        if (s_cal_args.phase->count == 0 || s_cal_args.value->count == 0) {
            printf("auto-phi requires --phase a|b|c --value <P_ref_W> (PF=0.5L) [--tolerance <percent>]\n");
            return 1;
        }
        int phase = parse_phase(s_cal_args.phase->sval[0]);
        float ref_w = strtof(s_cal_args.value->sval[0], NULL);
        if (phase < 0 || ref_w <= 0.0f) {
            printf("phase must be a|b|c and P_ref must be > 0\n");
            return 1;
        }
        float tolerance = (s_cal_args.tolerance->count > 0)
                        ? (float)s_cal_args.tolerance->dval[0]
                        : (float)CONFIG_APP_ATM90E32AS_CALIB_TOLERANCE_PERCENT;
        energy_meter_phase_calib_request_t request = {
            .phase = (atm90e32as_phase_t)phase,
            .reference_w = ref_w,
            .samples = 3,
            .settle_ms = 700,
            .tolerance_percent = tolerance,
        };
        energy_meter_phase_calib_result_t result;
        esp_err_t ret = energy_meter_auto_calibrate_phase(&request, &result);
        printf("auto-phi phase %c: %s ref=%.3f W before=%.3f W after=%.3f W phi=%d->%d PAngle=%.1f deg rollback=%d\n",
               'A' + phase, esp_err_to_name(ret), result.reference,
               result.measured_before, result.measured_after,
               result.old_phase_comp, result.new_phase_comp,
               result.phase_angle_after, result.rolled_back);
        if (ret == ESP_ERR_INVALID_STATE) {
            printf("phase_comp not at baseline; run: meter-cal default --field phi --phase <a|b|c>, then re-run auto-phi\n");
        }
        if (ret == ESP_OK) printf("verify the result, then run meter-cal save\n");
        return ret == ESP_OK ? 0 : 1;
    }

    if (strcmp(sub, "phi-err") == 0) {
        if (s_cal_args.phase->count == 0 || s_cal_args.error->count == 0) {
            printf("phi-err requires --phase a|b|c --error <percent> [--tolerance <percent>]\n");
            return 1;
        }
        int phase = parse_phase(s_cal_args.phase->sval[0]);
        float error_percent = (float)s_cal_args.error->dval[0];
        if (phase < 0) {
            printf("phase must be a|b|c\n");
            return 1;
        }
        float tolerance = (s_cal_args.tolerance->count > 0)
                        ? (float)s_cal_args.tolerance->dval[0]
                        : (float)CONFIG_APP_ATM90E32AS_CALIB_TOLERANCE_PERCENT;

        /* Step 1: Measure P_chip (average over 3 samples, 100ms interval) */
        int64_t p_chip_mw = 0;
        esp_err_t ret = energy_meter_get_average_active_power((atm90e32as_phase_t)phase, 3, 100, &p_chip_mw);
        if (ret != ESP_OK || p_chip_mw <= 0) {
            printf("Failed to measure P_chip or P_chip <= 0: %s\n", esp_err_to_name(ret));
            return 1;
        }

        /* Step 2: Calculate P_ref from error
         * error_percent = (P_chip - P_ref) / P_ref * 100
         * P_ref = P_chip / (1 + error_percent/100) */
        double p_ref_w = ((double)p_chip_mw / 1000.0) / (1.0 + error_percent / 100.0);

        printf("P_chip measured: %.3f W\n", (float)p_chip_mw / 1000.0f);
        printf("Known error: %.3f%%\n", error_percent);
        printf("Computed P_ref: %.3f W\n", (float)p_ref_w);

        /* Step 3: Call standard auto-phi with computed P_ref */
        energy_meter_phase_calib_request_t request = {
            .phase = (atm90e32as_phase_t)phase,
            .reference_w = (float)p_ref_w,
            .samples = 3,
            .settle_ms = 700,
            .tolerance_percent = tolerance,
        };

        energy_meter_phase_calib_result_t result;
        ret = energy_meter_auto_calibrate_phase(&request, &result);

        printf("\nPhase calibration with known error:\n");
        printf("  Phase: %c\n", 'A' + phase);
        printf("  P_ref (computed): %.3f W\n", result.reference);
        printf("  P_chip before: %.3f W\n", result.measured_before);
        printf("  P_chip after: %.3f W\n", result.measured_after);
        printf("  Old Phi: %d\n", result.old_phase_comp);
        printf("  New Phi: %d\n", result.new_phase_comp);
        printf("  Gphase: %" PRIu32 "\n", result.gphase_x1000);
        if (isfinite(result.phase_angle_after)) {
            printf("  Phase angle: %.1f deg\n", result.phase_angle_after);
        }
        printf("  Result: %s\n", ret == ESP_OK ? "PASS" : "FAIL");
        if (result.rolled_back) {
            printf("  (rolled back)\n");
        }
        if (ret == ESP_ERR_INVALID_STATE) {
            printf("phase_comp not at baseline; run: meter-cal default --field phi --phase <a|b|c>, then re-run\n");
        }
        if (ret == ESP_OK) printf("verify the result, then run meter-cal save\n");

        return (ret == ESP_OK) ? 0 : 1;
    }

    if (strcmp(sub, "get-p") == 0) {
        if (s_cal_args.phase->count == 0) {
            printf("get-p requires --phase a|b|c [--value <samples>] [--interval <ms>]\n");
            return 1;
        }
        int phase = parse_phase(s_cal_args.phase->sval[0]);
        if (phase < 0) {
            printf("phase must be a|b|c\n");
            return 1;
        }

        /* Parse samples (optional, default 3, max 50) */
        uint16_t samples = 3;
        if (s_cal_args.value->count > 0) {
            samples = (uint16_t)strtoul(s_cal_args.value->sval[0], NULL, 10);
            if (samples == 0 || samples > 50) {
                printf("Invalid samples count (1-50)\n");
                return 1;
            }
        }

        /* Parse interval (optional, default 100ms, max 1000ms) */
        uint16_t interval_ms = 100;
        if (s_cal_args.interval->count > 0) {
            interval_ms = (uint16_t)s_cal_args.interval->ival[0];
            if (interval_ms < 1 || interval_ms > 1000) {
                printf("Invalid interval (1-1000 ms)\n");
                return 1;
            }
        }

        printf("Measuring phase %c active power (%u samples, %u ms interval)...\n",
               'A' + phase, samples, interval_ms);

        int64_t average_mw = 0;
        esp_err_t ret = energy_meter_get_average_active_power((atm90e32as_phase_t)phase,
                                                              samples,
                                                              interval_ms,
                                                              &average_mw);
        if (ret != ESP_OK) {
            printf("Failed to measure: %s\n", esp_err_to_name(ret));
            return 1;
        }

        printf("Average active power: %.3f W (%.0f mW)\n",
               (float)average_mw / 1000.0f, (float)average_mw);
        return 0;
    }

    if (strcmp(sub, "auto") == 0) {
        if (s_cal_args.field->count == 0 || s_cal_args.value->count == 0) {
            printf("auto requires --field u|i --value <reference|external|offset>\n");
            printf("  --phase a|b|c|all (optional; omit = all 3 phases share one reference)\n");
            return 1;
        }
        const char *field = s_cal_args.field->sval[0];
        const char *value = s_cal_args.value->sval[0];
        bool offset = strcmp(value, "offset") == 0;
        bool current = strcmp(field, "i") == 0;
        if (strcmp(field, "u") != 0 && !current) {
            printf("field must be u or i\n");
            return 1;
        }
        /* --phase is optional: absent or "all" calibrates the three phases at once
         * against a single shared reference (one AC source + neutral on all three
         * voltage channels, or three CTs on the same load). */
        uint8_t mask;
        if (s_cal_args.phase->count == 0 ||
            strcmp(s_cal_args.phase->sval[0], "all") == 0) {
            mask = (uint8_t)ENERGY_METER_PHASE_MASK_ALL;
        } else {
            int phase = parse_phase(s_cal_args.phase->sval[0]);
            if (phase < 0) {
                printf("invalid phase (use a|b|c|all)\n");
                return 1;
            }
            mask = (uint8_t)(1u << phase);
        }
        if (!offset && strcmp(value, "external") != 0 && strtof(value, NULL) <= 0.0f) {
            printf("gain reference must be greater than zero; use --value offset for no-load offset calibration\n");
            return 1;
        }
        energy_meter_multi_calib_request_t request = {
            .phase_mask = mask,
            .current = current,
            .calibrate_offset = offset,
            .source = strcmp(value, "external") == 0 ? ENERGY_METER_CALIB_REFERENCE_EXTERNAL
                                                     : ENERGY_METER_CALIB_REFERENCE_MANUAL,
            .manual_reference = offset ? 0.0f : strtof(value, NULL),
            .samples = 20,
            .settle_ms = 200,
            .tolerance_percent = 0.2f,
        };
        energy_meter_multi_calib_result_t result;
        esp_err_t ret = energy_meter_auto_calibrate_multi(&request, &result);

        char phase_label[8];
        if (mask == (uint8_t)ENERGY_METER_PHASE_MASK_ALL) {
            snprintf(phase_label, sizeof(phase_label), "all");
        } else {
            int k = 0;
            for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
                if (mask & (1u << p)) phase_label[k++] = (char)('A' + p);
            }
            phase_label[k] = '\0';
        }

        printf("auto%s %s (phase %s) applied to chip: %s pga=x%d rollback=%d\n",
               offset ? "-offset" : "", field, phase_label,
               esp_err_to_name(ret), 1 << result.pga, result.rolled_back);
        for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
            if (!(mask & (1u << p))) continue;
            if (offset) {
                printf("  phase %c: residual_before=%.6f residual_after=%.6f offset=%d->%d\n",
                       'A' + p, result.phase[p].measured_before, result.phase[p].measured_after,
                       result.phase[p].old_offset, result.phase[p].new_offset);
            } else {
                printf("  phase %c: ref=%.6f before=%.6f after=%.6f gain=%u->%u err=%.3f%%\n",
                       'A' + p, result.phase[p].reference, result.phase[p].measured_before,
                       result.phase[p].measured_after, result.phase[p].old_gain,
                       result.phase[p].new_gain, result.phase[p].error_percent);
            }
        }
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            printf("calibration is blocked in 3P3W; switch to 3P4W first:\n");
            printf("  meter-cal set --field wiring --value 3p4w --apply\n");
        } else if (ret == ESP_ERR_INVALID_SIZE && current) {
            printf("current out of range at PGA x%d — use: meter-cal set --field pga --value 1|2|4 --apply\n",
                   1 << result.pga);
        }
        if (ret == ESP_OK) printf("verify the result, then run meter-cal save\n");
        return ret == ESP_OK ? 0 : 1;
    }

    if (strcmp(sub, "set") == 0) {
        if (s_cal_args.field->count == 0) {
            printf("set requires --field\n");
            return 1;
        }

        /* Chip-wide fields (pga|wiring|freq) do not take --phase. */
        int mode_result = 0;
        if (cal_set_mode_field(s_cal_args.field->sval[0], apply, &mode_result)) {
            return mode_result;
        }

        if (s_cal_args.phase->count == 0) {
            printf("set %s requires --phase\n", s_cal_args.field->sval[0]);
            return 1;
        }
        int phase = parse_phase(s_cal_args.phase->sval[0]);
        if (phase < 0) {
            printf("invalid phase (use a|b|c)\n");
            return 1;
        }

        atm90e32as_calib_t c;
        if (energy_meter_get_calibration(&c) != ESP_OK) {
            printf("get calibration failed\n");
            return 1;
        }
        atm90e32as_phase_calib_t *pc = &c.phase[phase];
        const char *field = s_cal_args.field->sval[0];

        if (strcmp(field, "gain") == 0 || strcmp(field, "uigain") == 0) {
            if (s_cal_args.u->count) {
                int value = s_cal_args.u->ival[0];
                if (value < 1 || value > UINT16_MAX) {
                    printf("voltage gain must be in range 1..65535\n");
                    return 1;
                }
                pc->voltage_gain = (uint16_t)value;
            }
            if (s_cal_args.i->count) {
                int value = s_cal_args.i->ival[0];
                if (value < 1 || value > UINT16_MAX) {
                    printf("current gain must be in range 1..65535\n");
                    return 1;
                }
                pc->current_gain = (uint16_t)value;
            }
        } else if (strcmp(field, "offset") == 0 || strcmp(field, "uioffset") == 0) {
            if (s_cal_args.u->count) {
                int value = s_cal_args.u->ival[0];
                if (value < INT16_MIN || value > INT16_MAX) { printf("voltage offset out of int16 range\n"); return 1; }
                pc->voltage_offset = (int16_t)value;
            }
            if (s_cal_args.i->count) {
                int value = s_cal_args.i->ival[0];
                if (value < INT16_MIN || value > INT16_MAX) { printf("current offset out of int16 range\n"); return 1; }
                pc->current_offset = (int16_t)value;
            }
        } else if (strcmp(field, "power-offset") == 0) {
            if (s_cal_args.p->count) {
                int value = s_cal_args.p->ival[0];
                if (value < INT16_MIN || value > INT16_MAX) {
                    printf("active power offset out of int16 range\n");
                    return 1;
                }
                pc->active_power_offset = (int16_t)value;
            }
            if (s_cal_args.q->count) {
                int value = s_cal_args.q->ival[0];
                if (value < INT16_MIN || value > INT16_MAX) {
                    printf("reactive power offset out of int16 range\n");
                    return 1;
                }
                pc->reactive_power_offset = (int16_t)value;
            }
        } else if (strcmp(field, "phase") == 0) {
            if (s_cal_args.phi->count) {
                int value = s_cal_args.phi->ival[0];
                if (value < -255 || value > 255) {
                    printf("phase compensation out of range (-255..255)\n");
                    return 1;
                }
                pc->phase_comp = (int16_t)value;
            }
        } else if (strcmp(field, "pq-gain") == 0) {
            if (s_cal_args.value->count == 0) {
                printf("pq-gain requires --value <n>\n");
                return 1;
            }
            char *end = NULL;
            long value = strtol(s_cal_args.value->sval[0], &end, 0);
            if (end == s_cal_args.value->sval[0] || value < INT16_MIN || value > INT16_MAX) {
                printf("pq-gain out of int16 range\n");
                return 1;
            }
            pc->pq_gain = (int16_t)value;
        } else if (strcmp(field, "fundamental-power-gain") == 0) {
            if (s_cal_args.value->count == 0) {
                printf("fundamental-power-gain requires --value <n>\n");
                return 1;
            }
            char *end = NULL;
            long value = strtol(s_cal_args.value->sval[0], &end, 0);
            if (end == s_cal_args.value->sval[0] || value < INT16_MIN || value > INT16_MAX) {
                printf("fundamental-power-gain out of int16 range\n");
                return 1;
            }
            pc->fundamental_power_gain = (int16_t)value;
        } else {
            printf("unknown field '%s' (uigain|uioffset|gain|offset|power-offset|phase|pq-gain|fundamental-power-gain)\n", field);
            return 1;
        }

        /* Per-phase calibration fields apply to the chip immediately — the operator
         * is calibrating and must see the change right away via `meter latest`.
         * --apply is accepted but is now a no-op here. Only the chip-wide mode
         * fields (pga|wiring|freq, handled by cal_set_mode_field) still require it. */
        esp_err_t ret = energy_meter_set_calibration(&c);
        if (ret == ESP_OK) {
            ret = energy_meter_apply_calibration();
        }
        printf("set %s phase %c applied to chip: %s\n", field, 'A' + phase, esp_err_to_name(ret));
        return ret == ESP_OK ? 0 : 1;
    }

    if (strcmp(sub, "guide") == 0) {
        printf(
            "ATM90E32AS calibration guide (thesis: use real references, do not fake values)\n"
            "0) Calibrate only in 3P4W (a neutral is required). Phase gains are shared across\n"
            "   wiring modes, so a 3P4W calibration is already correct for 3P3W:\n"
            "     meter-cal set --field wiring --value 3p4w --apply\n"
            "1) Bring-up defaults only verify SPI/comm, not accuracy.\n"
            "2) Voltage gain (U). One AC source + neutral on all 3 channels calibrates all 3 at once:\n"
            "     meter-cal auto --field u --value <ref_V>        (all 3 phases, shared reference)\n"
            "     meter-cal auto --field u --phase a --value <ref_V>   (single phase)\n"
            "     meter-cal auto --field u --value external       (reference from a Modbus meter)\n"
            "   Per-phase: new_gain = round(old_gain * ref / measured), applied immediately.\n"
            "3) Current gain (I). Three CTs clamped on the same load calibrate all 3 at once:\n"
            "     meter-cal auto --field i --value <ref_A>        (all 3 phases)\n"
            "     meter-cal auto --field i --phase a --value <ref_A>\n"
            "4) Offsets from measured no-load/zero conditions (all 3 or one phase):\n"
            "     meter-cal auto --field u --value offset\n"
            "     meter-cal auto --field i --value offset\n"
            "   Manual per-phase override (applies immediately, no --apply):\n"
            "     meter-cal set --field uigain   --phase a --u <n> --i <n>\n"
            "     meter-cal set --field uioffset --phase a --u <n> --i <n>\n"
            "5) Energy/phase calibration from a reference meter and known PF load:\n"
            "     Production calibrates once. Bring a field to baseline first (applies immediately):\n"
            "       meter-cal default --field phi --phase a     (Phi -> 0; keeps PQGain/gain)\n"
            "       meter-cal default --field pqgain --phase a\n"
            "     (precondition: U/I gain calibrated, current ~ Ib)\n"
            "     PQGain at PF=1 (auto):    meter-cal auto-pq-gain --phase a --value <P_ref_W> [--tolerance <percent>]\n"
            "     PQGain at PF=1 (manual):  meter-cal set --field pq-gain --phase a --value <n>\n"
            "     Phi at PF=0.5L (auto):    meter-cal auto-phi --phase a --value <P_ref_W> [--tolerance <percent>]  (requires Phi=0 baseline)\n"
            "     Phi with known error:     meter-cal phi-err --phase a --error <percent> [--tolerance <percent>]\n"
            "       (Use when reference meter unavailable; error from PF=1 baseline)\n"
            "     Phi at PF=0.5L (manual):  meter-cal set --field phase --phase a --phi <n>\n"
            "     Get power average:        meter-cal get-p --phase a [--value <samples>]\n"
            "       Returns average active power measured by chip (not reference)\n"
            "     PGainF at PF=1:           meter-cal set --field fundamental-power-gain --phase a --value <n>\n"
            "     Tolerance: optional, defaults to %d%%%% (Kconfig), overridable per command\n"
            "     Order: default baseline -> U/I gain (all 3) -> PQGain (PF=1) -> Phi (PF=0.5L) -> save\n"
            "6) Verify with: meter latest\n"
            "7) Persist after verification: meter-cal save\n"
            "   On boot it auto-loads. Force reload: meter-cal load --apply\n"
            "\n"
            "Apply semantics: per-phase calib (auto / set / default --field) writes the chip\n"
            "immediately — --apply is accepted but a no-op there. Only the chip-wide mode fields\n"
            "and the whole-image `default` (no --field) still require --apply:\n"
            "   meter-cal set --field wiring --value 3p4w|3p3w --apply  (drives MODE_SEL relay)\n"
            "   meter-cal set --field freq   --value 50|60 --apply\n"
            "   meter-cal set --field pga    --value 1|2|4 --apply      (DEV: overrides system PGA; re-cal igain after)\n"
            "   Note: product path sets PGA via LCD Current CT Apply; console is for debug.\n",
            CONFIG_APP_ATM90E32AS_CALIB_TOLERANCE_PERCENT);
        return 0;
    }

    printf("unknown cal subcommand '%s' (show|default|apply|save|load|auto|auto-pq-gain|auto-phi|auto-power-offset|set|guide)\n", sub);
    return 1;
}

/* net-cfg: view / set network configuration (STA credentials) in NVS. */
static struct {
    struct arg_str *sub;
    struct arg_str *arg;   /* second positional, e.g. on|off for "ap" */
    struct arg_str *ssid;
    struct arg_str *pass;
    struct arg_end *end;
} s_netcfg_args;

static int cmd_net_cfg(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_netcfg_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_netcfg_args.end, argv[0]);
        return 1;
    }

    const char *sub = s_netcfg_args.sub->sval[0];

    /* "ap" is a runtime action, not configuration — handle it before paying for
     * the ~1.2 KB snapshot. */
    if (strcmp(sub, "ap") == 0) {
        const char *arg = s_netcfg_args.arg->count ? s_netcfg_args.arg->sval[0] : NULL;
        if (arg == NULL || (strcmp(arg, "on") != 0 && strcmp(arg, "off") != 0)) {
            printf("ap requires on|off (e.g. net-cfg ap on)\n");
            return 1;
        }
        esp_err_t ap_ret;
        if (strcmp(arg, "on") == 0) {
            ap_ret = network_manager_start_config_portal();
            printf("config portal AP %s: %s\n", ap_ret == ESP_OK ? "started" : "start failed",
                   esp_err_to_name(ap_ret));
        } else {
            ap_ret = network_manager_stop_config_portal();
            printf("config portal AP %s: %s\n", ap_ret == ESP_OK ? "stopped" : "stop failed",
                   esp_err_to_name(ap_ret));
        }
        return ap_ret == ESP_OK ? 0 : 1;
    }

    /* Network config now reads/writes through the Configuration Manager, the
     * same source the Ethernet driver and web portal use. config_manager_t is
     * ~1.2 KB; keep it off the console task stack. */
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        printf("no memory for network config\n");
        return 1;
    }
    esp_err_t ret = config_manager_get(cfg);
    if (ret != ESP_OK) {
        printf("read network config failed: %s\n", esp_err_to_name(ret));
        free(cfg);
        return 1;
    }

    int rc = 0;

    if (strcmp(sub, "show") == 0) {
        printf("wifi_ssid=\"%s\"\n", cfg->wifi_ssid);
        printf("wifi_pass=%s\n", strlen(cfg->wifi_pass) ? "(set)" : "(empty)");
        printf("eth_dhcp=%d\n", (int)cfg->dhcp_enable);
        if (!cfg->dhcp_enable) {
            printf("static_ip=%s netmask=%s gateway=%s dns=%s\n",
                   cfg->static_ip, cfg->netmask, cfg->gateway, cfg->dns);
        }
    } else if (strcmp(sub, "sta") == 0) {
        if (s_netcfg_args.ssid->count == 0) {
            printf("sta requires --ssid (and usually --pass)\n");
            rc = 1;
        } else {
            strlcpy(cfg->wifi_ssid, s_netcfg_args.ssid->sval[0], sizeof(cfg->wifi_ssid));
            if (s_netcfg_args.pass->count) {
                strlcpy(cfg->wifi_pass, s_netcfg_args.pass->sval[0], sizeof(cfg->wifi_pass));
            }
            ret = config_manager_update(cfg);
            if (ret != ESP_OK) {
                printf("update network config failed: %s\n", esp_err_to_name(ret));
                rc = 1;
            } else {
                printf("STA credentials staged (ssid=\"%s\"). "
                       "Run 'cfg-apply wifi' to apply now, 'cfg-save' to persist.\n",
                       cfg->wifi_ssid);
            }
        }
    } else {
        printf("unknown net-cfg subcommand '%s' (show|sta|ap)\n", sub);
        rc = 1;
    }

    free(cfg);
    return rc;
}

/* mqtt-cfg: view / set the device's single MQTT broker via the Configuration
 * Manager (Feature 12A — config_manager is now the single source of truth for MQTT
 * config; this command no longer touches config_store directly).
 *
 * RAM-only: every "set"-like subcommand below calls config_manager_update()
 * and nothing else — no NVS save, no apply, no reconnect. mqtt_manager reads
 * its broker once at task start (before the network is even up), so
 * a RAM-only edit here has no live effect on a running connection either way;
 * this matches Feature 12A's "KHÔNG Apply / KHÔNG reconnect / KHÔNG Save NVS"
 * constraints exactly. TLS/custom-CA entry stays deferred (a future TLS
 * Runtime feature), same as before this migration. */
static struct {
    struct arg_str *sub;
    struct arg_str *name;
    struct arg_str *uri;
    struct arg_int *port;
    struct arg_str *user;
    struct arg_str *pass;
    struct arg_int *period;
    struct arg_str *tls;
    struct arg_end *end;
} s_mqttcfg_args;

/* mqtt-cfg set --tls <mode>. The certificate paths themselves are not options:
 * for MUTUAL they are filled in from the certificate store's fixed /flash slots,
 * which is where the Web upload API writes. A caller that really needs a custom
 * path still has dp write CFG_MQTT_CA_PATH and friends. */
static bool parse_tls_mode(const char *name, mqtt_tls_mode_t *out)
{
    if (strcmp(name, "off") == 0 || strcmp(name, "disable") == 0) {
        *out = MQTT_TLS_DISABLE;
    } else if (strcmp(name, "ca") == 0) {
        *out = MQTT_TLS_CA_ONLY;
    } else if (strcmp(name, "mutual") == 0) {
        *out = MQTT_TLS_MUTUAL;
    } else if (strcmp(name, "insecure") == 0) {
        *out = MQTT_TLS_INSECURE;
    } else {
        return false;
    }
    return true;
}

static int cmd_mqtt_cfg(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_mqttcfg_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_mqttcfg_args.end, argv[0]);
        return 1;
    }

    const char *sub = s_mqttcfg_args.sub->sval[0];

    /* config_manager_t is ~1.2 KB (one MQTT broker since the profile array went
     * away); keep it off the console task stack. */
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        printf("no memory for mqtt config\n");
        return 1;
    }
    esp_err_t ret = config_manager_get(cfg);
    if (ret != ESP_OK) {
        printf("read mqtt config failed: %s\n", esp_err_to_name(ret));
        free(cfg);
        return 1;
    }

    int rc = 0;

    if (strcmp(sub, "show") == 0) {
        const config_mqtt_profile_t *p = &cfg->mqtt;
        printf("enable=%d name=\"%s\" broker=\"%s\" port=%u keepalive=%us user=\"%s\" pass=%s tls_mode=%d\n",
               p->enable, p->name, p->broker, (unsigned)p->port, (unsigned)p->keepalive_s,
               p->username, strlen(p->password) ? "(set)" : "(empty)", (int)p->tls_mode);
        printf("publish_period=%us\n", (unsigned)(cfg->mqtt_publish_ms / 1000U));
        /* Paths only, and only whether a file is there — never any PEM
         * content, for either the certificates or the private key. */
        if (p->tls_mode != MQTT_TLS_DISABLE) {
            printf("ca=%s cert=%s key=%s\n",
                   p->ca_path[0] ? p->ca_path : "(cert bundle)",
                   p->cert_path[0] ? p->cert_path : "(none)",
                   p->key_path[0] ? p->key_path : "(none)");
        }
        if (cert_store_ready()) {
            /* The broker owns the single certificate-store index, so one line
             * covers every slot. */
            printf("cert store:");
            for (int i = 0; i < CERT_SLOT_COUNT; i++) {
                cert_slot_info_t info;
                if (cert_store_stat(0, (cert_slot_t)i, &info) != ESP_OK) {
                    continue;
                }
                printf(" %s=%s", cert_store_slot_name((cert_slot_t)i),
                       info.present ? info.fingerprint : "absent");
            }
            printf("\n");
        } else {
            printf("cert store %s: not mounted\n", CERT_STORE_MOUNT_POINT);
        }
        free(cfg);
        return 0;
    }

    if (strcmp(sub, "set") == 0) {
        config_mqtt_profile_t *p = &cfg->mqtt;
        if (s_mqttcfg_args.name->count) strlcpy(p->name, s_mqttcfg_args.name->sval[0], sizeof(p->name));
        if (s_mqttcfg_args.uri->count)  strlcpy(p->broker, s_mqttcfg_args.uri->sval[0], sizeof(p->broker));
        if (s_mqttcfg_args.port->count) p->port = (uint16_t)s_mqttcfg_args.port->ival[0];
        if (s_mqttcfg_args.user->count) strlcpy(p->username, s_mqttcfg_args.user->sval[0], sizeof(p->username));
        if (s_mqttcfg_args.pass->count) strlcpy(p->password, s_mqttcfg_args.pass->sval[0], sizeof(p->password));
        if (s_mqttcfg_args.tls->count) {
            mqtt_tls_mode_t mode;
            if (!parse_tls_mode(s_mqttcfg_args.tls->sval[0], &mode)) {
                printf("--tls must be off|ca|mutual|insecure\n");
                free(cfg);
                return 1;
            }
            p->tls_mode = mode;
            /* Point the broker at the certificate store slots — the files the
             * Web upload API writes. CA_ONLY deliberately leaves
             * ca_path empty so the built-in certificate bundle is used unless an
             * explicit CA is uploaded; if one is present on /flash, prefer it.
             * MUTUAL needs all three files. */
            char path[CERT_STORE_PATH_MAX];
            p->ca_path[0] = '\0';
            p->cert_path[0] = '\0';
            p->key_path[0] = '\0';
            if (mode == MQTT_TLS_CA_ONLY || mode == MQTT_TLS_MUTUAL) {
                cert_slot_info_t info;
                bool have_ca = cert_store_stat(0, CERT_SLOT_CA, &info) == ESP_OK && info.present;
                if (have_ca || mode == MQTT_TLS_MUTUAL) {
                    strlcpy(p->ca_path, cert_store_slot_path(0, CERT_SLOT_CA, path, sizeof(path)),
                            sizeof(p->ca_path));
                }
            }
            if (mode == MQTT_TLS_MUTUAL) {
                strlcpy(p->cert_path, cert_store_slot_path(0, CERT_SLOT_CERT, path, sizeof(path)),
                        sizeof(p->cert_path));
                strlcpy(p->key_path, cert_store_slot_path(0, CERT_SLOT_KEY, path, sizeof(path)),
                        sizeof(p->key_path));
            }
        }

        ret = config_manager_update(cfg);
        printf("broker updated: %s (RAM only, not persisted)\n", esp_err_to_name(ret));
        rc = ret == ESP_OK ? 0 : 1;
    } else if (strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) {
        /* The product path for this switch is the LCD (Settings > MQTT); the
         * console variant lets a developer bring MQTT up without the panel.
         * Both write config_mqtt_profile_t.enable, which is what mqtt_manager
         * reads — there is no separate legacy mqtt_enable field anymore. */
        cfg->mqtt.enable = (strcmp(sub, "enable") == 0);
        ret = config_manager_update(cfg);
        printf("mqtt %s: %s (RAM only, not persisted)\n",
               cfg->mqtt.enable ? "enabled" : "disabled", esp_err_to_name(ret));
        rc = ret == ESP_OK ? 0 : 1;
    } else if (strcmp(sub, "period") == 0) {
        if (s_mqttcfg_args.period->count == 0) {
            printf("period requires --period <s>\n");
            free(cfg);
            return 1;
        }
        int period_s = s_mqttcfg_args.period->ival[0];
        /* Checked here as well as in config_manager_update() so the console
         * reports the unit it actually accepts: seconds. */
        if (period_s < (int)(CONFIG_MANAGER_MQTT_PERIOD_MIN_MS / 1000U) ||
            period_s > (int)(CONFIG_MANAGER_MQTT_PERIOD_MAX_MS / 1000U)) {
            printf("period out of range (5..60 seconds)\n");
            free(cfg);
            return 1;
        }
        cfg->mqtt_publish_ms = (uint32_t)period_s * 1000U;
        ret = config_manager_update(cfg);
        printf("publish period = %us: %s (RAM only, not persisted)\n",
               (unsigned)period_s, esp_err_to_name(ret));
        rc = ret == ESP_OK ? 0 : 1;
    } else {
        printf("unknown mqtt-cfg subcommand '%s' (show|set|enable|disable|period)\n", sub);
        rc = 1;
    }

    free(cfg);
    return rc;
}

/* log: change the runtime log level of a specific TAG (or all tags with '*').
 * Raising the threshold (e.g. to warn/none) hides chatter from a noisy task;
 * showing debug/verbose only works if CONFIG_LOG_MAXIMUM_LEVEL is built high
 * enough (it is raised to DEBUG in this project). */
static struct {
    struct arg_str *tag;
    struct arg_str *level;
    struct arg_end *end;
} s_log_args;

static bool parse_log_level(const char *name, esp_log_level_t *out)
{
    if (strcmp(name, "none") == 0)         { *out = ESP_LOG_NONE;    return true; }
    if (strcmp(name, "error") == 0)        { *out = ESP_LOG_ERROR;   return true; }
    if (strcmp(name, "warn") == 0)         { *out = ESP_LOG_WARN;    return true; }
    if (strcmp(name, "info") == 0)         { *out = ESP_LOG_INFO;    return true; }
    if (strcmp(name, "debug") == 0)        { *out = ESP_LOG_DEBUG;   return true; }
    if (strcmp(name, "verbose") == 0)      { *out = ESP_LOG_VERBOSE; return true; }
    return false;
}

static int cmd_log(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_log_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_log_args.end, argv[0]);
        return 1;
    }

    const char *tag = s_log_args.tag->sval[0];
    const char *level_name = s_log_args.level->sval[0];

    esp_log_level_t level;
    if (!parse_log_level(level_name, &level)) {
        printf("invalid level '%s' (none|error|warn|info|debug|verbose)\n", level_name);
        return 1;
    }

    /* "*" applies to every tag; esp_log_level_set already treats "*" specially. */
    esp_log_level_set(tag, level);
    printf("log level of '%s' set to %s\n", tag, level_name);
    return 0;
}

/* mb-slave-log: turn the esp-modbus library's verbose DEBUG output
 * (MB_SERIAL / MB_CONTROLLER_SLAVE / MBS_TIMER) on or off. Off by
 * default — the library's per-frame "RX: N bytes" log is off-by-one and
 * floods the console. The setting is applied on every slave_stack_start(),
 * so it survives reconfigure. */
static struct {
    struct arg_str *state;
    struct arg_end *end;
} s_mb_slave_log_args;

/* mb-master-ref ... */
static struct {
    struct arg_str *sub;        /* list | read | compare */
    struct arg_int *id;         /* --id <slave_id> */
    struct arg_str *phase;      /* --phase <a|b|c> for compare */
    struct arg_int *samples;    /* --samples <N> */
    struct arg_int *interval;   /* --interval <ms> */
    struct arg_end *end;
} s_mb_ref_args;

static int cmd_mb_slave_log(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_mb_slave_log_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_mb_slave_log_args.end, argv[0]);
        return 1;
    }
    const char *state = s_mb_slave_log_args.state->sval[0];
    bool enable;
    if (strcmp(state, "on") == 0) {
        enable = true;
    } else if (strcmp(state, "off") == 0) {
        enable = false;
    } else {
        printf("expected 'on' or 'off'\n");
        return 1;
    }
    modbus_slave_set_verbose_logging(enable);
    printf("modbus slave verbose logging: %s (effective on next slave event)\n",
           enable ? "on" : "off");
    return 0;
}

/* mb-slave-diag: snapshot of the slave task's per-event counters and
 * the time since the most recent master request. Use this to confirm
 * the master is actually polling (events_holding_rd climbs), and to
 * spot queue overflow (queue_overflow_drops > 0). */
static int cmd_mb_slave_diag(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    modbus_slave_diag_t d;
    modbus_slave_get_diag(&d);
    printf("stack_up              : %u\n", (unsigned)d.stack_up);
    printf("events_holding_rd     : %u\n", (unsigned)d.events_holding_rd);
    printf("events_holding_wr     : %u\n", (unsigned)d.events_holding_wr);
    printf("events_input_rd       : %u\n", (unsigned)d.events_input_rd);
    printf("events_coils_rd       : %u\n", (unsigned)d.events_coils_rd);
    printf("events_coils_wr       : %u\n", (unsigned)d.events_coils_wr);
    printf("events_discrete_rd    : %u\n", (unsigned)d.events_discrete_rd);
    printf("queue_overflow_drops  : %u\n", (unsigned)d.queue_overflow_drops);
    if (d.time_since_last_req_ms == UINT32_MAX) {
        printf("time_since_last_req_ms: n/a (no request seen yet)\n");
    } else if (d.time_since_last_req_ms > 60000U) {
        printf("time_since_last_req_ms: >=60000 (master likely silent)\n");
    } else {
        printf("time_since_last_req_ms: %u\n", (unsigned)d.time_since_last_req_ms);
    }
    return 0;
}

/* reboot: restart the device from the console, same effect as the physical
 * reset button or the existing Modbus HR_REBOOT command. A short delay lets
 * the "rebooting..." line actually reach the terminal before the restart. */
static int cmd_reboot(int argc, char **argv)
{
    printf("rebooting...\n");
    fflush(stdout);
    energy_meter_flush_persist();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return 0; /* unreachable */
}

/* mb-master-ref: read from Modbus reference meter (list slots, read power) */
static int cmd_mb_master_ref(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_mb_ref_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_mb_ref_args.end, argv[0]);
        return 1;
    }

    if (s_mb_ref_args.sub->count == 0) {
        printf("Usage: ref <list|read> [--id <N>]\n");
        return 1;
    }

    const char *sub = s_mb_ref_args.sub->sval[0];

    if (strcmp(sub, "list") == 0) {
        /* List all configured slots with slave_id and device type */
        printf("Configured Modbus Master Slots:\n");
        printf("%-6s %-10s %-10s %-20s %-10s\n",
               "Slot", "Slave ID", "Type", "Name", "Status");
        printf("--------------------------------------------------------------\n");

        for (uint8_t slot = 0; slot < MODBUS_MASTER_SLOT_COUNT; slot++) {
            modbus_master_slot_status_t status;
            esp_err_t ret = modbus_master_get_slot_status(slot, &status);
            if (ret != ESP_OK || !status.used) {
                continue;  /* Skip unconfigured slots */
            }

            const char *type_name;
            if (status.type == METER_DEV_PM710) {
                type_name = "PM710";
            } else if (status.type == METER_DEV_EM07K) {
                type_name = "EM07K";
            } else {
                type_name = "UNKNOWN";
            }

            const char *online_str = modbus_master_dev_state_name(status.state);

            printf("%-6u %-10u %-10s %-20s %-10s\n",
                   (unsigned)slot,
                   (unsigned)status.slave_id,
                   type_name,
                   status.name,
                   online_str);
        }

        return 0;
    }
    else if (strcmp(sub, "read") == 0) {
        /* Read active power from specific slave_id, optionally average over multiple samples */
        if (s_mb_ref_args.id->count == 0) {
            printf("Error: --id <slave_id> is required\n");
            return 1;
        }

        uint8_t target_id = (uint8_t)s_mb_ref_args.id->ival[0];

        /* Parse samples (optional, default 1, max 50) */
        uint16_t samples = 1;
        if (s_mb_ref_args.samples->count > 0) {
            samples = (uint16_t)s_mb_ref_args.samples->ival[0];
            if (samples == 0 || samples > 50) {
                printf("Invalid samples count (1-50)\n");
                return 1;
            }
        }

        /* Parse interval (optional, default 100ms, max 1000ms) */
        uint16_t interval_ms = 100;
        if (s_mb_ref_args.interval->count > 0) {
            interval_ms = (uint16_t)s_mb_ref_args.interval->ival[0];
            if (interval_ms < 1 || interval_ms > 1000) {
                printf("Invalid interval (1-1000 ms)\n");
                return 1;
            }
        }

        /* Find slot with matching slave_id */
        int found_slot = -1;
        for (uint8_t slot = 0; slot < MODBUS_MASTER_SLOT_COUNT; slot++) {
            modbus_master_slot_status_t status;
            esp_err_t ret = modbus_master_get_slot_status(slot, &status);
            if (ret == ESP_OK && status.used && status.slave_id == target_id) {
                found_slot = slot;
                break;
            }
        }

        if (found_slot < 0) {
            printf("Slave ID %u not found in configured slots\n", (unsigned)target_id);
            return 1;
        }

        if (samples > 1) {
            printf("Measuring slave ID %u active power (%u samples, %u ms interval)...\n",
                   (unsigned)target_id, samples, interval_ms);
        }

        /* Read multiple samples and average */
        double sum_p = 0.0;
        for (uint16_t n = 0; n < samples; n++) {
            meter_readings_t readings;
            esp_err_t ret = modbus_master_get_readings_slot((uint8_t)found_slot, &readings);
            if (ret != ESP_OK) {
                printf("Failed to read slot %d sample %u: %s\n",
                       found_slot, n + 1, esp_err_to_name(ret));
                return 1;
            }
            sum_p += readings.active_power;

            if (n < samples - 1) {  /* Don't delay after last sample */
                vTaskDelay(pdMS_TO_TICKS(interval_ms));
            }
        }

        float avg_p = (float)(sum_p / samples);

        printf("Slave ID %u - Active Power: %.3f W", (unsigned)target_id, avg_p);
        if (samples > 1) {
            printf(" (average of %u samples)", samples);
        }
        printf("\n");

        /* Show other readings from last sample only */
        meter_readings_t last_readings;
        esp_err_t ret = modbus_master_get_readings_slot((uint8_t)found_slot, &last_readings);
        if (ret == ESP_OK) {
            printf("  Voltage: L1=%.1fV L2=%.1fV L3=%.1fV\n",
                   last_readings.voltage[0], last_readings.voltage[1], last_readings.voltage[2]);
            printf("  Current: L1=%.2fA L2=%.2fA L3=%.2fA\n",
                   last_readings.current[0], last_readings.current[1], last_readings.current[2]);
            printf("  Reactive: %.3f var\n", last_readings.reactive_power);
            printf("  Apparent: %.3f VA\n", last_readings.apparent_power);
            printf("  PF: %.3f\n", last_readings.power_factor);
            printf("  Frequency: %.2f Hz\n", last_readings.frequency);
        }

        return 0;
    }
    else if (strcmp(sub, "compare") == 0) {
        /* Compare P_ref from reference meter with P_meter from DUT, calculate error */
        if (s_mb_ref_args.id->count == 0 || s_mb_ref_args.phase->count == 0) {
            printf("Error: compare requires --id <slave_id> --phase <a|b|c> [--samples <N>] [--interval <ms>]\n");
            return 1;
        }

        uint8_t target_id = (uint8_t)s_mb_ref_args.id->ival[0];
        int phase = parse_phase(s_mb_ref_args.phase->sval[0]);
        if (phase < 0) {
            printf("phase must be a|b|c\n");
            return 1;
        }

        /* Parse samples (optional, default 5, max 50) */
        uint16_t samples = 5;
        if (s_mb_ref_args.samples->count > 0) {
            samples = (uint16_t)s_mb_ref_args.samples->ival[0];
            if (samples == 0 || samples > 50) {
                printf("Invalid samples count (1-50)\n");
                return 1;
            }
        }

        /* Parse interval (optional, default 200ms, max 1000ms) */
        uint16_t interval_ms = 200;
        if (s_mb_ref_args.interval->count > 0) {
            interval_ms = (uint16_t)s_mb_ref_args.interval->ival[0];
            if (interval_ms < 1 || interval_ms > 1000) {
                printf("Invalid interval (1-1000 ms)\n");
                return 1;
            }
        }

        /* Find reference meter slot */
        int found_slot = -1;
        for (uint8_t slot = 0; slot < MODBUS_MASTER_SLOT_COUNT; slot++) {
            modbus_master_slot_status_t status;
            esp_err_t ret = modbus_master_get_slot_status(slot, &status);
            if (ret == ESP_OK && status.used && status.slave_id == target_id) {
                found_slot = slot;
                break;
            }
        }

        if (found_slot < 0) {
            printf("Slave ID %u not found in configured slots\n", (unsigned)target_id);
            return 1;
        }

        printf("Comparing phase %c: DUT vs Reference meter (slave ID %u)\n",
               'A' + phase, (unsigned)target_id);
        printf("Sampling: %u samples, %u ms interval\n", samples, interval_ms);
        printf("--------------------------------------------------\n");

        /* Read samples alternately: P_ref then P_meter for each iteration */
        double sum_p_ref = 0.0;
        double sum_p_meter = 0.0;

        for (uint16_t n = 0; n < samples; n++) {
            /* Step 1: Read P_ref from reference meter */
            meter_readings_t ref_readings;
            esp_err_t ret = modbus_master_get_readings_slot((uint8_t)found_slot, &ref_readings);
            if (ret != ESP_OK) {
                printf("Failed to read reference meter sample %u: %s\n",
                       n + 1, esp_err_to_name(ret));
                return 1;
            }
            float p_ref = ref_readings.active_power;
            sum_p_ref += p_ref;

            /* Step 2: Immediately read P_meter from DUT chip */
            atm90e32as_measurements_t meter_meas;
            ret = energy_meter_get_latest(&meter_meas);
            if (ret != ESP_OK) {
                printf("Failed to read DUT meter sample %u: %s\n",
                       n + 1, esp_err_to_name(ret));
                return 1;
            }
            float p_meter = meter_meas.active_power[phase];
            sum_p_meter += p_meter;

            printf("Sample %2u: P_ref=%.3f W, P_meter=%.3f W\n",
                   n + 1, p_ref, p_meter);

            /* Wait before next sample (except after last) */
            if (n < samples - 1) {
                vTaskDelay(pdMS_TO_TICKS(interval_ms));
            }
        }

        /* Calculate averages and error */
        float avg_p_ref = (float)(sum_p_ref / samples);
        float avg_p_meter = (float)(sum_p_meter / samples);
        float error_percent = 0.0f;

        if (avg_p_ref > 0.001f) {  /* Avoid division by zero */
            error_percent = ((avg_p_meter - avg_p_ref) / avg_p_ref) * 100.0f;
        }

        printf("--------------------------------------------------\n");
        printf("Results:\n");
        printf("  P_ref average:   %.3f W\n", avg_p_ref);
        printf("  P_meter average: %.3f W\n", avg_p_meter);
        printf("  Power error:     %.3f%%\n", error_percent);
        printf("\nUse this error value for phase calibration:\n");
        printf("  meter-cal phi-err --phase %c --error %.3f\n", 'a' + phase, error_percent);

        return 0;
    }
    else {
        printf("Unknown subcommand: %s\n", sub);
        return 1;
    }
}

/* ping: ICMP echo to a numeric IPv4 target via esp_ping (lwip app). Runs one
 * bounded session (default 4 packets) and blocks the REPL until esp_ping's
 * on_ping_end callback fires, so the summary always prints after the last
 * packet. No hostname resolution — pass an address; the network path (ETH/STA)
 * must be up or every request times out, which the statistics still report
 * honestly. Callbacks run in esp_ping's own task, so their state lives in these
 * file-scope variables rather than in a stack struct. */
static struct {
    struct arg_str *host;
    struct arg_int *count;
    struct arg_int *interval;
    struct arg_int *timeout;
    struct arg_end *end;
} s_ping_args;

#define PING_HOST_MAX 46 /* "IPv6 string + %scope" fits; IPv4 needs 15 */

static volatile bool s_ping_ended;
static uint32_t s_ping_sent;
static uint32_t s_ping_recv;
static uint32_t s_ping_rtt_min;
static uint32_t s_ping_rtt_max;
static uint64_t s_ping_rtt_sum;
static char s_ping_host[PING_HOST_MAX];

static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    uint16_t seqno = 0;
    uint8_t ttl = 0;
    uint32_t size = 0;
    uint32_t elapsed_ms = 0;
    ip_addr_t raddr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SIZE, &size, sizeof(size));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &raddr, sizeof(raddr));
    if (elapsed_ms < s_ping_rtt_min) {
        s_ping_rtt_min = elapsed_ms;
    }
    if (elapsed_ms > s_ping_rtt_max) {
        s_ping_rtt_max = elapsed_ms;
    }
    s_ping_rtt_sum += elapsed_ms;
    printf("%u bytes from %s: icmp_seq=%u ttl=%u time=%u ms\n",
           (unsigned)size, ipaddr_ntoa(&raddr), (unsigned)seqno, (unsigned)ttl,
           (unsigned)elapsed_ms);
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args)
{
    (void)hdl; (void)args;
    printf("ping: no response — icmp echo request timed out\n");
}

static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    (void)args;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &s_ping_sent, sizeof(s_ping_sent));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &s_ping_recv, sizeof(s_ping_recv));
    printf("--- %s ping statistics ---\n", s_ping_host);
    printf("%u packets transmitted, %u received, %u%% packet loss\n",
           (unsigned)s_ping_sent, (unsigned)s_ping_recv,
           s_ping_sent ? (unsigned)((s_ping_sent - s_ping_recv) * 100U / s_ping_sent) : 0U);
    if (s_ping_recv > 0U) {
        printf("rtt min/avg/max = %u/%u/%u ms\n",
               (unsigned)s_ping_rtt_min, (unsigned)(s_ping_rtt_sum / s_ping_recv),
               (unsigned)s_ping_rtt_max);
    }
    s_ping_ended = true;
}

static int cmd_ping(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_ping_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_ping_args.end, argv[0]);
        return 1;
    }

    const char *host = s_ping_args.host->sval[0];
    uint32_t count = s_ping_args.count->count ? (uint32_t)s_ping_args.count->ival[0] : 4U;
    uint32_t period = s_ping_args.interval->count ? (uint32_t)s_ping_args.interval->ival[0] : 1000U;
    uint32_t timeout = s_ping_args.timeout->count ? (uint32_t)s_ping_args.timeout->ival[0] : 2000U;
    if (count == 0U || count > 100U || period < 100U || timeout < 100U) {
        printf("ping: count must be 1..100, interval/timeout >= 100ms\n");
        return 1;
    }

    ip_addr_t target;
    if (!ipaddr_aton(host, &target)) {
        printf("ping: '%s' is not a numeric IP address (no DNS here)\n", host);
        return 1;
    }

    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    config.target_addr = target;
    config.count = count;
    config.interval_ms = period;
    config.timeout_ms = timeout;

    const esp_ping_callbacks_t cbs = {
        .cb_args = NULL,
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end = ping_on_end,
    };
    esp_ping_handle_t hdl = NULL;
    esp_err_t ret = esp_ping_new_session(&config, &cbs, &hdl);
    if (ret != ESP_OK) {
        printf("ping session setup failed: %s\n", esp_err_to_name(ret));
        return 1;
    }

    s_ping_ended = false;
    s_ping_sent = 0;
    s_ping_recv = 0;
    s_ping_rtt_min = UINT32_MAX; /* first reply sets it; only printed if recv > 0 */
    s_ping_rtt_max = 0;
    s_ping_rtt_sum = 0;
    strlcpy(s_ping_host, host, sizeof(s_ping_host));
    printf("PING %s %u data bytes, interval=%ums timeout=%ums\n",
           s_ping_host, (unsigned)config.data_size, (unsigned)period, (unsigned)timeout);
    ret = esp_ping_start(hdl);
    if (ret != ESP_OK) {
        printf("ping start failed: %s\n", esp_err_to_name(ret));
        (void)esp_ping_delete_session(hdl);
        return 1;
    }

    /* Each procedure can burn the full reply timeout plus one interval, hence
     * count*(period+timeout); the slack absorbs task start-up and scheduling.
     * A timeout here only guards against esp_ping itself stalling — an
     * unreachable host still ends normally with 100% loss. */
    const uint32_t guard_ms = count * (period + timeout) + 3000U;
    for (uint32_t waited = 0; waited < guard_ms && !s_ping_ended; waited += 50U) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!s_ping_ended) {
        printf("ping: session did not end in time; stopping\n");
        (void)esp_ping_stop(hdl);
        /* stop() only clears a flag; the task notices at the top of its next
         * loop, so give it one procedure's worth of grace to print the
         * summary before the session is torn down. */
        for (uint32_t waited = 0; waited < period + timeout + 500U && !s_ping_ended; waited += 50U) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    (void)esp_ping_delete_session(hdl);
    return s_ping_recv > 0U ? 0 : 1;
}

/* cfg-save / cfg-reset: explicit persistence operations. Neither command applies
 * the resulting RAM snapshot to drivers or restarts the device. */
static int cmd_cfg_save(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_err_t ret = config_manager_save();
    printf("config_manager_save() -> %s\n", esp_err_to_name(ret));
    return ret == ESP_OK ? 0 : 1;
}

static struct {
    struct arg_str *confirmation;
    struct arg_end *end;
} s_cfgreset_args;

static int cmd_cfg_reset(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_cfgreset_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_cfgreset_args.end, argv[0]);
        return 1;
    }
    if (strcmp(s_cfgreset_args.confirmation->sval[0], "confirm") != 0) {
        printf("factory reset refused; use: cfg-reset confirm\n");
        return 1;
    }

    esp_err_t ret = config_manager_factory_reset();
    printf("config_manager_factory_reset() -> %s\n", esp_err_to_name(ret));
    if (ret != ESP_OK) {
        return 1;
    }

    /* The RAM snapshot is back to defaults, but the runtime modules still hold
     * the old configuration. Reboot so the device actually comes up on the
     * default configuration. */
    printf("factory reset done; rebooting to run on default configuration...\n");
    fflush(stdout);
    /* Energy survives a factory reset by design (it is the meter reading, not a
     * setting), so commit it before the restart. */
    energy_meter_flush_persist();
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return 0;
}

/* cfg-apply: manually invoke the Configuration Apply Engine. MQTT is applied
 * at runtime; the remaining domain handlers currently only log. */
static struct {
    struct arg_str *domain; /* all|ethernet|wifi|mqtt|modbus|lcd (default: all) */
    struct arg_end *end;
} s_cfgapply_args;

static int cmd_cfg_apply(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_cfgapply_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_cfgapply_args.end, argv[0]);
        return 1;
    }

    config_apply_flags_t flags = CONFIG_APPLY_ALL;
    if (s_cfgapply_args.domain->count != 0) {
        const char *d = s_cfgapply_args.domain->sval[0];
        if (strcasecmp(d, "all") == 0)          { flags = CONFIG_APPLY_ALL; }
        else if (strcasecmp(d, "ethernet") == 0) { flags = CONFIG_APPLY_ETHERNET; }
        else if (strcasecmp(d, "wifi") == 0)     { flags = CONFIG_APPLY_WIFI; }
        else if (strcasecmp(d, "mqtt") == 0)     { flags = CONFIG_APPLY_MQTT; }
        else if (strcasecmp(d, "modbus") == 0)   { flags = CONFIG_APPLY_MODBUS; }
        else if (strcasecmp(d, "modbus-master") == 0) { flags = CONFIG_APPLY_MODBUS_MASTER; }
        else if (strcasecmp(d, "modbus-slave") == 0)  { flags = CONFIG_APPLY_MODBUS_SLAVE; }
        else if (strcasecmp(d, "lcd") == 0)      { flags = CONFIG_APPLY_LCD; }
        else {
            printf("unknown domain '%s' (all|ethernet|wifi|mqtt|modbus|modbus-master|modbus-slave|lcd)\n", d);
            return 1;
        }
    }

    esp_err_t ret = config_apply(flags);
    printf("config_apply(%s) -> %s\n", config_apply_flag_name(flags), esp_err_to_name(ret));
    return (ret == ESP_OK) ? 0 : 1;
}

/* ioexp: re-run the PCF8574 / W5500-reset diagnostics without reflashing. The
 * boot-time block in ethernet_driver only prints once, and a failing boot is the
 * one you cannot pause; these subcommands regenerate the same measurements on
 * demand and let "ioexp write 0 0" hold RESETn low while a probe is attached. */
static struct {
    struct arg_str *sub;    /* diag|scan|read|write|reset */
    struct arg_int *pin;    /* 0..7 (write) */
    struct arg_int *level;  /* 0|1 (write) */
    struct arg_end *end;
} s_ioexp_args;

static int cmd_ioexp(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_ioexp_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_ioexp_args.end, argv[0]);
        return 1;
    }

    const char *sub = s_ioexp_args.sub->sval[0];

    if (strcasecmp(sub, "diag") == 0) {
        esp_err_t ret = io_expander_diag_dump();
        printf("io_expander_diag_dump() -> %s\n", esp_err_to_name(ret));
        return (ret == ESP_OK) ? 0 : 1;
    }

    if (strcasecmp(sub, "scan") == 0) {
        uint8_t addrs[16];
        size_t found = 0;
        esp_err_t ret = i2c_bus_scan(addrs, sizeof(addrs), &found);
        if (ret != ESP_OK) {
            printf("i2c_bus_scan() -> %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("found %u device(s):", (unsigned)found);
        for (size_t i = 0; i < found && i < sizeof(addrs); i++) {
            printf(" 0x%02X", addrs[i]);
        }
        printf("\n");
        return 0;
    }

    if (strcasecmp(sub, "read") == 0) {
        uint8_t port = 0;
        esp_err_t ret = io_expander_read_port(&port);
        if (ret != ESP_OK) {
            printf("io_expander_read_port() -> %s\n", esp_err_to_name(ret));
            return 1;
        }
        printf("port(hw)=0x%02X\n", port);
        return 0;
    }

    if (strcasecmp(sub, "write") == 0) {
        if (s_ioexp_args.pin->count == 0 || s_ioexp_args.level->count == 0) {
            printf("usage: ioexp write <pin 0..7> <0|1>\n");
            return 1;
        }
        int pin = s_ioexp_args.pin->ival[0];
        int level = s_ioexp_args.level->ival[0];
        if (pin < 0 || pin > 7 || (level != 0 && level != 1)) {
            printf("pin must be 0..7 and level 0 or 1\n");
            return 1;
        }
        uint8_t port = 0;
        esp_err_t ret = io_expander_write_pin_raw((uint8_t)pin, level != 0, &port);
        printf("write P%d -> %d : %s  port(hw)=0x%02X\n", pin, level, esp_err_to_name(ret), port);
        return (ret == ESP_OK) ? 0 : 1;
    }

    if (strcasecmp(sub, "reset") == 0) {
        esp_err_t ret = io_expander_w5500_reset_pulse();
        printf("io_expander_w5500_reset_pulse() -> %s\n", esp_err_to_name(ret));
        return (ret == ESP_OK) ? 0 : 1;
    }

    printf("unknown subcommand '%s' (diag|scan|read|write|reset)\n", sub);
    return 1;
}

/* ext-meter: RTU master bus + multi-slot device table. */
static struct {
    struct arg_str *sub;      /* show|bus|add|set|del|enable|disable */
    struct arg_int *slot;     /* slot index 0..N-1 */
    struct arg_str *dev;      /* pm710|em07k */
    struct arg_int *addr;     /* slave address */
    struct arg_str *name;     /* slot label */
    struct arg_int *baud;     /* baud code 0..4 (bus) */
    struct arg_int *parity;   /* 0=none,1=even,2=odd (bus) */
    struct arg_int *period;   /* poll period ms (bus) */
    struct arg_end *end;
} s_extmeter_args;

static const char *ext_meter_baud_str(uint8_t code)
{
    switch (code) {
    case 1: return "19200";
    case 2: return "38400";
    case 3: return "57600";
    case 4: return "115200";
    default: return "9600";
    }
}

static const char *ext_meter_parity_str(uint8_t code)
{
    switch (code) {
    case 1: return "even";
    case 2: return "odd";
    default: return "none";
    }
}

/* (removed) ext_meter_sync_legacy_mirrors: mirrored slot[0] into mb_device AND
 * mb_slave_id. The mb_device mirror now runs inside config_manager_update() for
 * every writer, and mb_slave_id is this device's own RTU slave address — a
 * master-side console command must never touch it. */

static int cmd_ext_meter(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_extmeter_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_extmeter_args.end, argv[0]);
        return 1;
    }

    const char *sub = s_extmeter_args.sub->sval[0];

    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        printf("out of memory\n");
        return 1;
    }
    esp_err_t ret = config_manager_get(cfg);
    if (ret != ESP_OK) {
        printf("read ext-meter config failed: %s\n", esp_err_to_name(ret));
        free(cfg);
        return 1;
    }

    if (strcmp(sub, "show") == 0) {
        printf("bus enabled=%d baud=%s parity=%s period_ms=%lu\n",
               cfg->mb_enabled, ext_meter_baud_str(cfg->mb_baud_code),
               ext_meter_parity_str(cfg->mb_parity_code),
               (unsigned long)cfg->mb_poll_period_ms);
        for (unsigned i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
            const config_mb_slot_t *s = &cfg->mb_slots[i];
            if (!s->used) {
                continue;
            }
            modbus_master_slot_status_t st;
            const char *on = "?";
            if (modbus_master_get_slot_status((uint8_t)i, &st) == ESP_OK) {
                on = modbus_master_dev_state_name(st.state);
            }
            printf("  slot%u name=%s type=%s id=%u en=%d %s\n",
                   i, s->name[0] ? s->name : "-",
                   modbus_meters_device_name((meter_device_t)s->type),
                   (unsigned)s->slave_id, (int)s->enabled, on);
            meter_readings_t r;
            if (modbus_master_get_readings_slot((uint8_t)i, &r) == ESP_OK) {
                printf("    V=[%.1f %.1f %.1f] I=[%.3f %.3f %.3f] P=%.1fW F=%.2f E=%.3fkWh\n",
                       r.voltage[0], r.voltage[1], r.voltage[2],
                       r.current[0], r.current[1], r.current[2],
                       r.active_power, r.frequency, r.active_energy);
            }
        }
        free(cfg);
        return 0;
    }

    if (strcmp(sub, "bus") == 0) {
        if (s_extmeter_args.baud->count) {
            cfg->mb_baud_code = (uint8_t)s_extmeter_args.baud->ival[0];
        }
        if (s_extmeter_args.parity->count) {
            cfg->mb_parity_code = (uint8_t)s_extmeter_args.parity->ival[0];
        }
        if (s_extmeter_args.period->count) {
            cfg->mb_poll_period_ms = (uint32_t)s_extmeter_args.period->ival[0];
        }
        printf("bus params staged\n");
    } else if (strcmp(sub, "add") == 0 || strcmp(sub, "set") == 0) {
        int slot = -1;
        if (s_extmeter_args.slot->count) {
            slot = s_extmeter_args.slot->ival[0];
        } else if (strcmp(sub, "add") == 0) {
            for (unsigned i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
                if (!cfg->mb_slots[i].used) {
                    slot = (int)i;
                    break;
                }
            }
            if (slot < 0) {
                printf("no free slot (max %u)\n", (unsigned)CONFIG_MANAGER_MB_SLOT_COUNT);
                free(cfg);
                return 1;
            }
        } else {
            printf("set requires --slot <n>\n");
            free(cfg);
            return 1;
        }
        if (slot < 0 || slot >= (int)CONFIG_MANAGER_MB_SLOT_COUNT) {
            printf("invalid slot\n");
            free(cfg);
            return 1;
        }
        config_mb_slot_t *s = &cfg->mb_slots[slot];
        s->used = true;
        if (s_extmeter_args.dev->count) {
            const char *dev = s_extmeter_args.dev->sval[0];
            if (strcmp(dev, "pm710") == 0) {
                s->type = METER_DEV_PM710;
            } else if (strcmp(dev, "em07k") == 0) {
                s->type = METER_DEV_EM07K;
            } else {
                printf("unknown device '%s' (pm710|em07k)\n", dev);
                free(cfg);
                return 1;
            }
        } else if (strcmp(sub, "add") == 0) {
            s->type = METER_DEV_PM710;
        }
        if (s_extmeter_args.addr->count) {
            int a = s_extmeter_args.addr->ival[0];
            if (a < 1 || a > 247) {
                printf("slave id must be 1..247\n");
                free(cfg);
                return 1;
            }
            s->slave_id = (uint8_t)a;
        } else if (strcmp(sub, "add") == 0) {
            s->slave_id = 1;
        }
        if (s_extmeter_args.name->count) {
            strlcpy(s->name, s_extmeter_args.name->sval[0], sizeof(s->name));
        } else if (strcmp(sub, "add") == 0 && s->name[0] == '\0') {
            snprintf(s->name, sizeof(s->name), "M%d", slot);
        }
        if (strcmp(sub, "add") == 0) {
            s->enabled = true;
        }
        /* Reject duplicate slave ids among used slots. */
        for (unsigned i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
            if ((int)i == slot || !cfg->mb_slots[i].used) {
                continue;
            }
            if (cfg->mb_slots[i].slave_id == s->slave_id) {
                printf("slave id %u already used by slot %u\n",
                       (unsigned)s->slave_id, i);
                free(cfg);
                return 1;
            }
        }
        /* config_manager_update() below refreshes the mb_device legacy mirror;
         * this device's own mb_slave_id is never touched by master-side edits. */
        printf("slot %d staged: name=%s type=%s id=%u en=%d\n",
               slot, s->name, modbus_meters_device_name((meter_device_t)s->type),
               (unsigned)s->slave_id, (int)s->enabled);
    } else if (strcmp(sub, "del") == 0) {
        if (!s_extmeter_args.slot->count) {
            printf("del requires --slot <n>\n");
            free(cfg);
            return 1;
        }
        int slot = s_extmeter_args.slot->ival[0];
        if (slot < 0 || slot >= (int)CONFIG_MANAGER_MB_SLOT_COUNT) {
            printf("invalid slot\n");
            free(cfg);
            return 1;
        }
        memset(&cfg->mb_slots[slot], 0, sizeof(cfg->mb_slots[slot]));
        printf("slot %d cleared\n", slot);
    } else if (strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) {
        bool on = (strcmp(sub, "enable") == 0);
        if (s_extmeter_args.slot->count) {
            int slot = s_extmeter_args.slot->ival[0];
            if (slot < 0 || slot >= (int)CONFIG_MANAGER_MB_SLOT_COUNT ||
                !cfg->mb_slots[slot].used) {
                printf("invalid/unused slot\n");
                free(cfg);
                return 1;
            }
            cfg->mb_slots[slot].enabled = on;
            printf("slot %d enabled=%d\n", slot, (int)on);
        } else {
            cfg->mb_enabled = on;
            printf("bus enabled=%d\n", (int)on);
        }
    } else {
        printf("unknown subcommand '%s' (show|bus|add|set|del|enable|disable)\n", sub);
        free(cfg);
        return 1;
    }

    ret = config_manager_update(cfg);
    free(cfg);
    if (ret != ESP_OK) {
        printf("update config failed: %s\n", esp_err_to_name(ret));
        return 1;
    }

    ret = config_apply(CONFIG_APPLY_MODBUS);
    if (ret != ESP_OK) {
        printf("apply failed: %s\n", esp_err_to_name(ret));
        return 1;
    }
    printf("live-applied. Run 'cfg-save' to persist.\n");
    return 0;
}

#if CONFIG_APP_DP_DEBUG
/* ---- Data Point Layer test harness (temporary) ----
 *
 * Calls data_point_read()/data_point_write() straight from the console so the
 * CFG_* mapping can be exercised before any protocol is wired to the Data Point
 * Layer. Test scaffolding, not a product feature: gated on CONFIG_APP_DP_DEBUG,
 * and it prints a readable secret field (CFG_WIFI_PASS) in cleartext — the MQTT
 * password and cert paths are write-only, so "dp read" cannot expose those.
 */
typedef enum {
    DP_KIND_STR,
    DP_KIND_U8,
    DP_KIND_U16,
    DP_KIND_U32,
    DP_KIND_U64,
    DP_KIND_BOOL,
    DP_KIND_F32,
} dp_kind_t;

typedef struct {
    const char *name;
    data_point_id_t id;
    dp_kind_t kind;
    size_t size;        /* the size data_point_read/write expects */
} dp_entry_t;

static const dp_entry_t s_dp_table[] = {
    /* Configuration: the whole CFG_ group. */
    { "CFG_VERSION",             CFG_VERSION,             DP_KIND_U32,  4 },
    { "CFG_DEVICE_NAME",         CFG_DEVICE_NAME,         DP_KIND_STR,  CONFIG_MANAGER_DEVICE_NAME_LEN },
    { "CFG_FIRMWARE_VERSION",    CFG_FIRMWARE_VERSION,    DP_KIND_STR,  CONFIG_MANAGER_VERSION_LEN },
    { "CFG_HARDWARE_VERSION",    CFG_HARDWARE_VERSION,    DP_KIND_STR,  CONFIG_MANAGER_VERSION_LEN },
    { "CFG_DHCP_ENABLE",         CFG_DHCP_ENABLE,         DP_KIND_BOOL, sizeof(bool) },
    { "CFG_STATIC_IP",           CFG_STATIC_IP,           DP_KIND_STR,  CONFIG_MANAGER_IP_LEN },
    { "CFG_GATEWAY",             CFG_GATEWAY,             DP_KIND_STR,  CONFIG_MANAGER_IP_LEN },
    { "CFG_NETMASK",             CFG_NETMASK,             DP_KIND_STR,  CONFIG_MANAGER_IP_LEN },
    { "CFG_DNS",                 CFG_DNS,                 DP_KIND_STR,  CONFIG_MANAGER_IP_LEN },
    { "CFG_WIFI_SSID",           CFG_WIFI_SSID,           DP_KIND_STR,  CONFIG_MANAGER_SSID_LEN },
    { "CFG_WIFI_PASS",           CFG_WIFI_PASS,           DP_KIND_STR,  CONFIG_MANAGER_PASS_LEN },
    { "CFG_MQTT_PUBLISH_MS",     CFG_MQTT_PUBLISH_MS,     DP_KIND_U32,  4 },  /* 1000..60000 */
    { "CFG_MB_SLAVE_ID",         CFG_MB_SLAVE_ID,         DP_KIND_U8,   1 },  /* this device's own slave addr (LCD-owned) */
    { "CFG_MB_BAUD_CODE",        CFG_MB_BAUD_CODE,        DP_KIND_U8,   1 },  /* master bus only */
    { "CFG_MB_PARITY_CODE",      CFG_MB_PARITY_CODE,      DP_KIND_U8,   1 },  /* master bus only */
    { "CFG_MB_STOP_BITS",        CFG_MB_STOP_BITS,        DP_KIND_U8,   1 },
    { "CFG_LINE_FREQ",           CFG_LINE_FREQ,           DP_KIND_U8,   1 },
    { "CFG_WIRING_MODE",         CFG_WIRING_MODE,         DP_KIND_U8,   1 },
    { "CFG_CT_RATIO",            CFG_CT_RATIO,            DP_KIND_U16,  2 },
    { "CFG_PT_RATIO",            CFG_PT_RATIO,            DP_KIND_U16,  2 },
    { "CFG_LCD_BACKLIGHT",       CFG_LCD_BACKLIGHT,       DP_KIND_BOOL, sizeof(bool) },
    { "CFG_LCD_SLEEP_TIMEOUT_S", CFG_LCD_SLEEP_TIMEOUT_S, DP_KIND_U32,  4 },
    { "CFG_BUZZER_ENABLE",       CFG_BUZZER_ENABLE,       DP_KIND_BOOL, sizeof(bool) },
    { "CFG_MB_SLAVE_BAUD",       CFG_MB_SLAVE_BAUD,       DP_KIND_U8,   1 },  /* slave link baud (LCD-owned) */
    /* The device's single MQTT broker (cfg.mqtt).
     *
     * There is no profile selector: these ids address the one broker directly.
     * ENABLE is the flag the MQTT runtime gates on and the LCD Settings > MQTT
     * toggle writes.
     *
     * PASSWORD / CA_PATH / CERT_PATH / KEY_PATH are write-only by design — a
     * "dp read" on those returns ESP_ERR_NOT_SUPPORTED and that is a pass, not a
     * harness bug. They are listed anyway so the refusal itself is testable.
     *
     * Sizes come from the same config_mqtt_profile_t field widths the Data Point
     * Layer marshals against; a mismatch here shows up as ESP_ERR_INVALID_SIZE
     * rather than a bad write. TLS_MODE is a 1-byte wire enum
     * (0=DISABLE 1=CA_ONLY 2=MUTUAL 3=INSECURE). */
    { "CFG_MQTT_ENABLE",         CFG_MQTT_ENABLE,         DP_KIND_BOOL, sizeof(bool) },
    { "CFG_MQTT_BROKER",         CFG_MQTT_BROKER,         DP_KIND_STR,  CONFIG_MANAGER_MQTT_BROKER_LEN },
    { "CFG_MQTT_PORT",           CFG_MQTT_PORT,           DP_KIND_U16,  2 },
    { "CFG_MQTT_USERNAME",       CFG_MQTT_USERNAME,       DP_KIND_STR,  CONFIG_MANAGER_MQTT_USER_LEN },
    { "CFG_MQTT_PASSWORD",       CFG_MQTT_PASSWORD,       DP_KIND_STR,  CONFIG_MANAGER_MQTT_PASS_LEN },
    { "CFG_MQTT_CLIENT_ID",      CFG_MQTT_CLIENT_ID,      DP_KIND_STR,  CONFIG_MANAGER_MQTT_CLIENT_ID_LEN },
    { "CFG_MQTT_PUBLISH_TOPIC",  CFG_MQTT_PUBLISH_TOPIC,  DP_KIND_STR,  CONFIG_MANAGER_MQTT_TOPIC_LEN },
    { "CFG_MQTT_SUBSCRIBE_TOPIC", CFG_MQTT_SUBSCRIBE_TOPIC, DP_KIND_STR, CONFIG_MANAGER_MQTT_TOPIC_LEN },
    { "CFG_MQTT_TLS_MODE",       CFG_MQTT_TLS_MODE,       DP_KIND_U8,   1 },
    { "CFG_MQTT_CA_PATH",        CFG_MQTT_CA_PATH,        DP_KIND_STR,  CONFIG_MANAGER_MQTT_PATH_LEN },
    { "CFG_MQTT_CERT_PATH",      CFG_MQTT_CERT_PATH,      DP_KIND_STR,  CONFIG_MANAGER_MQTT_PATH_LEN },
    { "CFG_MQTT_KEY_PATH",       CFG_MQTT_KEY_PATH,       DP_KIND_STR,  CONFIG_MANAGER_MQTT_PATH_LEN },
    /* A few non-CFG points, for the regression checks. */
    { "MEAS_VOLTAGE_L1",         MEAS_VOLTAGE_L1,         DP_KIND_F32,  4 },
    { "MEAS_VALID",              MEAS_VALID,              DP_KIND_U8,   1 },
    { "MEAS_LAST_UPDATE_US",     MEAS_LAST_UPDATE_US,     DP_KIND_U64,  8 },
    { "SYS_MQTT_STATUS",         SYS_MQTT_STATUS,         DP_KIND_U8,   1 },
    { "DI_INPUT0_STATE",         DI_INPUT0_STATE,         DP_KIND_U8,   1 },
    { "DO_RELAY0_STATE",         DO_RELAY0_STATE,         DP_KIND_U8,   1 },
};

#define DP_TABLE_COUNT (sizeof(s_dp_table) / sizeof(s_dp_table[0]))

static const dp_entry_t *dp_lookup(const char *name)
{
    for (size_t i = 0; i < DP_TABLE_COUNT; i++) {
        if (strcasecmp(name, s_dp_table[i].name) == 0) {
            return &s_dp_table[i];
        }
    }
    return NULL;
}

static void dp_print_value(const dp_entry_t *e, const uint8_t *buf, size_t size)
{
    switch (e->kind) {
    case DP_KIND_STR: {
        /* Report where the terminator landed: an unterminated string in the
         * snapshot is exactly what this harness is here to catch. */
        size_t len = strnlen((const char *)buf, size);
        printf("  value = \"%.*s\"  strlen=%u field=%u  %s\n",
               (int)len, (const char *)buf, (unsigned)len, (unsigned)size,
               len < size ? "NUL-terminated" : "NOT NUL-terminated");
        break;
    }
    case DP_KIND_BOOL:
        printf("  value = %u (bool)\n", (unsigned)buf[0]);
        break;
    case DP_KIND_U8:
        printf("  value = %u\n", (unsigned)buf[0]);
        break;
    case DP_KIND_U16: {
        uint16_t v;
        memcpy(&v, buf, sizeof(v));
        printf("  value = %u\n", (unsigned)v);
        break;
    }
    case DP_KIND_U32: {
        uint32_t v;
        memcpy(&v, buf, sizeof(v));
        printf("  value = %lu\n", (unsigned long)v);
        break;
    }
    case DP_KIND_U64: {
        uint64_t v;
        memcpy(&v, buf, sizeof(v));
        printf("  value = %llu\n", (unsigned long long)v);
        break;
    }
    case DP_KIND_F32: {
        float v;
        memcpy(&v, buf, sizeof(v));
        printf("  value = %.3f\n", v);
        break;
    }
    }
}

static struct {
    struct arg_str *op;
    struct arg_str *name;
    struct arg_str *value;
    struct arg_int *size;
    struct arg_lit *nullbuf;
    struct arg_int *fill;
    struct arg_end *end;
} s_dp_args;

static int cmd_dp(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_dp_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_dp_args.end, argv[0]);
        return 1;
    }

    const char *op = s_dp_args.op->sval[0];

    if (strcmp(op, "list") == 0) {
        /* 28 wide: the longest name is CFG_MQTT_SUBSCRIBE_TOPIC (24). */
        printf("%-28s %4s %5s\n", "NAME", "ID", "SIZE");
        for (size_t i = 0; i < DP_TABLE_COUNT; i++) {
            printf("%-28s %4d %5u\n", s_dp_table[i].name, (int)s_dp_table[i].id,
                   (unsigned)s_dp_table[i].size);
        }
        printf("%u entries. A raw numeric ID also works, with --size <n>.\n",
               (unsigned)DP_TABLE_COUNT);
        return 0;
    }

    if (strcmp(op, "read") != 0 && strcmp(op, "write") != 0) {
        printf("unknown dp op '%s' (use list|read|write)\n", op);
        return 1;
    }
    if (s_dp_args.name->count == 0) {
        printf("read/write need a data point name or numeric ID (try: dp list)\n");
        return 1;
    }

    const char *name = s_dp_args.name->sval[0];
    const dp_entry_t *e = dp_lookup(name);
    data_point_id_t id;
    size_t size;

    if (e != NULL) {
        id = e->id;
        size = e->size;
    } else {
        /* Unnamed / out-of-range IDs are reachable on purpose, so the argument
         * validation in the Data Point Layer can be tested too. */
        char *endp = NULL;
        long raw = strtol(name, &endp, 0);
        if (endp == name || *endp != '\0') {
            printf("unknown data point '%s' (try: dp list)\n", name);
            return 1;
        }
        if (s_dp_args.size->count == 0) {
            printf("a numeric ID needs --size <n>\n");
            return 1;
        }
        id = (data_point_id_t)raw;
        size = (size_t)s_dp_args.size->ival[0];
    }

    /* An explicit --size overrides the table, to force a size mismatch. */
    if (s_dp_args.size->count != 0) {
        int want = s_dp_args.size->ival[0];
        if (want < 0) {
            printf("--size must be >= 0\n");
            return 1;
        }
        size = (size_t)want;
    }

    uint8_t buf[192];
    if (size > sizeof(buf)) {
        printf("--size %u exceeds the harness buffer (%u)\n",
               (unsigned)size, (unsigned)sizeof(buf));
        return 1;
    }
    memset(buf, 0, sizeof(buf));

    void *bufp = (s_dp_args.nullbuf->count != 0) ? NULL : buf;
    esp_err_t ret;

    if (strcmp(op, "read") == 0) {
        ret = data_point_read(id, bufp, size);
        printf("data_point_read(id=%d, size=%u) -> %s\n",
               (int)id, (unsigned)size, esp_err_to_name(ret));
        if (ret == ESP_OK && bufp != NULL) {
            if (e != NULL && size == e->size) {
                dp_print_value(e, buf, size);
            } else {
                printf("  raw:");
                for (size_t i = 0; i < size; i++) {
                    printf(" %02X", buf[i]);
                }
                printf("\n");
            }
        }
        return (ret == ESP_OK) ? 0 : 1;
    }

    /* write */
    if (s_dp_args.fill->count != 0) {
        /* Fill every byte with 'A' and no terminator: checks that a string write
         * cannot leave the snapshot unterminated. */
        int n = s_dp_args.fill->ival[0];
        if (n < 0 || (size_t)n > size) {
            n = (int)size;
        }
        memset(buf, 'A', (size_t)n);
        printf("  filling %d byte(s) with 'A', no terminator\n", n);
    } else {
        if (s_dp_args.value->count == 0) {
            printf("write needs <value>, or --fill <n>\n");
            return 1;
        }
        if (e == NULL) {
            printf("a numeric ID write needs --fill <n> (no type known to parse <value>)\n");
            return 1;
        }
        const char *v = s_dp_args.value->sval[0];
        switch (e->kind) {
        case DP_KIND_STR:
            if (size > 0) {
                strlcpy((char *)buf, v, size);
            }
            break;
        case DP_KIND_BOOL:
            buf[0] = (strtoul(v, NULL, 0) != 0) ? 1 : 0;
            break;
        case DP_KIND_U8:
            buf[0] = (uint8_t)strtoul(v, NULL, 0);
            break;
        case DP_KIND_U16: {
            uint16_t x = (uint16_t)strtoul(v, NULL, 0);
            memcpy(buf, &x, sizeof(x));
            break;
        }
        case DP_KIND_U32: {
            uint32_t x = (uint32_t)strtoul(v, NULL, 0);
            memcpy(buf, &x, sizeof(x));
            break;
        }
        case DP_KIND_U64: {
            uint64_t x = (uint64_t)strtoull(v, NULL, 0);
            memcpy(buf, &x, sizeof(x));
            break;
        }
        case DP_KIND_F32: {
            float x = strtof(v, NULL);
            memcpy(buf, &x, sizeof(x));
            break;
        }
        }
    }

    ret = data_point_write(id, bufp, size);
    printf("data_point_write(id=%d, size=%u) -> %s\n",
           (int)id, (unsigned)size, esp_err_to_name(ret));
    if (ret == ESP_OK) {
        printf("  RAM snapshot only: nothing saved to NVS, nothing applied.\n");
    }
    return (ret == ESP_OK) ? 0 : 1;
}
#endif /* CONFIG_APP_DP_DEBUG */

static esp_err_t register_meter_commands(void)
{
    const esp_console_cmd_t latest_cmd = {
        .command = "meter-latest",
        .help = "Print latest ATM90E32AS measurements",
        .hint = NULL,
        .func = &cmd_meter_latest,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&latest_cmd), TAG, "register meter-latest failed");

    s_reg_args.op = arg_str1(NULL, NULL, "<read|write>", "register operation");
    s_reg_args.addr = arg_str1(NULL, NULL, "<addr>", "register address, e.g. 0x61");
    s_reg_args.value = arg_str0(NULL, NULL, "[value]", "value for write, e.g. 0x1C89");
    s_reg_args.end = arg_end(4);
    const esp_console_cmd_t reg_cmd = {
        .command = "meter-reg",
        .help = "Read/write ATM90E32AS register: meter-reg read 0x61 | meter-reg write 0x61 0x1C89",
        .hint = NULL,
        .func = &cmd_meter_reg,
        .argtable = &s_reg_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&reg_cmd), TAG, "register meter-reg failed");

    s_cal_args.sub = arg_str1(NULL, NULL, "<subcmd>", "show|default|apply|save|load|auto|auto-pq-gain|auto-phi|phi-err|get-p|auto-power-offset|set|guide");
    s_cal_args.field = arg_str0(NULL, "field", "<field>", "auto/set: u|i (auto), uigain|uioffset|gain|offset|power-offset|phase|pq-gain|fundamental-power-gain; chip-wide pga|wiring|freq; default --field: phi|pqgain|uigain|uioffset|power-offset|fundamental|all");
    s_cal_args.phase = arg_str0(NULL, "phase", "<a|b|c|all>", "target phase; auto: omit or 'all' calibrates all 3 phases (set/default: a|b|c only)");
    s_cal_args.u = arg_int0(NULL, "u", "<n>", "voltage-related value");
    s_cal_args.i = arg_int0(NULL, "i", "<n>", "current-related value");
    s_cal_args.p = arg_int0(NULL, "p", "<n>", "active power offset");
    s_cal_args.q = arg_int0(NULL, "q", "<n>", "reactive power offset");
    s_cal_args.phi = arg_int0(NULL, "phi", "<n>", "phase compensation");
    s_cal_args.value = arg_str0(NULL, "value", "<v>", "chip-wide value or per-phase scalar (pq-gain, fundamental-power-gain)");
    s_cal_args.tolerance = arg_dbl0(NULL, "tolerance", "<percent>", "calibration tolerance percent (default: Kconfig)");
    s_cal_args.error = arg_dbl0(NULL, "error", "<percent>", "known power error from PF=1 baseline");
    s_cal_args.interval = arg_int0(NULL, "interval", "<ms>", "sampling interval for get-p (1-1000ms, default 100ms)");
    s_cal_args.apply = arg_lit0(NULL, "apply", "apply to chip (required for chip-wide pga/wiring/freq and whole-image default; per-phase calib applies automatically, so this is a no-op there)");
    s_cal_args.end = arg_end(15);
    const esp_console_cmd_t cal_cmd = {
        .command = "meter-cal",
        .help = "Calibrate ATM90E32AS. Try: meter-cal guide",
        .hint = NULL,
        .func = &cmd_meter_cal,
        .argtable = &s_cal_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cal_cmd), TAG, "register meter-cal failed");

    s_netcfg_args.sub = arg_str1(NULL, NULL, "<show|sta|ap>", "network config subcommand");
    s_netcfg_args.arg = arg_str0(NULL, NULL, "[on|off]", "argument for 'ap' subcommand");
    s_netcfg_args.ssid = arg_str0(NULL, "ssid", "<ssid>", "WiFi STA SSID");
    s_netcfg_args.pass = arg_str0(NULL, "pass", "<pass>", "WiFi STA password");
    s_netcfg_args.end = arg_end(5);
    const esp_console_cmd_t netcfg_cmd = {
        .command = "net-cfg",
        .help = "Network config: net-cfg show | net-cfg sta --ssid <s> --pass <p> | net-cfg ap on|off",
        .hint = NULL,
        .func = &cmd_net_cfg,
        .argtable = &s_netcfg_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&netcfg_cmd), TAG, "register net-cfg failed");

    s_mb_ref_args.sub = arg_str1(NULL, NULL, "<list|read|compare>", "subcommand");
    s_mb_ref_args.id = arg_int0(NULL, "id", "<N>", "slave ID for read/compare");
    s_mb_ref_args.phase = arg_str0(NULL, "phase", "<a|b|c>", "DUT phase for compare");
    s_mb_ref_args.samples = arg_int0(NULL, "samples", "<N>", "number of samples (1-50, default: read=1, compare=5)");
    s_mb_ref_args.interval = arg_int0(NULL, "interval", "<ms>", "sampling interval (1-1000ms, default: read=100, compare=200)");
    s_mb_ref_args.end = arg_end(6);
    const esp_console_cmd_t mb_ref_cmd = {
        .command = "ref",
        .help = "Reference meter: ref list | ref read --id <N> | ref compare --id <N> --phase <a|b|c>",
        .hint = NULL,
        .func = &cmd_mb_master_ref,
        .argtable = &s_mb_ref_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&mb_ref_cmd), TAG, "register ref failed");

    s_ping_args.host = arg_str1(NULL, NULL, "<ip>", "numeric IPv4 target, e.g. 192.168.1.1");
    s_ping_args.count = arg_int0(NULL, "count", "<1..100>", "packets to send (default 4)");
    s_ping_args.interval = arg_int0(NULL, "interval", "<ms>", "ms between packets, >=100 (default 1000)");
    s_ping_args.timeout = arg_int0(NULL, "timeout", "<ms>", "per-packet reply timeout, >=100 (default 2000)");
    s_ping_args.end = arg_end(4);
    const esp_console_cmd_t ping_cmd = {
        .command = "ping",
        .help = "ICMP ping from the device: ping <ip> [--count N] [--interval ms] [--timeout ms]",
        .hint = NULL,
        .func = &cmd_ping,
        .argtable = &s_ping_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&ping_cmd), TAG, "register ping failed");

    s_extmeter_args.sub = arg_str1(NULL, NULL, "<show|bus|add|set|del|enable|disable>", "ext-meter subcommand");
    s_extmeter_args.slot = arg_int0(NULL, "slot", "<0..7>", "device slot index");
    s_extmeter_args.dev = arg_str0(NULL, "dev", "<pm710|em07k>", "device type (add/set)");
    s_extmeter_args.addr = arg_int0(NULL, "addr", "<1..247>", "RTU slave id (add/set)");
    s_extmeter_args.name = arg_str0(NULL, "name", "<label>", "slot name (add/set)");
    s_extmeter_args.baud = arg_int0(NULL, "baud", "<0..4>", "bus baud code (bus)");
    s_extmeter_args.parity = arg_int0(NULL, "parity", "<0..2>", "bus parity (bus)");
    s_extmeter_args.period = arg_int0(NULL, "period", "<ms>", "bus poll period ms, 5000..60000 (bus)");
    s_extmeter_args.end = arg_end(10);
    const esp_console_cmd_t extmeter_cmd = {
        .command = "ext-meter",
        .help = "RTU master: ext-meter show | bus --baud --parity --period | add [--slot] --dev --addr [--name] | set --slot ... | del --slot | enable|disable [--slot]",
        .hint = NULL,
        .func = &cmd_ext_meter,
        .argtable = &s_extmeter_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&extmeter_cmd), TAG, "register ext-meter failed");

    s_mqttcfg_args.sub = arg_str1(NULL, NULL, "<show|set|enable|disable|period>", "mqtt config subcommand");
    s_mqttcfg_args.name = arg_str0(NULL, "name", "<name>", "broker label (set)");
    s_mqttcfg_args.uri = arg_str0(NULL, "uri", "<host>", "broker host, no scheme (set)");
    s_mqttcfg_args.port = arg_int0(NULL, "port", "<n>", "broker port, e.g. 1883 (set)");
    s_mqttcfg_args.user = arg_str0(NULL, "user", "<user>", "broker username (set)");
    s_mqttcfg_args.pass = arg_str0(NULL, "pass", "<pass>", "broker password (set)");
    s_mqttcfg_args.period = arg_int0(NULL, "period", "<s>", "publish period in seconds, 5..60 (period)");
    s_mqttcfg_args.tls = arg_str0(NULL, "tls", "<mode>", "off|ca|mutual|insecure (set); fills the /flash cert paths");
    s_mqttcfg_args.end = arg_end(10);
    const esp_console_cmd_t mqttcfg_cmd = {
        .command = "mqtt-cfg",
        .help = "MQTT config: mqtt-cfg show | set --uri <h> --port 8883 [--user --pass --tls ca] | enable | disable | period --period <5..60 s>",
        .hint = NULL,
        .func = &cmd_mqtt_cfg,
        .argtable = &s_mqttcfg_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&mqttcfg_cmd), TAG, "register mqtt-cfg failed");

    s_log_args.tag = arg_str1(NULL, NULL, "<tag|*>", "log tag, or * for all tags");
    s_log_args.level = arg_str1(NULL, NULL, "<level>", "none|error|warn|info|debug|verbose");
    s_log_args.end = arg_end(3);
    const esp_console_cmd_t log_cmd = {
        .command = "log",
        .help = "Set runtime log level: log mqtt_mgr none | log * warn | log net_mgr debug",
        .hint = NULL,
        .func = &cmd_log,
        .argtable = &s_log_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&log_cmd), TAG, "register log failed");

    s_mb_slave_log_args.state = arg_str1(NULL, NULL, "<on|off>",
                                         "Toggle esp-modbus library DEBUG logs");
    s_mb_slave_log_args.end = arg_end(2);
    const esp_console_cmd_t mb_slave_log_cmd = {
        .command = "mb-slave-log",
        .help = "Toggle verbose esp-modbus library logs (MB_SERIAL etc.)",
        .hint = NULL,
        .func = &cmd_mb_slave_log,
        .argtable = &s_mb_slave_log_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&mb_slave_log_cmd), TAG, "register mb-slave-log failed");

    const esp_console_cmd_t mb_slave_diag_cmd = {
        .command = "mb-slave-diag",
        .help = "Snapshot of per-event counters and time since last master request",
        .hint = NULL,
        .func = &cmd_mb_slave_diag,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&mb_slave_diag_cmd), TAG, "register mb-slave-diag failed");

    const esp_console_cmd_t reboot_cmd = {
        .command = "reboot",
        .help = "Restart the device",
        .hint = NULL,
        .func = &cmd_reboot,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&reboot_cmd), TAG, "register reboot failed");

    const esp_console_cmd_t cfgsave_cmd = {
        .command = "cfg-save",
        .help = "Persist the complete Configuration Manager RAM snapshot",
        .hint = NULL,
        .func = &cmd_cfg_save,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cfgsave_cmd), TAG, "register cfg-save failed");

    s_cfgreset_args.confirmation = arg_str1(NULL, NULL, "<confirm>",
                                             "literal 'confirm' is required");
    s_cfgreset_args.end = arg_end(1);
    const esp_console_cmd_t cfgreset_cmd = {
        .command = "cfg-reset",
        .help = "Erase persisted configuration and reload defaults: cfg-reset confirm",
        .hint = NULL,
        .func = &cmd_cfg_reset,
        .argtable = &s_cfgreset_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cfgreset_cmd), TAG, "register cfg-reset failed");

    s_cfgapply_args.domain = arg_str0(NULL, NULL, "[all|ethernet|wifi|mqtt|modbus|lcd]",
                                       "domain to apply (default: all)");
    s_cfgapply_args.end = arg_end(2);
    const esp_console_cmd_t cfgapply_cmd = {
        .command = "cfg-apply",
        .help = "Apply a Configuration Manager domain: cfg-apply [all|ethernet|wifi|mqtt|modbus|lcd]. "
                "MQTT is applied at runtime; other domain handlers currently log.",
        .hint = NULL,
        .func = &cmd_cfg_apply,
        .argtable = &s_cfgapply_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cfgapply_cmd), TAG, "register cfg-apply failed");

    s_ioexp_args.sub = arg_str1(NULL, NULL, "<diag|scan|read|write|reset>", "ioexp subcommand");
    s_ioexp_args.pin = arg_int0(NULL, NULL, "[pin]", "PCF8574 pin 0..7 (write)");
    s_ioexp_args.level = arg_int0(NULL, NULL, "[0|1]", "pin level (write)");
    s_ioexp_args.end = arg_end(4);
    const esp_console_cmd_t ioexp_cmd = {
        .command = "ioexp",
        .help = "PCF8574 / W5500-reset diagnostics: ioexp diag | scan | read | write <pin> <0|1> | reset",
        .hint = NULL,
        .func = &cmd_ioexp,
        .argtable = &s_ioexp_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&ioexp_cmd), TAG, "register ioexp failed");

#if CONFIG_APP_DP_DEBUG
    s_dp_args.op = arg_str1(NULL, NULL, "<list|read|write>", "dp subcommand");
    s_dp_args.name = arg_str0(NULL, NULL, "<name|id>", "data point name (dp list) or numeric ID");
    s_dp_args.value = arg_str0(NULL, NULL, "[value]", "value for write");
    s_dp_args.size = arg_int0(NULL, "size", "<n>", "override buffer size (forces a mismatch if wrong)");
    s_dp_args.nullbuf = arg_lit0(NULL, "null", "pass a NULL buffer instead of one");
    s_dp_args.fill = arg_int0(NULL, "fill", "<n>", "write: fill n bytes with 'A', no terminator (string ids)");
    s_dp_args.end = arg_end(6);
    const esp_console_cmd_t dp_cmd = {
        .command = "dp",
        .help = "Data Point Layer test harness (Feature 10): dp list | dp read <name> | "
                "dp write <name> <value> | dp read <id> --size <n> | dp write <name> --fill <n>",
        .hint = NULL,
        .func = &cmd_dp,
        .argtable = &s_dp_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&dp_cmd), TAG, "register dp failed");
#endif

    return ESP_OK;
}

#if CONFIG_APP_CONSOLE_AUTH_ENABLE
/* Read one line into buf (NUL-terminated, newline stripped). Blocks until a
 * line arrives. Returns false only if linenoise gives back NULL (EOF/error). */
static bool console_read_line(const char *prompt, char *buf, size_t buf_len)
{
    char *line = linenoise(prompt);
    if (line == NULL) {
        return false;
    }
    strlcpy(buf, line, buf_len);
    linenoiseFree(line);
    return true;
}

/* Gate the console behind a username/password prompt. Blocks until the correct
 * credentials are entered or the attempt budget is exhausted (then blocks
 * forever, forcing a reboot to retry). Cleartext over serial: this deters
 * casual end-user access, it is not a cryptographic safeguard. */
static void console_login_gate(void)
{
    char user[64];
    char pass[64];

    printf("\n=== Power Meter developer console ===\n");
    printf("Login required.\n");

    for (int attempt = 0; attempt < CONFIG_APP_CONSOLE_AUTH_MAX_ATTEMPTS; attempt++) {
        if (!console_read_line("username: ", user, sizeof(user)) ||
            !console_read_line("password: ", pass, sizeof(pass))) {
            /* No interactive input available; do not unlock. */
            break;
        }

        if (strcmp(user, CONFIG_APP_CONSOLE_AUTH_USERNAME) == 0 &&
            strcmp(pass, CONFIG_APP_CONSOLE_AUTH_PASSWORD) == 0) {
            printf("Login OK.\n\n");
            return;
        }
        printf("Invalid credentials (%d/%d).\n", attempt + 1, CONFIG_APP_CONSOLE_AUTH_MAX_ATTEMPTS);
    }

    printf("Console locked. Reboot to try again.\n");
    ESP_LOGW(TAG, "console login failed; locking until reboot");
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}
#endif

/* The REPL device setup, blocking login gate, and REPL start all run in this
 * task so console_task_start() returns immediately. This keeps the developer
 * console (and its login prompt) fully independent of the metering, Modbus, and
 * WiFi services, which must run whether or not anyone logs in to the console. */
static void console_launch_task(void *arg)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = CONFIG_APP_CONSOLE_PROMPT;
    repl_config.max_cmdline_length = CONFIG_APP_CONSOLE_MAX_CMDLINE_LENGTH;

    esp_err_t ret = esp_console_register_help_command();
    if (ret == ESP_OK) {
        ret = register_meter_commands();
    }

#if defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) || defined(CONFIG_ESP_CONSOLE_UART_CUSTOM)
    esp_console_dev_uart_config_t hw_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    if (ret == ESP_OK) {
        ret = esp_console_new_repl_uart(&hw_config, &repl_config, &repl);
    }
#elif defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw_config = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    if (ret == ESP_OK) {
        ret = esp_console_new_repl_usb_serial_jtag(&hw_config, &repl_config, &repl);
    }
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw_config = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    if (ret == ESP_OK) {
        ret = esp_console_new_repl_usb_cdc(&hw_config, &repl_config, &repl);
    }
#else
#error "No supported console device configured"
#endif

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "console setup failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

#if CONFIG_APP_CONSOLE_AUTH_ENABLE
    /* The REPL device (VFS + line discipline) is installed by esp_console_new_repl_*,
     * so linenoise input works here, before the REPL task starts consuming commands. */
    console_login_gate();
#endif

    ret = esp_console_start_repl(repl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start REPL failed: %s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "console started. Type 'help' or 'meter-cal guide'");
    vTaskDelete(NULL);
}

esp_err_t console_task_start(void)
{
    BaseType_t ok = xTaskCreate(console_launch_task, "console_launch",
                                CONFIG_APP_CONSOLE_MAX_CMDLINE_LENGTH + 4096,
                                NULL, 2, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "create console launch task failed");
    return ESP_OK;
}

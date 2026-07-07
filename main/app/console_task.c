#include "console_task.h"

#include <stdio.h>
#include <string.h>
#include "argtable3/argtable3.h"
#include "config_store.h"
#include "energy_meter_task.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "linenoise/linenoise.h"
#include "network_manager.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "wifi_manager.h"

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
    printf("line_freq=%s wiring=%s pga=0x%04X\n",
           c->line_freq == ATM90E32AS_LINE_FREQ_60HZ ? "60Hz" : "50Hz",
           c->wiring_mode == ATM90E32AS_WIRING_3P3W ? "3P3W" : "3P4W",
           c->pga_gain);
    for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
        const atm90e32as_phase_calib_t *p = &c->phase[i];
        printf("Phase %c: ugain=%u igain=%u uoffset=%d ioffset=%d poffset=%d qoffset=%d pqgain=%u phi=%d pgainf=%u refV=%.2f refI=%.3f\n",
               'A' + i,
               p->voltage_gain, p->current_gain,
               p->voltage_offset, p->current_offset,
               p->active_power_offset, p->reactive_power_offset,
               p->pq_gain, p->phase_comp, p->fundamental_power_gain,
               p->reference_voltage, p->reference_current);
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
    struct arg_lit *apply;
    struct arg_end *end;
} s_cal_args;

/* Chip-wide mode fields (pga|wiring|freq) are not per-phase. Returns true if
 * handled (whether success or error), false if the field is not chip-wide. */
static bool cal_set_mode_field(const char *field, bool apply, int *result)
{
    atm90e32as_calib_t c;
    if (energy_meter_get_calibration(&c) != ESP_OK) {
        printf("get calibration failed\n");
        *result = 1;
        return true;
    }

    const char *val = s_cal_args.value->count ? s_cal_args.value->sval[0] : NULL;

    if (strcmp(field, "pga") == 0) {
        if (val == NULL) { printf("pga requires --value 1|2|4\n"); *result = 1; return true; }
        if (strcmp(val, "1") == 0) c.pga_gain = ATM90E32AS_PGA_GAIN_1X;
        else if (strcmp(val, "2") == 0) c.pga_gain = ATM90E32AS_PGA_GAIN_2X;
        else if (strcmp(val, "4") == 0) c.pga_gain = ATM90E32AS_PGA_GAIN_4X;
        else { printf("invalid pga '%s' (use 1|2|4)\n", val); *result = 1; return true; }
        printf("NOTE: PGA is the current-channel gain. Re-calibrate igain after changing it.\n");
    } else if (strcmp(field, "wiring") == 0) {
        if (val == NULL) { printf("wiring requires --value 3p4w|3p3w\n"); *result = 1; return true; }
        if (strcmp(val, "3p4w") == 0) c.wiring_mode = ATM90E32AS_WIRING_3P4W;
        else if (strcmp(val, "3p3w") == 0) c.wiring_mode = ATM90E32AS_WIRING_3P3W;
        else { printf("invalid wiring '%s' (use 3p4w|3p3w)\n", val); *result = 1; return true; }
    } else if (strcmp(field, "freq") == 0) {
        if (val == NULL) { printf("freq requires --value 50|60\n"); *result = 1; return true; }
        if (strcmp(val, "50") == 0) c.line_freq = ATM90E32AS_LINE_FREQ_50HZ;
        else if (strcmp(val, "60") == 0) c.line_freq = ATM90E32AS_LINE_FREQ_60HZ;
        else { printf("invalid freq '%s' (use 50|60)\n", val); *result = 1; return true; }
    } else {
        return false;
    }

    esp_err_t ret = energy_meter_set_calibration(&c);
    if (ret == ESP_OK && apply) {
        ret = energy_meter_apply_calibration();
    }
    printf("set %s=%s%s: %s\n", field, val, apply ? " and applied" : "", esp_err_to_name(ret));
    *result = ret == ESP_OK ? 0 : 1;
    return true;
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
        esp_err_t ret = energy_meter_reset_calibration_defaults(apply);
        printf("defaults loaded%s: %s\n", apply ? " and applied" : "", esp_err_to_name(ret));
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

        if (strcmp(field, "gain") == 0) {
            if (s_cal_args.u->count) pc->voltage_gain = (uint16_t)s_cal_args.u->ival[0];
            if (s_cal_args.i->count) pc->current_gain = (uint16_t)s_cal_args.i->ival[0];
        } else if (strcmp(field, "offset") == 0) {
            if (s_cal_args.u->count) pc->voltage_offset = (int16_t)s_cal_args.u->ival[0];
            if (s_cal_args.i->count) pc->current_offset = (int16_t)s_cal_args.i->ival[0];
        } else if (strcmp(field, "power-offset") == 0) {
            if (s_cal_args.p->count) pc->active_power_offset = (int16_t)s_cal_args.p->ival[0];
            if (s_cal_args.q->count) pc->reactive_power_offset = (int16_t)s_cal_args.q->ival[0];
        } else if (strcmp(field, "phase") == 0) {
            if (s_cal_args.phi->count) pc->phase_comp = (int16_t)s_cal_args.phi->ival[0];
        } else if (strcmp(field, "ref") == 0) {
            if (s_cal_args.u->count) pc->reference_voltage = (float)s_cal_args.u->ival[0];
            if (s_cal_args.i->count) pc->reference_current = (float)s_cal_args.i->ival[0];
        } else {
            printf("unknown field '%s' (gain|offset|power-offset|phase|ref)\n", field);
            return 1;
        }

        esp_err_t ret = energy_meter_set_calibration(&c);
        if (ret == ESP_OK && apply) {
            ret = energy_meter_apply_calibration();
        }
        printf("set %s phase %c%s: %s\n", field, 'A' + phase, apply ? " and applied" : "", esp_err_to_name(ret));
        return ret == ESP_OK ? 0 : 1;
    }

    if (strcmp(sub, "guide") == 0) {
        printf(
            "ATM90E32AS calibration guide (thesis: use real references, do not fake values)\n"
            "1) Bring-up defaults only verify SPI/comm, not accuracy.\n"
            "2) Apply a known reference voltage, read: meter latest\n"
            "   new_ugain = round(old_ugain * ref_V / measured_V)\n"
            "   meter cal set --field gain --phase a --u <new_ugain> --apply\n"
            "3) Apply a known reference current/load:\n"
            "   new_igain = round(old_igain * ref_I / measured_I)\n"
            "   meter cal set --field gain --phase a --i <new_igain> --apply\n"
            "4) Offsets from measured no-load/zero conditions only:\n"
            "   meter cal set --field offset --phase a --u <v> --i <v> --apply\n"
            "5) Phase/power offset from a reference meter and known PF load.\n"
            "6) Verify with: meter latest\n"
            "7) Persist after verification: meter cal save\n"
            "   On boot it auto-loads. Force reload: meter cal load --apply\n"
            "\n"
            "Chip-wide mode (not per-phase, no --phase):\n"
            "   meter-cal set --field wiring --value 3p4w|3p3w --apply  (drives MODE_SEL relay)\n"
            "   meter-cal set --field freq   --value 50|60 --apply\n"
            "   meter-cal set --field pga    --value 1|2|4 --apply      (current-channel gain; re-cal igain after)\n"
            "   Then persist: meter-cal save\n");
        return 0;
    }

    printf("unknown cal subcommand '%s' (show|default|apply|save|load|set|guide)\n", sub);
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

static const char *network_mode_str(config_network_mode_t mode)
{
    switch (mode) {
    case CONFIG_NETWORK_MODE_ETH_ONLY: return "ETH_ONLY";
    case CONFIG_NETWORK_MODE_WIFI_ONLY: return "WIFI_ONLY";
    default: return "AUTO";
    }
}

static int cmd_net_cfg(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_netcfg_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_netcfg_args.end, argv[0]);
        return 1;
    }

    const char *sub = s_netcfg_args.sub->sval[0];

    config_network_t net;
    esp_err_t ret = config_store_get_network(&net);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        printf("read network config failed: %s\n", esp_err_to_name(ret));
        return 1;
    }

    if (strcmp(sub, "show") == 0) {
        printf("mode=%s\n", network_mode_str(net.mode));
        printf("wifi_ssid=\"%s\"\n", net.wifi_ssid);
        printf("wifi_pass=%s\n", strlen(net.wifi_pass) ? "(set)" : "(empty)");
        printf("eth_dhcp=%d\n", net.eth_dhcp);
        return 0;
    }

    if (strcmp(sub, "sta") == 0) {
        if (s_netcfg_args.ssid->count == 0) {
            printf("sta requires --ssid (and usually --pass)\n");
            return 1;
        }
        strlcpy(net.wifi_ssid, s_netcfg_args.ssid->sval[0], sizeof(net.wifi_ssid));
        if (s_netcfg_args.pass->count) {
            strlcpy(net.wifi_pass, s_netcfg_args.pass->sval[0], sizeof(net.wifi_pass));
        }

        ret = config_store_set_network(&net);
        if (ret != ESP_OK) {
            printf("save network config failed: %s\n", esp_err_to_name(ret));
            return 1;
        }
        /* Push into the running WiFi driver so a later failover can use it
         * without a reboot. Does not connect now. */
        wifi_manager_sta_set_credentials(net.wifi_ssid, net.wifi_pass);
        printf("STA credentials saved (ssid=\"%s\"). Takes effect on next failover.\n", net.wifi_ssid);
        return 0;
    }

    if (strcmp(sub, "ap") == 0) {
        const char *arg = s_netcfg_args.arg->count ? s_netcfg_args.arg->sval[0] : NULL;
        if (arg == NULL || (strcmp(arg, "on") != 0 && strcmp(arg, "off") != 0)) {
            printf("ap requires on|off (e.g. net-cfg ap on)\n");
            return 1;
        }
        if (strcmp(arg, "on") == 0) {
            ret = network_manager_start_config_portal();
            printf("config portal AP %s: %s\n", ret == ESP_OK ? "started" : "start failed", esp_err_to_name(ret));
        } else {
            ret = network_manager_stop_config_portal();
            printf("config portal AP %s: %s\n", ret == ESP_OK ? "stopped" : "stop failed", esp_err_to_name(ret));
        }
        return ret == ESP_OK ? 0 : 1;
    }

    printf("unknown net-cfg subcommand '%s' (show|sta|ap)\n", sub);
    return 1;
}

/* mqtt-cfg: view / set MQTT broker profiles in NVS. Enough to test a plain
 * (non-TLS) broker; TLS/custom-CA entry is deferred (CA PEM is too long for a
 * single console line). */
static struct {
    struct arg_str *sub;
    struct arg_int *idx;
    struct arg_str *name;
    struct arg_str *uri;
    struct arg_int *port;
    struct arg_str *user;
    struct arg_str *pass;
    struct arg_int *period;
    struct arg_end *end;
} s_mqttcfg_args;

static int cmd_mqtt_cfg(int argc, char **argv)
{
    int nerrors = arg_parse(argc, argv, (void **)&s_mqttcfg_args);
    if (nerrors != 0) {
        arg_print_errors(stderr, s_mqttcfg_args.end, argv[0]);
        return 1;
    }

    const char *sub = s_mqttcfg_args.sub->sval[0];

    /* config_mqtt_t is ~7 KB (3 profiles x 2 KB CA); keep it off the console
     * task stack, exactly like mqtt_manager and config_store do. */
    config_mqtt_t *mqtt = malloc(sizeof(*mqtt));
    if (mqtt == NULL) {
        printf("no memory for mqtt config\n");
        return 1;
    }
    esp_err_t ret = config_store_get_mqtt(mqtt);
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        printf("read mqtt config failed: %s\n", esp_err_to_name(ret));
        free(mqtt);
        return 1;
    }

    int rc = 0;

    if (strcmp(sub, "show") == 0) {
        printf("enabled=%d active=%u keepalive=%us period=%ums\n",
               mqtt->enabled, (unsigned)mqtt->active,
               (unsigned)mqtt->keepalive_s, (unsigned)mqtt->publish_period_ms);
        for (int i = 0; i < CONFIG_STORE_MQTT_PROFILE_COUNT; i++) {
            const mqtt_profile_t *p = &mqtt->profiles[i];
            printf("[%d]%s name=\"%s\" uri=\"%s\" port=%u user=\"%s\" pass=%s tls=%d custom_ca=%d\n",
                   i, i == mqtt->active ? "*" : " ",
                   p->name, p->uri, (unsigned)p->port, p->username,
                   strlen(p->password) ? "(set)" : "(empty)",
                   p->tls_enable, p->use_custom_ca);
        }
        free(mqtt);
        return 0;
    }

    if (strcmp(sub, "set") == 0) {
        if (s_mqttcfg_args.idx->count == 0) {
            printf("set requires --idx <0..%d>\n", CONFIG_STORE_MQTT_PROFILE_COUNT - 1);
            free(mqtt);
            return 1;
        }
        int idx = s_mqttcfg_args.idx->ival[0];
        if (idx < 0 || idx >= CONFIG_STORE_MQTT_PROFILE_COUNT) {
            printf("idx out of range (0..%d)\n", CONFIG_STORE_MQTT_PROFILE_COUNT - 1);
            free(mqtt);
            return 1;
        }
        mqtt_profile_t *p = &mqtt->profiles[idx];
        if (s_mqttcfg_args.name->count) strlcpy(p->name, s_mqttcfg_args.name->sval[0], sizeof(p->name));
        if (s_mqttcfg_args.uri->count)  strlcpy(p->uri, s_mqttcfg_args.uri->sval[0], sizeof(p->uri));
        if (s_mqttcfg_args.port->count) p->port = (uint16_t)s_mqttcfg_args.port->ival[0];
        if (s_mqttcfg_args.user->count) strlcpy(p->username, s_mqttcfg_args.user->sval[0], sizeof(p->username));
        if (s_mqttcfg_args.pass->count) strlcpy(p->password, s_mqttcfg_args.pass->sval[0], sizeof(p->password));

        ret = config_store_set_mqtt(mqtt);
        printf("profile %d saved: %s (reboot to apply)\n", idx, esp_err_to_name(ret));
        rc = ret == ESP_OK ? 0 : 1;
    } else if (strcmp(sub, "active") == 0) {
        if (s_mqttcfg_args.idx->count == 0) {
            printf("active requires --idx <0..%d>\n", CONFIG_STORE_MQTT_PROFILE_COUNT - 1);
            free(mqtt);
            return 1;
        }
        int idx = s_mqttcfg_args.idx->ival[0];
        if (idx < 0 || idx >= CONFIG_STORE_MQTT_PROFILE_COUNT) {
            printf("idx out of range (0..%d)\n", CONFIG_STORE_MQTT_PROFILE_COUNT - 1);
            free(mqtt);
            return 1;
        }
        mqtt->active = (uint8_t)idx;
        ret = config_store_set_mqtt(mqtt);
        printf("active profile = %d: %s (reboot to apply)\n", idx, esp_err_to_name(ret));
        rc = ret == ESP_OK ? 0 : 1;
    } else if (strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) {
        mqtt->enabled = (strcmp(sub, "enable") == 0);
        ret = config_store_set_mqtt(mqtt);
        printf("mqtt %s: %s (reboot to apply)\n", mqtt->enabled ? "enabled" : "disabled", esp_err_to_name(ret));
        rc = ret == ESP_OK ? 0 : 1;
    } else if (strcmp(sub, "period") == 0) {
        if (s_mqttcfg_args.period->count == 0) {
            printf("period requires --period <ms>\n");
            free(mqtt);
            return 1;
        }
        mqtt->publish_period_ms = (uint32_t)s_mqttcfg_args.period->ival[0];
        ret = config_store_set_mqtt(mqtt);
        printf("publish period = %ums: %s (reboot to apply)\n",
               (unsigned)mqtt->publish_period_ms, esp_err_to_name(ret));
        rc = ret == ESP_OK ? 0 : 1;
    } else {
        printf("unknown mqtt-cfg subcommand '%s' (show|set|active|enable|disable|period)\n", sub);
        rc = 1;
    }

    free(mqtt);
    return rc;
}

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

    s_cal_args.sub = arg_str1(NULL, NULL, "<subcmd>", "show|default|apply|save|load|set|guide");
    s_cal_args.field = arg_str0(NULL, "field", "<field>", "gain|offset|power-offset|phase|ref|pga|wiring|freq");
    s_cal_args.phase = arg_str0(NULL, "phase", "<a|b|c>", "target phase (per-phase fields)");
    s_cal_args.u = arg_int0(NULL, "u", "<n>", "voltage-related value");
    s_cal_args.i = arg_int0(NULL, "i", "<n>", "current-related value");
    s_cal_args.p = arg_int0(NULL, "p", "<n>", "active power offset");
    s_cal_args.q = arg_int0(NULL, "q", "<n>", "reactive power offset");
    s_cal_args.phi = arg_int0(NULL, "phi", "<n>", "phase compensation");
    s_cal_args.value = arg_str0(NULL, "value", "<v>", "chip-wide value: pga 1|2|4, wiring 3p4w|3p3w, freq 50|60");
    s_cal_args.apply = arg_lit0(NULL, "apply", "apply to chip immediately");
    s_cal_args.end = arg_end(12);
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

    s_mqttcfg_args.sub = arg_str1(NULL, NULL, "<show|set|active|enable|disable|period>", "mqtt config subcommand");
    s_mqttcfg_args.idx = arg_int0(NULL, "idx", "<0..2>", "profile index (set/active)");
    s_mqttcfg_args.name = arg_str0(NULL, "name", "<name>", "profile label (set)");
    s_mqttcfg_args.uri = arg_str0(NULL, "uri", "<host>", "broker host, no scheme (set)");
    s_mqttcfg_args.port = arg_int0(NULL, "port", "<n>", "broker port, e.g. 1883 (set)");
    s_mqttcfg_args.user = arg_str0(NULL, "user", "<user>", "broker username (set)");
    s_mqttcfg_args.pass = arg_str0(NULL, "pass", "<pass>", "broker password (set)");
    s_mqttcfg_args.period = arg_int0(NULL, "period", "<ms>", "publish period ms (period)");
    s_mqttcfg_args.end = arg_end(9);
    const esp_console_cmd_t mqttcfg_cmd = {
        .command = "mqtt-cfg",
        .help = "MQTT config: mqtt-cfg show | set --idx 0 --uri <h> --port 1883 [--user --pass] | active --idx 0 | enable | disable | period --period <ms>",
        .hint = NULL,
        .func = &cmd_mqtt_cfg,
        .argtable = &s_mqttcfg_args,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&mqttcfg_cmd), TAG, "register mqtt-cfg failed");

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

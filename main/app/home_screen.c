#include "home_screen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boot_manager.h"
#include "config_apply.h"
#include "config_manager.h"
#include "energy_meter_task.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hmi_bsp.h"
#include "io_expander.h"
#include "lcd_menu.h"
#include "modbus_master_task.h"
#include "modbus_meters.h"
#include "modbus_slave_task.h"
#include "network_manager.h"
#include "sd_card.h"
#include "sdkconfig.h"
#include "system_status.h"
#include "wifi_manager.h"

/*
 * Home Screen — unified LCD UI/UX for the power meter (Feature: LCD UI/UX).
 *
 * Architecture:
 * - Single task owns the LCD post-boot (no flicker from multiple writers).
 * - Three top-level modes: MAIN (auto-cycle measurement pages), STATUS (system
 *   status pages), MENU (settings/actions).
 * - Page Registry: all measurement/status pages are declared once; both
 *   auto-cycle and manual navigation read from the same list (no index
 *   duplication).
 * - LED/Buzzer integration: LED state reflects system status; buzzer gives
 *   button/alarm feedback when enabled.
 * - Data from existing APIs only — no duplicate measurement/status logic.
 */

#ifndef CONFIG_APP_HMI_TASK_STACK_SIZE
#define CONFIG_APP_HMI_TASK_STACK_SIZE 4096
#endif
#ifndef CONFIG_APP_HMI_TASK_PRIORITY
#define CONFIG_APP_HMI_TASK_PRIORITY 5
#endif

#define HOME_POLL_MS 20
#define HOME_REFRESH_MS 500
#define HOME_AUTOCYCLE_MS 3000
#define HOME_STATUS_TIMEOUT_MS 10000
#define HOME_LCD_WIDTH 20
#define HOME_LCD_HEIGHT 4
#define HOME_INFO_HOLD_MS 1500
#define HOME_RESULT_HOLD_MS 1000     /* "Done!"/"Failed!" flash duration */
#define HOME_LED_BLINK_HALF_MS 500   /* 500 ms on / 500 ms off → 1 Hz blink */
#define HOME_LED_REFRESH_MS 100      /* LED re-paint cadence; must be < blink half-period */
#define HOME_SETTING_MIN_S 0U
#define HOME_SETTING_MAX_S 100U
#define HOME_SETTING_HOLD_MS 500U
#define HOME_SETTING_REPEAT_SLOW_MS 200U
#define HOME_SETTING_REPEAT_MEDIUM_MS 100U
#define HOME_SETTING_REPEAT_FAST_MS 50U
#define HOME_SETTING_MEDIUM_AFTER_MS 1500U
#define HOME_SETTING_FAST_AFTER_MS 3000U
/* (Removed local AP idle timeout — network_manager owns the auto-stop timer.) */

/* LED indices (hardware has 5 LEDs). */
#define LED_POWER 0
#define LED_WIRING 1
#define LED_NETWORK 2
#define LED_MQTT 3
#define LED_ALARM 4

/* Buzzer durations (ms). */
#define BUZZER_BUTTON_CLICK_MS 50
#define BUZZER_ALARM_BEEP_MS 200
#define BUZZER_ALARM_OFF_MS 800

/* ============================================================
 * MODE / STATE
 * ============================================================ */

typedef enum {
    HOME_MODE_MAIN = 0,
    HOME_MODE_STATUS,
    HOME_MODE_MENU,
} home_mode_t;

/* ============================================================
 * PAGE REGISTRY
 * Every measurement page is declared here exactly once.
 * Auto-cycle and manual navigation both iterate this table.
 * ============================================================ */

typedef void (*page_render_fn)(void);

typedef struct {
    const char *id;
    const char *title;
    page_render_fn render;
} main_page_desc_t;

/* Forward declarations for render functions. */
static void render_voltage(void);
static void render_current(void);
static void render_total(void);
static void render_active_power(void);
static void render_energy_import(void);
static void render_energy_export(void);
static void render_power_quality(void);
static void render_alarm_page(void);
static void render_io(void);

static const main_page_desc_t k_main_pages[] = {
    {"voltage",    "VOLTAGE",       render_voltage},
    {"current",    "CURRENT",       render_current},
    {"total",      "TOTAL",         render_total},
    {"power",      "ACTIVE POWER",  render_active_power},
    {"energy_imp", "ENERGY IMPORT", render_energy_import},
    {"energy_exp", "ENERGY EXPORT", render_energy_export},
    {"pq",         "PF / FREQUENCY", render_power_quality},
};
#define MAIN_PAGE_COUNT ((int)(sizeof(k_main_pages)/sizeof(k_main_pages[0])))

/* Status sub-pages (system status view). */
typedef void (*status_render_fn)(void);
static void render_status_network(void);
static void render_status_system(void);
static void render_status_modbus(void);

typedef struct {
    const char *title;
    status_render_fn render;
} status_page_desc_t;

static const status_page_desc_t k_status_pages[] = {
    {"NETWORK STATUS", render_status_network},
    {"SYSTEM STATUS",  render_status_system},
    {"MODBUS STATUS",  render_status_modbus},
    {"ALARMS",         render_alarm_page},
    {"I/O",            render_io},
};
#define STATUS_PAGE_COUNT ((int)(sizeof(k_status_pages)/sizeof(k_status_pages[0])))

/* ============================================================
 * TASK-LOCAL STATE
 * ============================================================ */

static const char *TAG = "home_screen";
static bool s_started;

/* Config-pending flag: caller raises it; task reloads on next poll. */
static volatile bool s_cfg_pending;

/* LCD display settings (reloaded from config on start / Apply). */
static bool s_backlight_on;
static uint32_t s_sleep_timeout_s;
static bool s_asleep;
static uint32_t s_idle_ms;
static bool s_autocycle;        /* true = auto advance pages */
static uint32_t s_cycle_time_ms;
static bool s_mqtt_enabled;
static char s_device_name[CONFIG_MANAGER_DEVICE_NAME_LEN];

/* Buzzer enable flags (from config). */
static bool s_buzzer_button;
static bool s_buzzer_alarm;
static bool s_buzzer_button_val;
static bool s_buzzer_alarm_val;

/* Alarm blink / buzzer timing. */
static uint32_t s_alarm_blink_ms;
static bool s_alarm_blink_state;
static uint32_t s_alarm_beep_ms;
static bool s_alarm_beep_state;

/* Menu-exit request set by menu handlers. */
static bool s_menu_exit;

/* ============================================================
 * LCD HELPERS
 * ============================================================ */

/* Write a line padded to full width (no flicker from stale chars). */
static void put_line(uint8_t row, const char *text)
{
    char line[HOME_LCD_WIDTH + 1];
    size_t len = text ? strlen(text) : 0;
    if (len > HOME_LCD_WIDTH) len = HOME_LCD_WIDTH;
    memset(line, ' ', HOME_LCD_WIDTH);
    if (len > 0) memcpy(line, text, len);
    line[HOME_LCD_WIDTH] = '\0';
    hmi_bsp_lcd_print_line(row, line);
}

/* Write a centred line padded to full width. */
static void put_line_centre(uint8_t row, const char *text)
{
    char line[HOME_LCD_WIDTH + 1];
    size_t len = text ? strlen(text) : 0;
    if (len > HOME_LCD_WIDTH) len = HOME_LCD_WIDTH;
    memset(line, ' ', HOME_LCD_WIDTH);
    int pad = (HOME_LCD_WIDTH - (int)len) / 2;
    if (pad < 0) pad = 0;
    memcpy(line + pad, text, len);
    line[HOME_LCD_WIDTH] = '\0';
    hmi_bsp_lcd_print_line(row, line);
}

/* Key-value line: label left-aligned, value right-aligned, total = LCD width. */
static void put_kv(uint8_t row, const char *key, const char *val)
{
    char line[HOME_LCD_WIDTH + 1];
    size_t klen = key ? strlen(key) : 0;
    size_t vlen = val ? strlen(val) : 0;
    memset(line, ' ', HOME_LCD_WIDTH);
    if (klen > HOME_LCD_WIDTH) klen = HOME_LCD_WIDTH;
    memcpy(line, key, klen);
    if (vlen > HOME_LCD_WIDTH) vlen = HOME_LCD_WIDTH;
    size_t vstart = HOME_LCD_WIDTH - vlen;
    if (vstart < klen + 1) vstart = klen + 1;
    if (vstart + vlen <= HOME_LCD_WIDTH)
        memcpy(line + vstart, val, vlen);
    line[HOME_LCD_WIDTH] = '\0';
    hmi_bsp_lcd_print_line(row, line);
}

/* Map a system_status_state to a short UI string. */
static const char *state_str(system_status_state_t s)
{
    switch (s) {
    case SYS_STATUS_READY:   return "OK";
    case SYS_STATUS_OFFLINE: return "OFFLINE";
    case SYS_STATUS_ERROR:   return "ERROR";
    case SYS_STATUS_INIT:    return "INIT";
    case SYS_STATUS_WARNING: return "WARNING";
    default:                 return "UNKNOWN";
    }
}

static const char *meter_unavailable_str(void)
{
    switch (system_status_get(SYS_MODULE_ATM90)) {
    case SYS_STATUS_INIT:    return "INIT";
    case SYS_STATUS_ERROR:   return "ERROR";
    case SYS_STATUS_OFFLINE: return "OFFLINE";
    default:                 return "NO DATA";
    }
}

static bool meter_data_is_live(void)
{
    system_status_state_t state = system_status_get(SYS_MODULE_ATM90);
    return state == SYS_STATUS_READY || state == SYS_STATUS_WARNING;
}

/* ============================================================
 * MAIN PAGE RENDERERS
 * ============================================================ */

static void render_voltage(void)
{
    atm90e32as_measurements_t m;
    bool ok = meter_data_is_live() && (energy_meter_get_latest(&m) == ESP_OK);
    char line[HOME_LCD_WIDTH + 1];

    put_line_centre(0, "VOLTAGE");
    if (!ok) {
        const char *status = meter_unavailable_str();
        put_kv(1, "V1N", status);
        put_kv(2, "V2N", status);
        put_kv(3, "V3N", status);
        return;
    }

    if (m.wiring_mode == ATM90E32AS_WIRING_3P3W) {
        snprintf(line, sizeof(line), "%.1f V", m.voltage[0]);
        put_kv(1, "U12", line);
        snprintf(line, sizeof(line), "%.1f V", m.voltage[2]);
        put_kv(2, "U32", line);
        put_kv(3, "U13", "N/A");
        return;
    }

    snprintf(line, sizeof(line), "%.1f V", m.voltage[0]);
    put_kv(1, "V1N", line);
    snprintf(line, sizeof(line), "%.1f V", m.voltage[1]);
    put_kv(2, "V2N", line);
    snprintf(line, sizeof(line), "%.1f V", m.voltage[2]);
    put_kv(3, "V3N", line);
}

static void render_current(void)
{
    atm90e32as_measurements_t m;
    bool ok = meter_data_is_live() && (energy_meter_get_latest(&m) == ESP_OK);
    char line[HOME_LCD_WIDTH + 1];

    put_line_centre(0, "CURRENT");
    if (!ok) {
        const char *status = meter_unavailable_str();
        put_kv(1, "I1", status);
        put_kv(2, "I2", status);
        put_kv(3, "I3", status);
        return;
    }

    snprintf(line, sizeof(line), "%.2f A", m.current[0]);
    put_kv(1, "I1", line);
    if (m.wiring_mode == ATM90E32AS_WIRING_3P3W) {
        put_kv(2, "I2", "N/A");
    } else {
        snprintf(line, sizeof(line), "%.2f A", m.current[1]);
        put_kv(2, "I2", line);
    }
    snprintf(line, sizeof(line), "%.2f A", m.current[2]);
    put_kv(3, "I3", line);
}

/* roundf() keeps the sign: a tiny negative input (e.g. -3.7 W, under the
 * 0.005 kW display LSB) rounds to -0.0, which printf renders as "-0.00".
 * Snap any result that rounds to zero back to +0 so a no-load state can
 * never display as negative. */
static float round_kw(float w)
{
    float kw = roundf(w / 10.0f) / 100.0f;
    return (kw == 0.0f) ? 0.0f : kw;
}

static void render_total(void)
{
    atm90e32as_measurements_t m;
    bool ok = meter_data_is_live() && (energy_meter_get_latest(&m) == ESP_OK);
    char line[HOME_LCD_WIDTH + 1];

    put_line_centre(0, "TOTAL");
    if (!ok) {
        const char *status = meter_unavailable_str();
        put_kv(1, "I", status);
        put_kv(2, "P", status);
        put_kv(3, "Q", status);
        return;
    }

    snprintf(line, sizeof(line), "%.2f A", m.current_neutral);
    put_kv(1, "I", line);
    float kw = round_kw(m.total_active_power);
    snprintf(line, sizeof(line), "%.2f kW", kw);
    put_kv(2, "P", line);
    float kvar = round_kw(m.total_reactive_power);
    snprintf(line, sizeof(line), "%.2f kvar", kvar);
    put_kv(3, "Q", line);
}

static void render_active_power(void)
{
    atm90e32as_measurements_t m;
    bool ok = meter_data_is_live() && (energy_meter_get_latest(&m) == ESP_OK);
    char line[HOME_LCD_WIDTH + 1];

    put_line_centre(0, "ACTIVE POWER");
    if (!ok) {
        const char *status = meter_unavailable_str();
        put_kv(1, "L1", status);
        put_kv(2, "L2", status);
        put_kv(3, "L3", status);
        return;
    }
    float kw_a = round_kw(m.active_power[0]);
    snprintf(line, sizeof(line), "%.2f kW", kw_a);
    put_kv(1, "L1", line);
    float kw_b = round_kw(m.active_power[1]);
    snprintf(line, sizeof(line), "%.2f kW", kw_b);
    put_kv(2, "L2", line);
    float kw_c = round_kw(m.active_power[2]);
    snprintf(line, sizeof(line), "%.2f kW", kw_c);
    put_kv(3, "L3", line);
}

static void render_energy_import(void)
{
    energy_meter_energy_t e;
    bool ok = meter_data_is_live() && energy_meter_has_latest() &&
              (energy_meter_get_energy(&e) == ESP_OK);
    char line[HOME_LCD_WIDTH + 1];

    put_line_centre(0, "ENERGY IMPORT");
    if (!ok) {
        put_line_centre(1, meter_unavailable_str());
        put_line(2, "");
        put_line(3, "");
        return;
    }
    snprintf(line, sizeof(line), "%.3f kWh", e.active_import_kwh);
    put_line_centre(1, line);
    snprintf(line, sizeof(line), "%.1f kvarh", e.reactive_import_kvarh);
    put_kv(2, "React:", line);
    put_line(3, "");
}

static void render_energy_export(void)
{
    energy_meter_energy_t e;
    bool ok = meter_data_is_live() && energy_meter_has_latest() &&
              (energy_meter_get_energy(&e) == ESP_OK);
    char line[HOME_LCD_WIDTH + 1];

    put_line_centre(0, "ENERGY EXPORT");
    if (!ok) {
        put_line_centre(1, meter_unavailable_str());
        put_line(2, "");
        put_line(3, "");
        return;
    }
    snprintf(line, sizeof(line), "%.3f kWh", e.active_export_kwh);
    put_line_centre(1, line);
    snprintf(line, sizeof(line), "%.1f kvarh", e.reactive_export_kvarh);
    put_kv(2, "React:", line);
    put_line(3, "");
}

static void render_power_quality(void)
{
    atm90e32as_measurements_t m;
    bool ok = meter_data_is_live() && (energy_meter_get_latest(&m) == ESP_OK);
    char line[HOME_LCD_WIDTH + 1];

    put_line_centre(0, "PF / FREQUENCY");
    if (!ok) {
        put_kv(1, "PF", "---");
        put_kv(2, "Freq", "---");
        put_line(3, "");
        return;
    }
    snprintf(line, sizeof(line), "%.3f", m.total_power_factor);
    put_kv(1, "PF Total", line);
    snprintf(line, sizeof(line), "%.2f Hz", m.frequency);
    put_kv(2, "Freq", line);
    float kva = round_kw(m.total_apparent_power);
    snprintf(line, sizeof(line), "%.2f kVA", kva);
    put_kv(3, "S Total", line);
}

/* ============================================================
 * STATUS PAGE RENDERERS
 * ============================================================ */

static void render_status_network(void)
{
    put_line_centre(0, "NETWORK STATUS");

    char line[HOME_LCD_WIDTH + 1];
    const char *eth_s = "UNKNOWN";
    const char *wifi_s = "UNKNOWN";
    const char *mqtt_s;
    system_status_state_t eth = system_status_get(SYS_MODULE_ETHERNET);
    system_status_state_t wifi = system_status_get(SYS_MODULE_WIFI);
    system_status_state_t mqtt = system_status_get(SYS_MODULE_MQTT);

    network_status_t net = {0};
    if (network_manager_get_status(&net) == ESP_OK) {
        if (net.active_iface == NETWORK_IFACE_ETH && net.has_ip) {
            eth_s = "OK";
        } else if (eth == SYS_STATUS_READY) {
            eth_s = "LINK UP";
        } else if (net.state == NETWORK_STATE_CHECK_ETH ||
                   net.state == NETWORK_STATE_ETH_GET_IP) {
            eth_s = "CONNECTING";
        } else {
            eth_s = state_str(eth);
        }

        if (net.active_iface == NETWORK_IFACE_WIFI_STA && net.has_ip) {
            wifi_s = "OK";
        } else if (net.state == NETWORK_STATE_WIFI_CONNECTING) {
            wifi_s = "CONNECTING";
        } else if (net.active_iface == NETWORK_IFACE_ETH && net.has_ip) {
            wifi_s = "STANDBY";
        } else {
            wifi_s = state_str(wifi);
        }
    } else {
        eth_s = state_str(eth);
        wifi_s = state_str(wifi);
    }

    mqtt_s = s_mqtt_enabled ? state_str(mqtt) : "DISABLED";

    snprintf(line, sizeof(line), "ETH      %s", eth_s);
    put_line(1, line);
    snprintf(line, sizeof(line), "WiFi     %s", wifi_s);
    put_line(2, line);
    snprintf(line, sizeof(line), "MQTT     %s", mqtt_s);
    put_line(3, line);
}

static void render_status_system(void)
{
    put_line_centre(0, "SYSTEM STATUS");

    char line[HOME_LCD_WIDTH + 1];
    system_status_state_t atm90 = system_status_get(SYS_MODULE_ATM90);
    system_status_state_t sd = system_status_get(SYS_MODULE_SD_CARD);

    snprintf(line, sizeof(line), "ATM90    %s", state_str(atm90));
    put_line(1, line);
    const char *sd_s;
    if (sd_card_is_mounted()) {
        sd_s = "OK";
    } else if (sd_card_is_inserted()) {
        sd_s = "MOUNT ERROR";
    } else if (sd == SYS_STATUS_INIT || sd == SYS_STATUS_UNKNOWN) {
        sd_s = state_str(sd);
    } else {
        sd_s = "NO CARD";
    }
    snprintf(line, sizeof(line), "SD       %s", sd_s);
    put_line(2, line);

    /* Wiring mode directly from calibration — no config snapshot on stack. */
    atm90e32as_calib_t calib;
    const char *wiring = "---";
    if (energy_meter_get_calibration(&calib) == ESP_OK) {
        if (calib.wiring_mode == ATM90E32AS_WIRING_3P3W) {
            wiring = "3P3W";
        } else if (calib.wiring_mode == ATM90E32AS_WIRING_3P4W) {
            wiring = "3P4W";
        } else {
            wiring = "INVALID";
        }
    }
    snprintf(line, sizeof(line), "Wiring   %s", wiring);
    put_line(3, line);
}

static void render_status_modbus(void)
{
    put_line_centre(0, "MODBUS STATUS");

    char line[HOME_LCD_WIDTH + 1];
    modbus_master_status_t st;
    if (modbus_master_get_status(&st) != ESP_OK) {
        put_line(1, "Master   UNKNOWN");
        put_line(2, "Slots    -");
        put_line(3, "");
        return;
    }

    const char *master_s;
    if (!st.enabled) {
        master_s = "DISABLED";
    } else if (!st.stack_up && !st.configured) {
        master_s = "NOT CONFIG";
    } else if (st.online_count > 0) {
        master_s = "ONLINE";
    } else if (system_status_get(SYS_MODULE_RS485_MASTER) == SYS_STATUS_ERROR) {
        master_s = "ERROR";
    } else if (system_status_get(SYS_MODULE_RS485_MASTER) == SYS_STATUS_OFFLINE) {
        master_s = "OFFLINE";
    } else if (st.enabled && st.slot_count == 0) {
        master_s = "NO SLOTS";
    } else {
        master_s = "WAITING";
    }
    snprintf(line, sizeof(line), "Master   %s", master_s);
    put_line(1, line);

    snprintf(line, sizeof(line), "Online %u / %u",
             (unsigned)st.online_count, (unsigned)st.slot_count);
    put_line(2, line);

    if (st.enabled && st.slot_count > 0) {
        snprintf(line, sizeof(line), "Cycle %lu", (unsigned long)st.cycle_count);
        put_line(3, line);
    } else {
        put_line(3, "");
    }
}

/* ============================================================
 * ALARM (placeholder — no backend yet)
 * ============================================================ */

static int get_active_alarm_count(void)
{
    /* Placeholder: no alarm detection backend yet. */
    return 0;
}

static void render_alarm_page(void)
{
    put_line_centre(0, "ALARMS");
    put_line(1, "");
    put_line_centre(2, "NONE ACTIVE");
    put_line(3, "Detection: coming");
}

/* ============================================================
 * I/O VIEW/CONTROL
 * ============================================================ */

typedef enum {
    IO_MODE_VIEW = 0,
    IO_MODE_OUT0_SEL,
    IO_MODE_OUT1_SEL,
} io_mode_t;

static io_mode_t s_io_mode = IO_MODE_VIEW;

static const char *io_state_str(esp_err_t ret, bool level)
{
    return ret == ESP_OK ? (level ? "ON" : "OFF") : "ERR";
}

static void render_io(void)
{
    put_line_centre(0, "I/O");

    char line[HOME_LCD_WIDTH + 1];
    bool in0 = false, in1 = false, out0 = false, out1 = false;
    esp_err_t in0_ret = io_expander_get_in0(&in0);
    esp_err_t in1_ret = io_expander_get_in1(&in1);
    esp_err_t out0_ret = io_expander_get_out0(&out0);
    esp_err_t out1_ret = io_expander_get_out1(&out1);

    snprintf(line, sizeof(line), "IN1      %s", io_state_str(in0_ret, in0));
    put_line(1, line);
    snprintf(line, sizeof(line), "IN2      %s", io_state_str(in1_ret, in1));
    put_line(2, line);

    /* Show OUT1/OUT2 with selection marker. LCD 20x4: 3 lines left after title,
     * so line 3 shows both outputs with markers and states. */
    const char *out0_marker = (s_io_mode == IO_MODE_OUT0_SEL) ? ">" : " ";
    const char *out1_marker = (s_io_mode == IO_MODE_OUT1_SEL) ? ">" : " ";
    snprintf(line, sizeof(line), "%sOUT1:%s %sOUT2:%s",
             out0_marker, io_state_str(out0_ret, out0),
             out1_marker, io_state_str(out1_ret, out1));
    put_line(3, line);
}


/* ============================================================
 * LCD SETTINGS RELOAD (from config)
 * ============================================================ */

/* Load the buzzer enable flags from the config snapshot. Public because the
 * engineering-mode menu (hmi_test_task) runs the calibration callbacks defined
 * here, and those call button_click() — without this the flags stay at their
 * zero-init default and the click is silently disabled. */
void home_screen_load_buzzer_pref(void)
{
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        ESP_LOGW(TAG, "no memory for buzzer pref snapshot");
        return;
    }
    if (config_manager_get(cfg) == ESP_OK) {
        s_buzzer_button = cfg->buzzer_enable;
        s_buzzer_alarm = cfg->buzzer_alarm_enable;
        s_buzzer_button_val = cfg->buzzer_enable;
        s_buzzer_alarm_val = cfg->buzzer_alarm_enable;
    } else {
        ESP_LOGW(TAG, "config_manager not ready for buzzer pref");
    }
    free(cfg);
}

static void lcd_settings_reload(void)
{
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        ESP_LOGW(TAG, "no memory for config snapshot");
        return;
    }
    if (config_manager_get(cfg) != ESP_OK) {
        ESP_LOGW(TAG, "config_manager not ready");
        free(cfg);
        return;
    }
    s_backlight_on = cfg->lcd_backlight;
    s_sleep_timeout_s = cfg->lcd_sleep_timeout_s;
    strlcpy(s_device_name, cfg->device_name, sizeof(s_device_name));
    s_buzzer_button = cfg->buzzer_enable;
    s_buzzer_alarm = cfg->buzzer_alarm_enable;
    s_buzzer_button_val = cfg->buzzer_enable;
    s_buzzer_alarm_val = cfg->buzzer_alarm_enable;
    s_autocycle = cfg->lcd_autocycle;
    s_cycle_time_ms = (cfg->lcd_cycle_time_ms > 0) ? cfg->lcd_cycle_time_ms : HOME_AUTOCYCLE_MS;
    /* The snapshot is already in hand, so read the enable flag straight off it
     * rather than taking a second lock via config_manager_get_mqtt(). */
    s_mqtt_enabled = cfg->mqtt.enable;
    free(cfg);

    s_asleep = false;
    s_idle_ms = 0;
    hmi_bsp_lcd_backlight(s_backlight_on);
    ESP_LOGI(TAG, "LCD settings: backlight=%d sleep_timeout_s=%u autocycle=%d",
             (int)s_backlight_on, (unsigned)s_sleep_timeout_s, (int)s_autocycle);
}

/* ============================================================
 * AUTO-SLEEP (LCD backlight timeout)
 * ============================================================ */
static void lcd_idle_tick(bool activity)
{
    if (s_sleep_timeout_s == 0 || !s_backlight_on) return;

    if (activity) {
        s_idle_ms = 0;
        if (s_asleep) {
            s_asleep = false;
            hmi_bsp_lcd_backlight(true);
        }
        return;
    }

    if (!s_asleep) {
        s_idle_ms += HOME_POLL_MS;
        if (s_idle_ms >= s_sleep_timeout_s * 1000U) {
            s_asleep = true;
            hmi_bsp_lcd_backlight(false);
        }
    }
}

/* ============================================================
 * LED UPDATE
 * ============================================================ */

/* Current phase of the 1 Hz status blink, derived from the monotonic system
 * timer instead of a flag toggled by the main loop.
 *
 * Why: blocking modal screens (Info / editors) run their own poll loop and call
 * update_leds() themselves, but they never return to the main loop, so a
 * manually toggled phase froze at whatever value it had when the modal opened —
 * the status LED stopped blinking ("stuck LED"). Deriving the phase from elapsed
 * time makes every caller, main loop or modal, see a consistent live phase. */
static bool status_blink_phase(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    return (now_ms / HOME_LED_BLINK_HALF_MS) % 2 == 0;
}

/* Last mask pushed to the PCF8575, used to suppress redundant I2C writes.
 * update_leds() is called from every modal poll loop (~50 Hz) and the LEDs share
 * the I2C bus with the LCD, which those same loops are redrawing — so writing
 * only on an actual change keeps the bus free for the display. */
static uint8_t s_led_mask_written = 0xFF;  /* 0xFF = never written, forces first push */

static void update_leds(void)
{
    /* In engineering mode: LED status pattern is disabled (calibration is
     * isolated from all background tasks and visual indicators).
     * boot_manager_engineering_mode() is cached at boot, constant for session. */
    if (boot_manager_engineering_mode()) {
        return;
    }

    bool blink = status_blink_phase();
    uint8_t mask = 0;

    /* POWER: always ON when running. */
    mask |= (1 << LED_POWER);

    /* WIRING: ON=3P4W, OFF=3P3W, BLINK=runtime state unavailable/invalid. */
    atm90e32as_calib_t calib;
    if (energy_meter_get_calibration(&calib) == ESP_OK) {
        if (calib.wiring_mode == ATM90E32AS_WIRING_3P4W) {
            mask |= (1 << LED_WIRING);
        } else if (calib.wiring_mode != ATM90E32AS_WIRING_3P3W && blink) {
            mask |= (1 << LED_WIRING);
        }
    } else if (blink) {
        mask |= (1 << LED_WIRING);
    }

    /* NETWORK: ON=active data path has IP; BLINK=manager is bringing a link up;
     * OFF=no usable network and no connection attempt. */
    network_status_t net = {0};
    bool network_usable = false;
    if (network_manager_get_status(&net) == ESP_OK) {
        network_usable = net.has_ip && net.active_iface != NETWORK_IFACE_NONE;
        if (network_usable ||
            ((net.state == NETWORK_STATE_CHECK_ETH ||
              net.state == NETWORK_STATE_ETH_GET_IP ||
              net.state == NETWORK_STATE_WIFI_CONNECTING ||
              net.state == NETWORK_STATE_LOAD_CONFIG ||
              net.state == NETWORK_STATE_INIT) && blink)) {
            mask |= (1 << LED_NETWORK);
        }
    }

    /* MQTT uses the same System Status state shown by the LCD. Disabled and
     * disconnected are OFF; INIT/reconnect while network is usable blinks. */
    system_status_state_t mqtt = system_status_get(SYS_MODULE_MQTT);
    if (s_mqtt_enabled && mqtt == SYS_STATUS_READY) {
        mask |= (1 << LED_MQTT);
    } else if (s_mqtt_enabled && network_usable &&
               (mqtt == SYS_STATUS_INIT || mqtt == SYS_STATUS_OFFLINE) &&
               blink) {
        mask |= (1 << LED_MQTT);
    }

    /* ALARM: BLINK if active alarms exist. Blink state toggles separately. */
    if (get_active_alarm_count() > 0) {
        if (s_alarm_blink_state) {
            mask |= (1 << LED_ALARM);
        }
    }

    if (mask != s_led_mask_written) {
        s_led_mask_written = mask;
        hmi_bsp_set_leds(mask);
    }
}

/* ============================================================
 * ALARM BLINK / BEEP TICK
 * ============================================================ */

static void alarm_tick(void)
{
    int alarm_count = get_active_alarm_count();
    if (alarm_count == 0) {
        s_alarm_blink_ms = 0;
        s_alarm_beep_ms = 0;
        s_alarm_blink_state = false;
        s_alarm_beep_state = false;
        return;
    }

    /* Blink LED. */
    s_alarm_blink_ms += HOME_POLL_MS;
    uint32_t cycle = BUZZER_ALARM_BEEP_MS + BUZZER_ALARM_OFF_MS;
    if (s_alarm_blink_ms >= cycle) {
        s_alarm_blink_ms = 0;
    }
    s_alarm_blink_state = (s_alarm_blink_ms < BUZZER_ALARM_BEEP_MS);

    /* Alarm buzzer (if enabled). */
    if (s_buzzer_alarm) {
        s_alarm_beep_ms += HOME_POLL_MS;
        if (s_alarm_beep_ms >= cycle) {
            s_alarm_beep_ms = 0;
        }
        bool new_beep = (s_alarm_beep_ms < BUZZER_ALARM_BEEP_MS);
        if (new_beep != s_alarm_beep_state) {
            s_alarm_beep_state = new_beep;
            hmi_bsp_buzzer_set(s_alarm_beep_state);
        }
    }
}

/* ============================================================
 * BUTTON CLICK FEEDBACK
 * ============================================================ */

static void button_click(void)
{
    if (s_buzzer_button) {
        hmi_bsp_buzzer_click(BUZZER_BUTTON_CLICK_MS);
    }
}

/* Public entry point so the engineering-mode menu (hmi_test_task) produces the
 * same button feedback as production instead of driving the buzzer itself with
 * a different duration and ignoring the user's buzzer preference. */
void home_screen_button_click(void)
{
    button_click();
}

/* ============================================================
 * MENU WRITE-LINE CALLBACK
 * ============================================================ */

static esp_err_t menu_write_line(void *user_ctx, uint8_t row, const char *text)
{
    (void)user_ctx;
    return hmi_bsp_lcd_print_line(row, text);
}

/* ============================================================
 * MENU CALLBACKS — info screens held HOME_INFO_HOLD_MS then
 * return to let the menu re-render.
 * ============================================================ */

static void wait_buttons_released(void)
{
    uint8_t buttons = 0;
    do {
        (void)hmi_bsp_read_buttons(&buttons);
        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    } while (buttons != 0);
}

/* Information screens are closed by CENTER OR LEFT: CENTER acts like OK,
 * LEFT acts like the menu Back action. Used everywhere the operator only
 * needs to "see this and return" with no further decision. */
static bool wait_modal_ok_or_back(void)
{
    uint8_t buttons = 0;
    wait_buttons_released();
    while (1) {
        if (hmi_bsp_read_buttons(&buttons) == ESP_OK) {
            if (buttons & HMI_BSP_BUTTON_CENTER) {
                button_click();
                wait_buttons_released();
                return true;
            }
            if (buttons & HMI_BSP_BUTTON_LEFT) {
                button_click();
                wait_buttons_released();
                return false;
            }
        }
        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }
}

static bool wait_confirm_cancel(void)
{
    uint8_t buttons = 0;
    do {
        (void)hmi_bsp_read_buttons(&buttons);
        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    } while (buttons != 0);

    bool confirm = true;
    uint8_t previous = 0;
    while (1) {
        char choice[HOME_LCD_WIDTH + 1];
        snprintf(choice, sizeof(choice), confirm ? "<Confirm>" : "<Cancel>");
        put_line_centre(2, choice);

        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) buttons = 0;
        uint8_t edges = buttons & ~previous;
        previous = buttons;
        if (edges & (HMI_BSP_BUTTON_LEFT | HMI_BSP_BUTTON_RIGHT)) {
            confirm = !confirm;
            button_click();
        } else if (edges & HMI_BSP_BUTTON_CENTER) {
            button_click();
            wait_buttons_released();
            return confirm;
        }
        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }
}

/* Busy-wait for `ms` while still running the periodic alarm/LED ticks, so a
 * fixed on-screen hold does not freeze the status blink or mute the alarm beep.
 * Use instead of a bare vTaskDelay() whenever the display is paused but the
 * background indicators must stay alive. */
static void pump_ticks_for_ms(uint32_t ms)
{
    uint32_t elapsed = 0;
    while (elapsed < ms) {
        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
        elapsed += HOME_POLL_MS;
    }
}

static void show_action_result(bool success)
{
    put_line(0, "");
    put_line(1, "");
    put_line_centre(2, success ? "Done!" : "Failed!");
    put_line(3, "");
    /* Hold the result on screen for a moment, but keep pumping the alarm/LED
     * ticks: a plain vTaskDelay(1000) froze the status blink and silenced the
     * alarm beep for a full second on every Done!/Failed! flash. */
    pump_ticks_for_ms(HOME_RESULT_HOLD_MS);
    wait_buttons_released();
}

/* All Info screens must let the operator back out with either CENTER or LEFT
 * (back to the previous menu). wait_modal_ok_or_back handles both edges;
 * L3 has a default of "OK: Back" so the footer is consistent even when the
 * caller did not pass one. */
static void show_info(const char *l0, const char *l1, const char *l2, const char *l3)
{
    put_line_centre(0, l0);
    put_line(1, l1 ? l1 : "");
    put_line(2, l2 ? l2 : "");
    put_line(3, l3 ? l3 : "OK: Back");
    (void)wait_modal_ok_or_back();
}

/* ---- Device Info ---- */
static esp_err_t menu_device_info(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;

    config_manager_t cfg;
    if (config_manager_get(&cfg) != ESP_OK) {
        show_info("DEVICE INFO", "Config Error", "", "");
        return ESP_ERR_INVALID_STATE;
    }

    /* Prepare info lines */
    char lines[3][HOME_LCD_WIDTH + 1];
    int total_lines = 0;

    /* Line 0: Device Name (truncate to fit) */
    snprintf(lines[total_lines++], sizeof(lines[0]), "Name: %.13s", cfg.device_name);

    /* Line 1: Firmware build (OTA tracking from NVS) */
    snprintf(lines[total_lines++], sizeof(lines[0]), "FW: %.15s", cfg.ota_fw_build);

    /* Line 2: Version (OTA tracking from NVS) */
    snprintf(lines[total_lines++], sizeof(lines[0]), "Ver: %.14s", cfg.ota_version);

    /* Single-page display (all 3 lines fit), but use same navigation pattern */
    int cursor = 0;
    int top = 0;
    uint8_t previous = 0;
    wait_buttons_released();

    while (1) {
        put_line_centre(0, "DEVICE INFO");

        /* Display all 3 lines (no pagination needed, but reserve 2 chars anyway) */
        for (int row = 0; row < 3; row++) {
            int idx = top + row;
            uint8_t lcd_row = (uint8_t)(row + 1);

            if (idx >= total_lines) {
                put_line(lcd_row, "");
                continue;
            }

            /* Build display line with cursor and indent */
            char display[HOME_LCD_WIDTH + 1];
            char cursor_char = (idx == cursor) ? '>' : ' ';

            /* Cursor + 1 space indent + content (truncate to fit 18 chars total) */
            snprintf(display, sizeof(display), "%c %.16s", cursor_char, lines[idx]);

            put_line(lcd_row, display);
        }

        /* Button handling */
        uint8_t buttons = 0;
        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) {
            buttons = 0;
        }
        uint8_t edges = buttons & ~previous;
        previous = buttons;

        if (edges & HMI_BSP_BUTTON_TOP) {
            if (cursor > 0) {
                cursor--;
                if (cursor < top) {
                    top = cursor;
                }
            }
            button_click();
        } else if (edges & HMI_BSP_BUTTON_BOTTOM) {
            if (cursor < total_lines - 1) {
                cursor++;
                if (cursor >= top + 3) {
                    top = cursor - 2;
                }
            }
            button_click();
        } else if (edges & HMI_BSP_BUTTON_CENTER) {
            /* CENTER = exit */
            button_click();
            wait_buttons_released();
            return ESP_OK;
        } else if (edges & HMI_BSP_BUTTON_LEFT) {
            /* LEFT = back/exit */
            button_click();
            wait_buttons_released();
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* ---- Cached config snapshot for inline VALUE menu rows ----
 * The engine calls each VALUE row's value_get() on every render (which happens
 * after every key). config_manager_get() copies the whole ~1.2 KB snapshot under
 * a lock, so reading it per row per render would churn the heap and the bus.
 * Instead keep one static snapshot, refreshed lazily: the first provider in a
 * render reads it, the rest reuse it. It is invalidated after any local write
 * (update_save_apply) and on each fresh menu entry, so values are always current
 * when the operator returns from an action. Static (BSS), never on the task
 * stack — the snapshot is too large for the 4 KB HMI stack. */
static config_manager_t s_cfg_view;
static bool s_cfg_view_valid;

static const config_manager_t *cfg_view(void)
{
    if (!s_cfg_view_valid) {
        if (config_manager_get(&s_cfg_view) != ESP_OK) {
            return NULL;
        }
        s_cfg_view_valid = true;
    }
    return &s_cfg_view;
}

static void cfg_view_invalidate(void)
{
    s_cfg_view_valid = false;
}

static esp_err_t update_save_apply(config_manager_t *cfg, config_apply_flags_t flags)
{
    esp_err_t ret = config_manager_update(cfg);
    if (ret == ESP_OK) ret = config_apply(flags);
    if (ret == ESP_OK) ret = config_manager_save();
    /* A write just landed: drop the cached view so the next render re-reads it
     * and every inline VALUE row reflects the new value. */
    cfg_view_invalidate();
    return ret;
}

static esp_err_t apply_wiring_mode(uint8_t wiring_mode)
{
    /* energy_meter is the master for wiring mode — call it directly instead of
     * going through config_manager (which only mirrors the active mode). */
    atm90e32as_wiring_mode_t mode = (wiring_mode == 1U)
        ? ATM90E32AS_WIRING_3P3W
        : ATM90E32AS_WIRING_3P4W;
    esp_err_t ret = energy_meter_set_wiring_mode(mode, true);  /* apply=true */
    if (ret == ESP_OK) {
        ret = energy_meter_save_calibration();
    }
    return ret;
}

/* ---- Meter Setup: Wiring Mode ----
 * Inline VALUE item: OK toggles straight to the opposite mode, no submenu and
 * no confirm. The task re-renders after the action, so the inline value (3W/4W)
 * updates immediately. */
static esp_err_t menu_wiring_mode(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;

    atm90e32as_calib_t calib;
    if (energy_meter_get_calibration(&calib) != ESP_OK) {
        return ESP_OK;  /* meter not ready; inline value already shows "< ?>" */
    }

    bool current_3p3w = calib.wiring_mode == ATM90E32AS_WIRING_3P3W;
    (void)apply_wiring_mode(current_3p3w ? 0U : 1U);
    return ESP_OK;
}

/* ---- Meter Setup: Line Freq (50/60 Hz chip-wide) — inline toggle, no confirm. */
static esp_err_t menu_line_freq(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;

    atm90e32as_calib_t calib;
    if (energy_meter_get_calibration(&calib) != ESP_OK) {
        return ESP_OK;
    }

    bool is_60 = (calib.line_freq == ATM90E32AS_LINE_FREQ_60HZ);
    atm90e32as_line_freq_t next = is_60 ? ATM90E32AS_LINE_FREQ_50HZ
                                        : ATM90E32AS_LINE_FREQ_60HZ;
    esp_err_t ret = energy_meter_set_line_freq(next, true);
    if (ret == ESP_OK) {
        ret = energy_meter_save_calibration();
    }
    /* set_line_freq already syncs alarm nominal/band mirror in config_manager. */
    if (ret == ESP_OK) {
        (void)config_manager_save();
    }
    return ESP_OK;
}

/* ---- Meter Setup: inline value providers for the VALUE menu items ----
 * Format "<val>" followed by 2 trailing spaces: the 1st reserves the scroll
 * marker column, the 2nd keeps a visual gap from the right edge. */
static void wiring_mode_value(lcd_menu_t *menu, const lcd_menu_item_t *item, char *buf, size_t buf_size, void *ctx)
{
    (void)menu; (void)item; (void)ctx;
    atm90e32as_calib_t calib;
    if (energy_meter_get_calibration(&calib) != ESP_OK) {
        snprintf(buf, buf_size, "< ?>  ");
        return;
    }
    snprintf(buf, buf_size, "<%s>  ",
             calib.wiring_mode == ATM90E32AS_WIRING_3P3W ? "3W" : "4W");
}

static void line_freq_value(lcd_menu_t *menu, const lcd_menu_item_t *item, char *buf, size_t buf_size, void *ctx)
{
    (void)menu; (void)item; (void)ctx;
    atm90e32as_calib_t calib;
    if (energy_meter_get_calibration(&calib) != ESP_OK) {
        snprintf(buf, buf_size, "< ?>  ");
        return;
    }
    snprintf(buf, buf_size, "<%sHz>  ",
             calib.line_freq == ATM90E32AS_LINE_FREQ_60HZ ? "60" : "50");
}

/* ---- Meter Setup: Current CT ----
 * Draft edits on CT Ratio / I Rated / I Expected. Back:
 *   dirty → Apply? Yes=recompute PGA (+reset Igain if user edited), stay in menu
 *            No=discard draft and exit
 *   clean → exit, no prompt.
 * Device clamp of Expected alone does not count as user edit for Igain reset. */
/* CT configuration constants */
#define CT_RATIO_MIN  1000U
#define CT_RATIO_MAX  6000U
#define CT_RATIO_STEP 100U
#define CT_PRIMARY_MIN  1000U
#define CT_PRIMARY_MAX  6000U
#define CT_PRIMARY_STEP 100U
#define CT_SECONDARY_FIXED 1U  /* Secondary always 1 (1:N format) */
#define CT_I_EDIT_MIN 1U
#define CT_I_EDIT_MAX 10000U
#define CT_I_EDIT_STEP 1U

/* CT I_max calculation: V_ADC_max=180mV (720mV/PGA4), R_burden=4.4Ω
 * I_max(A) = (V_ADC_max / R_burden) × NCT
 *          = (0.180V / 4.4Ω) × NCT
 *          = 0.040909091 × NCT
 *
 * Optimized integer formula to avoid division in display path:
 * I_max_mA = (NCT × 40909) / 1000  (result in milliamps)
 * This way: only ONE division by constant 1000, MCU handles efficiently */
#define CT_IMAX_COEFF_MA  40909UL  /* 0.040909 A/ratio × 1000 mA/A */
#define CT_CALC_IMAX_MA(nct)  (((uint32_t)(nct) * CT_IMAX_COEFF_MA) / 1000UL)

static uint32_t setting_repeat_interval_ms(uint32_t held_ms);

/* u8 step editor — same UX as edit_setting_seconds but for 0..255 in [min,max].
 * Defined later; declared here so the RTU Slave ID / Baud menus can call it. */
static bool edit_setting_u8(const char *title, uint8_t initial, uint8_t min_v, uint8_t max_v,
                            uint8_t *result);

/* Baud rate picker — shows real baud (9600/19200/...) instead of internal
 * 0..4 code. Defined later; declared here so the RTU Slave Baud menu can call it. */
static bool edit_setting_baud(uint32_t initial_baud, uint8_t *result_code);

/* Snap to nearest step within [min,max]; step must be >= 1. */
static uint16_t clamp_step_u16(uint16_t value, uint16_t min_v, uint16_t max_v, uint16_t step)
{
    if (step < 1U) {
        step = 1U;
    }
    if (value < min_v) {
        return min_v;
    }
    if (value > max_v) {
        return max_v;
    }
    if (step == 1U) {
        return value;
    }
    uint16_t base = (uint16_t)(value - min_v);
    uint16_t q = (uint16_t)((base + (step / 2U)) / step);
    uint32_t snapped = (uint32_t)min_v + (uint32_t)q * (uint32_t)step;
    if (snapped > max_v) {
        /* Largest multiple of step on/under max. */
        uint16_t span = (uint16_t)(max_v - min_v);
        q = (uint16_t)(span / step);
        snapped = (uint32_t)min_v + (uint32_t)q * (uint32_t)step;
    }
    return (uint16_t)snapped;
}

static uint16_t wrap_step_u16(uint16_t value, bool increment,
                              uint16_t min_v, uint16_t max_v, uint16_t step)
{
    value = clamp_step_u16(value, min_v, max_v, step);
    if (step < 1U) {
        step = 1U;
    }
    if (increment) {
        if (value >= max_v || (uint32_t)value + (uint32_t)step > max_v) {
            return min_v;
        }
        return (uint16_t)(value + step);
    }
    if (value <= min_v || value < min_v + step) {
        /* Largest stepped value <= max. */
        return clamp_step_u16(max_v, min_v, max_v, step);
    }
    return (uint16_t)(value - step);
}

static esp_err_t menu_current_ct(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;

    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        show_action_result(false);
        return ESP_OK;
    }
    if (config_manager_get(cfg) != ESP_OK) {
        free(cfg);
        show_info("CURRENT CT", "Config unavailable", "", NULL);
        return ESP_OK;
    }

    uint16_t draft_nct = clamp_step_u16(cfg->ct_ratio ? cfg->ct_ratio : CT_RATIO_MIN,
                                        CT_RATIO_MIN, CT_RATIO_MAX, CT_RATIO_STEP);
    uint16_t orig_nct = draft_nct;

    bool redraw = true;
    uint8_t previous = 0;
    uint8_t held_key = 0;
    TickType_t pressed_at = 0;
    TickType_t repeat_at = 0;

    wait_buttons_released();
    while (1) {
        if (redraw) {
            char l1[HOME_LCD_WIDTH + 1];
            char l2[HOME_LCD_WIDTH + 1];
            put_line_centre(0, "CURRENT CT");

            /* Calculate I_max for display */
            uint32_t i_max_ma = CT_CALC_IMAX_MA(draft_nct);
            float i_max_a = (float)i_max_ma / 1000.0f;

            snprintf(l1, sizeof(l1), "> Ratio  <%u:1>  ", (unsigned)draft_nct);
            snprintf(l2, sizeof(l2), "  I max  %.1f A", i_max_a);
            put_line(1, l1);
            put_line(2, l2);
            put_line(3, "");
            redraw = false;
        }

        uint8_t buttons = 0;
        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) buttons = 0;
        uint8_t edges = buttons & ~previous;
        previous = buttons;

        if (edges & HMI_BSP_BUTTON_TOP) {
            button_click();
            draft_nct = wrap_step_u16(draft_nct, true, CT_RATIO_MIN, CT_RATIO_MAX, CT_RATIO_STEP);
            held_key = HMI_BSP_BUTTON_TOP;
            pressed_at = xTaskGetTickCount();
            repeat_at = pressed_at + pdMS_TO_TICKS(HOME_SETTING_HOLD_MS);
            redraw = true;
        } else if (edges & HMI_BSP_BUTTON_BOTTOM) {
            button_click();
            draft_nct = wrap_step_u16(draft_nct, false, CT_RATIO_MIN, CT_RATIO_MAX, CT_RATIO_STEP);
            held_key = HMI_BSP_BUTTON_BOTTOM;
            pressed_at = xTaskGetTickCount();
            repeat_at = pressed_at + pdMS_TO_TICKS(HOME_SETTING_HOLD_MS);
            redraw = true;
        } else if (edges & HMI_BSP_BUTTON_CENTER) {
            button_click();
            wait_buttons_released();

            bool dirty = (draft_nct != orig_nct);
            if (!dirty) {
                free(cfg);
                return ESP_OK;
            }

            put_line_centre(0, "APPLY CT?");
            put_line_centre(1, "Save ratio");
            put_line(2, "");
            put_line(3, "");
            if (!wait_confirm_cancel()) {
                free(cfg);
                return ESP_OK;
            }

            /* Get current I_rated */
            uint16_t i_rated = cfg->i_rated_a ? cfg->i_rated_a : 1U;
            if (i_rated < CT_I_EDIT_MIN) i_rated = CT_I_EDIT_MIN;
            if (i_rated > CT_I_EDIT_MAX) i_rated = CT_I_EDIT_MAX;

            /* Apply CT ratio */
            energy_meter_ct_apply_result_t result;
            esp_err_t ret = energy_meter_ct_apply(draft_nct, i_rated, 0, true, &result);
            if (ret == ESP_OK) {
                cfg->ct_ratio = result.ct_ratio;
                cfg->i_rated_a = result.i_rated_a;
                cfg->i_expected_a = result.i_expected_a;
                ret = config_manager_update(cfg);
                if (ret == ESP_OK) {
                    ret = config_manager_save();
                }
            }

            if (ret != ESP_OK) {
                show_action_result(false);
                draft_nct = orig_nct;  /* Rollback on error */
            } else {
                /* Success - update baseline */
                orig_nct = result.ct_ratio;
                draft_nct = orig_nct;
                show_action_result(true);
            }
            previous = 0;
            redraw = true;
        } else if (edges & HMI_BSP_BUTTON_LEFT) {
            button_click();
            wait_buttons_released();
            free(cfg);
            return ESP_OK;
        } else if (edges & HMI_BSP_BUTTON_RIGHT) {
            /* RIGHT button has no function here, but still beep for consistency */
            button_click();
        }

        /* Key repeat for ratio adjustment */
        if (held_key != 0 && (buttons & held_key)) {
            TickType_t now = xTaskGetTickCount();
            if ((int32_t)(now - repeat_at) >= 0) {
                draft_nct = wrap_step_u16(draft_nct, held_key == HMI_BSP_BUTTON_TOP,
                                          CT_RATIO_MIN, CT_RATIO_MAX, CT_RATIO_STEP);
                uint32_t held_ms = (uint32_t)((now - pressed_at) * portTICK_PERIOD_MS);
                repeat_at = now + pdMS_TO_TICKS(setting_repeat_interval_ms(held_ms));
                redraw = true;
            }
        } else {
            held_key = 0;
        }

        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }
}

/* ---- Config Portal (SoftAP + web config) ----
 *
 * Submenu under Settings:
 *   Connection Info — SSID / URL / whether AP is up (does not start/stop)
 *   Start Portal    — bring SoftAP + portal up until OK/LEFT
 *
 * LEFT or CENTER (OK) on the active screen tears the portal down. The
 * auto-stop idle timer is owned by network_manager
 * (CONFIG_APP_NET_AP_IDLE_TIMEOUT_MS), so this UI does not duplicate it. */

static void portal_format_ssid(char *line, size_t len)
{
    /* LCD is 20 cols; prefix "SSID " leaves room for a long AP name. */
    char ssid[CONFIG_MANAGER_SSID_LEN];
    if (wifi_manager_ap_get_ssid(ssid, sizeof(ssid)) != ESP_OK || ssid[0] == '\0') {
        strlcpy(ssid, CONFIG_APP_WIFI_METER_AP_SSID, sizeof(ssid));
    }
    snprintf(line, len, "SSID %.14s", ssid);
}

static esp_err_t menu_portal_start(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu;
    (void)item;
    (void)ctx;

    /* AP and STA are mutually exclusive: starting the portal drops the station
     * WiFi link (and with it MQTT/cloud) until the portal closes. This is the one
     * genuinely disruptive action in SETTINGS, so it keeps a single confirm gate
     * even though the trivial reversible toggles elsewhere do not. The redundant
     * "Connection Info" leaf was removed — this active screen already shows the
     * SSID and the 192.168.4.1 URL. */
    put_line_centre(0, "START PORTAL?");
    put_line(1, "WiFi STA drops");
    put_line(2, "");
    put_line_centre(3, "while portal is up");
    if (!wait_confirm_cancel()) {
        return ESP_OK;
    }

    put_line_centre(0, "CONFIG PORTAL");
    put_line_centre(1, "Starting...");
    put_line(2, "");
    put_line(3, "");

    esp_err_t ret = network_manager_start_config_portal();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Config portal start failed: %s", esp_err_to_name(ret));
        /* Best-effort stop so a half-started portal cannot linger. */
        (void)network_manager_stop_config_portal();
        show_action_result(false);
        return ESP_OK;
    }

    char ssid_line[HOME_LCD_WIDTH + 1];
    portal_format_ssid(ssid_line, sizeof(ssid_line));

    put_line_centre(0, "PORTAL ACTIVE");
    put_line(1, ssid_line);
    put_line(2, "Open 192.168.4.1");
    put_line(3, "OK/LEFT: Stop");

    /* Stay while portal is up. LEFT/CENTER(OK) → stop explicitly. The auto-stop
     * idle timer is enforced by network_manager; this loop just observes.
     * Track client presence to log status changes for the operator. */
    uint8_t buttons = 0;
    uint8_t previous = 0;
    bool had_client = false;
    wait_buttons_released();
    while (1) {
        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) {
            buttons = 0;
        }
        uint8_t edges = buttons & ~previous;
        previous = buttons;

        if (edges & (HMI_BSP_BUTTON_LEFT | HMI_BSP_BUTTON_CENTER)) {
            button_click();
            wait_buttons_released();
            break;
        }

        /* network_manager may auto-stop the AP on its idle timer. Detect it
         * here so the "Stopped" screen still renders and the loop returns. */
        if (!wifi_manager_ap_is_active()) {
            ESP_LOGI(TAG, "Config portal AP no longer active; exiting wait loop");
            break;
        }

        bool has_client = wifi_manager_ap_sta_count() > 0;
        if (has_client != had_client) {
            ESP_LOGI(TAG, "Config portal client %s", has_client ? "connected" : "disconnected");
            had_client = has_client;
        }

        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }

    ret = network_manager_stop_config_portal();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Config portal stop failed: %s", esp_err_to_name(ret));
        show_action_result(false);
    } else {
        put_line_centre(0, "CONFIG PORTAL");
        put_line_centre(1, "Stopped");
        put_line(2, "");
        put_line(3, "");
        pump_ticks_for_ms(800);
        wait_buttons_released();
    }
    return ESP_OK;
}

/* ---- RTU Slave (this meter as RS485 slave) ----
 * Slave is always on at boot (it must answer the upstream master). ID and baud
 * show their current value inline and open a single-step editor on OK — no
 * separate Info screen (it duplicated these two values) and no two-row cursor
 * editor. 8N1 framing is fixed (no parity / stop-bit UI).
 *
 * IMPORTANT — reboot to take effect: the esp-modbus slave link is built once at
 * boot and has no safe live teardown (modbus_slave_reconfigure() returns
 * ESP_ERR_NOT_SUPPORTED; the pending-rebuild path in the slave task is never
 * armed). So a new ID/baud is persisted to NVS immediately but only becomes live
 * after a restart. The setters say so and offer the reboot rather than flashing
 * "Done!" and implying a live change. */

static void rtu_slave_id_value(lcd_menu_t *m, const lcd_menu_item_t *it,
                               char *buf, size_t size, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    const config_manager_t *c = cfg_view();
    if (c == NULL) { snprintf(buf, size, "< ?>  "); return; }
    uint8_t addr = c->mb_slave_id;
    if (addr < 1U || addr > 247U) addr = (uint8_t)CONFIG_APP_MB_SLAVE_ADDR;
    if (addr < 1U || addr > 247U) addr = 1U;
    snprintf(buf, size, "<%u>  ", (unsigned)addr);
}

static void rtu_slave_baud_value(lcd_menu_t *m, const lcd_menu_item_t *it,
                                 char *buf, size_t size, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    const config_manager_t *c = cfg_view();
    if (c == NULL) { snprintf(buf, size, "< ?>  "); return; }
    /* The slave's own link speed — mb_baud_code (master bus) is a different
     * UART and must not be what this row shows. */
    uint8_t code = c->mb_slave_baud_code > 4U ? 0U : c->mb_slave_baud_code;
    snprintf(buf, size, "<%lu>  ", (unsigned long)modbus_slave_baud_from_code(code));
}

/* Shown after a saved ID/baud change: persist now, offer the restart that makes
 * it live. wait_confirm_cancel() draws <Confirm>/<Cancel> on row 2, so the
 * message occupies rows 0/1/3. */
static void rtu_slave_reboot_prompt(void)
{
    put_line_centre(0, "RTU SLAVE");
    put_line(1, "Saved. Reboot to");
    put_line(2, "");
    put_line_centre(3, "apply new link?");
    if (wait_confirm_cancel()) {
        modbus_slave_request_restart();   /* esp_restart() after 300 ms */
        put_line(0, "");
        put_line(1, "");
        put_line_centre(2, "Rebooting...");
        put_line(3, "");
        pump_ticks_for_ms(400);
    }
}

static esp_err_t menu_rtu_slave_set_id(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;

    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL || config_manager_get(cfg) != ESP_OK) {
        free(cfg);
        show_info("SLAVE ID", "Config unavailable", "", "");
        return ESP_OK;
    }

    uint8_t current = cfg->mb_slave_id;
    if (current < 1U || current > 247U) {
        current = 10U; /* sane default if NVS held 0 / out-of-range */
    }
    uint8_t picked = current;
    if (!edit_setting_u8("SLAVE ID", current, 1, 247, &picked)) {
        free(cfg);
        return ESP_OK; /* LEFT = discard */
    }
    if (picked == current) {
        free(cfg);
        return ESP_OK; /* unchanged — nothing saved, nothing to reboot */
    }
    cfg->mb_slave_id = picked;
    /* Slave-only reconfigure: editing the slave address must not rebuild the
     * master stack on the same bus. */
    esp_err_t ret = update_save_apply(cfg, CONFIG_APPLY_MODBUS_SLAVE);
    free(cfg);
    if (ret == ESP_OK) rtu_slave_reboot_prompt();
    else show_action_result(false);
    return ESP_OK;
}

static esp_err_t menu_rtu_slave_set_baud(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;

    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL || config_manager_get(cfg) != ESP_OK) {
        free(cfg);
        show_info("SLAVE BAUD", "Config unavailable", "", "");
        return ESP_OK;
    }

    uint8_t current = cfg->mb_slave_baud_code;
    if (current > 4U) {
        current = 0;
    }
    uint8_t picked = current;
    /* Pass the real baud rate so the picker opens on the value the operator sees
     * inline — no internal-code leakage. */
    if (!edit_setting_baud(modbus_slave_baud_from_code(current), &picked)) {
        free(cfg);
        return ESP_OK; /* LEFT = discard */
    }
    if (picked == current) {
        free(cfg);
        return ESP_OK; /* unchanged */
    }
    /* Slave link only — the master bus keeps its own mb_baud_code. */
    cfg->mb_slave_baud_code = picked;
    /* Slave-only reconfigure — same reasoning as menu_rtu_slave_set_id. */
    esp_err_t ret = update_save_apply(cfg, CONFIG_APPLY_MODBUS_SLAVE);
    free(cfg);
    if (ret == ESP_OK) rtu_slave_reboot_prompt();
    else show_action_result(false);
    return ESP_OK;
}

/* ---- TCP Server (Modbus TCP) ----
 * Not implemented yet: no backend, no config field. The old submenu had two
 * leaves (Info + Active) that both just printed "Not available" — a customer
 * drilled one level in only to read the same dead end twice. Collapsed to a
 * single honest screen; when the backend lands this becomes a real toggle. */
static esp_err_t menu_tcp_server(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;
    show_info("TCP SERVER", "Not available yet", "", NULL);
    return ESP_OK;
}

/* Step one second within [min,max], wrapping at both ends. The wrap is what
 * makes the editor usable with three buttons: from the floor, DOWN lands on the
 * ceiling instead of sticking. */
static uint32_t wrap_setting_seconds(uint32_t value, bool increment,
                                     uint32_t min, uint32_t max)
{
    if (increment) return value >= max ? min : value + 1U;
    return value <= min ? max : value - 1U;
}

static uint32_t setting_repeat_interval_ms(uint32_t held_ms)
{
    if (held_ms >= HOME_SETTING_FAST_AFTER_MS) {
        return HOME_SETTING_REPEAT_FAST_MS;
    }
    if (held_ms >= HOME_SETTING_MEDIUM_AFTER_MS) {
        return HOME_SETTING_REPEAT_MEDIUM_MS;
    }
    return HOME_SETTING_REPEAT_SLOW_MS;
}

/* Seconds editor shared by every timed setting. The range is a parameter
 * because the settings do not agree on one: LCD sleep / auto-cycle allow 0
 * ("Off") up to HOME_SETTING_MAX_S, the MQTT publish period is 1..60.
 * A stored value outside the range is pulled in before the first draw. */
static bool edit_setting_seconds(const char *title, uint32_t initial,
                                 uint32_t min, uint32_t max, uint32_t *result)
{
    uint32_t value = initial;
    if (value > max) value = max;
    if (value < min) value = min;

    wait_buttons_released();
    uint8_t previous = 0;
    uint8_t held_key = 0;
    TickType_t pressed_at = 0;
    TickType_t repeat_at = 0;
    bool redraw = true;

    while (1) {
        if (redraw) {
            char line[HOME_LCD_WIDTH + 1];
            put_line_centre(0, title);
            put_line(1, "");
            if (value == 0) snprintf(line, sizeof(line), "Off");
            else snprintf(line, sizeof(line), "%lu s", (unsigned long)value);
            put_line_centre(2, line);
            put_line(3, "");
            redraw = false;
        }

        uint8_t buttons = 0;
        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) buttons = 0;
        uint8_t edges = buttons & ~previous;
        previous = buttons;

        if (edges & HMI_BSP_BUTTON_LEFT) {
            button_click();
            wait_buttons_released();
            return false;
        }
        if (edges & HMI_BSP_BUTTON_CENTER) {
            button_click();
            wait_buttons_released();
            *result = value;
            return true;
        }

        const uint8_t adjust_mask = HMI_BSP_BUTTON_TOP | HMI_BSP_BUTTON_BOTTOM;
        uint8_t direction = buttons & adjust_mask;
        uint8_t direction_edges = edges & adjust_mask;
        TickType_t now = xTaskGetTickCount();

        /* Start a hold only when exactly one direction is pressed. Pressing TOP
         * and BOTTOM together is ambiguous and deliberately cancels repeat. */
        if ((direction_edges == HMI_BSP_BUTTON_TOP ||
             direction_edges == HMI_BSP_BUTTON_BOTTOM) &&
            direction == direction_edges) {
            value = wrap_setting_seconds(value,
                                         direction_edges == HMI_BSP_BUTTON_TOP, min, max);
            held_key = direction_edges;
            pressed_at = now;
            repeat_at = now + pdMS_TO_TICKS(HOME_SETTING_HOLD_MS);
            redraw = true;
            button_click();
        } else if (direction == held_key && held_key != 0) {
            if ((int32_t)(now - repeat_at) >= 0) {
                value = wrap_setting_seconds(value,
                                             held_key == HMI_BSP_BUTTON_TOP, min, max);
                uint32_t held_ms = (uint32_t)((now - pressed_at) * portTICK_PERIOD_MS);
                repeat_at = now + pdMS_TO_TICKS(setting_repeat_interval_ms(held_ms));
                redraw = true;
            }
        } else {
            held_key = 0;
        }

        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }
}

/* Baud rate picker for the RTU slave — shows real baud rates (9600/19200/...)
 * instead of internal 0..4 codes. Same UX as edit_setting_seconds: TOP/BOTTOM
 * steps, hold-to-repeat, CENTER commits, LEFT backs out. */
static const uint32_t s_baud_table[] = {9600U, 19200U, 38400U, 57600U, 115200U};
#define BAUD_TABLE_LEN (sizeof(s_baud_table) / sizeof(s_baud_table[0]))

static bool edit_setting_baud(uint32_t initial_baud, uint8_t *result_code)
{
    uint8_t value = 0;
    for (uint8_t i = 0; i < BAUD_TABLE_LEN; i++) {
        if (s_baud_table[i] == initial_baud) {
            value = i;
            break;
        }
    }

    wait_buttons_released();
    uint8_t previous = 0;
    uint8_t held_key = 0;
    TickType_t pressed_at = 0;
    TickType_t repeat_at = 0;
    bool redraw = true;

    while (1) {
        if (redraw) {
            char line[HOME_LCD_WIDTH + 1];
            put_line_centre(0, "BAUD");
            put_line(1, "");
            snprintf(line, sizeof(line), "%lu", (unsigned long)s_baud_table[value]);
            put_line_centre(2, line);
            put_line(3, "");
            redraw = false;
        }

        uint8_t buttons = 0;
        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) buttons = 0;
        uint8_t edges = buttons & ~previous;
        previous = buttons;

        if (edges & HMI_BSP_BUTTON_LEFT) {
            button_click();
            wait_buttons_released();
            return false;
        }
        if (edges & HMI_BSP_BUTTON_CENTER) {
            button_click();
            wait_buttons_released();
            *result_code = value;
            return true;
        }

        const uint8_t adjust_mask = HMI_BSP_BUTTON_TOP | HMI_BSP_BUTTON_BOTTOM;
        uint8_t direction = buttons & adjust_mask;
        uint8_t direction_edges = edges & adjust_mask;
        TickType_t now = xTaskGetTickCount();

        if ((direction_edges == HMI_BSP_BUTTON_TOP ||
             direction_edges == HMI_BSP_BUTTON_BOTTOM) &&
            direction == direction_edges) {
            if (direction_edges == HMI_BSP_BUTTON_TOP) {
                value = (value + 1U < BAUD_TABLE_LEN) ? (uint8_t)(value + 1U) : 0U;
            } else {
                value = (value == 0U) ? (uint8_t)(BAUD_TABLE_LEN - 1U) : (uint8_t)(value - 1U);
            }
            held_key = direction_edges;
            pressed_at = now;
            repeat_at = now + pdMS_TO_TICKS(HOME_SETTING_HOLD_MS);
            redraw = true;
            button_click();
        } else if (direction == held_key && held_key != 0) {
            if ((int32_t)(now - repeat_at) >= 0) {
                if (held_key == HMI_BSP_BUTTON_TOP) {
                    value = (value + 1U < BAUD_TABLE_LEN) ? (uint8_t)(value + 1U) : 0U;
                } else {
                    value = (value == 0U) ? (uint8_t)(BAUD_TABLE_LEN - 1U) : (uint8_t)(value - 1U);
                }
                uint32_t held_ms = (uint32_t)((now - pressed_at) * portTICK_PERIOD_MS);
                repeat_at = now + pdMS_TO_TICKS(setting_repeat_interval_ms(held_ms));
                redraw = true;
            }
        } else {
            held_key = 0;
        }

        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }
}

/* u8 step editor — same UX as edit_setting_seconds but for 0..255 in [min,max].
 * Used by RTU Slave ID (1..247) and the baud picker (0..4). */
static uint8_t wrap_setting_u8(uint8_t value, bool increment, uint8_t min_v, uint8_t max_v)
{
    if (increment) {
        return value >= max_v ? min_v : (uint8_t)(value + 1U);
    }
    return value <= min_v ? max_v : (uint8_t)(value - 1U);
}

static bool edit_setting_u8(const char *title, uint8_t initial, uint8_t min_v, uint8_t max_v,
                            uint8_t *result)
{
    uint8_t value = initial;
    if (value < min_v) value = min_v;
    if (value > max_v) value = max_v;

    wait_buttons_released();
    uint8_t previous = 0;
    uint8_t held_key = 0;
    TickType_t pressed_at = 0;
    TickType_t repeat_at = 0;
    bool redraw = true;

    while (1) {
        if (redraw) {
            char line[HOME_LCD_WIDTH + 1];
            put_line_centre(0, title);
            put_line(1, "");
            snprintf(line, sizeof(line), "%u", (unsigned)value);
            put_line_centre(2, line);
            put_line(3, "");
            redraw = false;
        }

        uint8_t buttons = 0;
        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) buttons = 0;
        uint8_t edges = buttons & ~previous;
        previous = buttons;

        if (edges & HMI_BSP_BUTTON_LEFT) {
            button_click();
            wait_buttons_released();
            return false;
        }
        if (edges & HMI_BSP_BUTTON_CENTER) {
            button_click();
            wait_buttons_released();
            *result = value;
            return true;
        }

        const uint8_t adjust_mask = HMI_BSP_BUTTON_TOP | HMI_BSP_BUTTON_BOTTOM;
        uint8_t direction = buttons & adjust_mask;
        uint8_t direction_edges = edges & adjust_mask;
        TickType_t now = xTaskGetTickCount();

        if ((direction_edges == HMI_BSP_BUTTON_TOP ||
             direction_edges == HMI_BSP_BUTTON_BOTTOM) &&
            direction == direction_edges) {
            value = wrap_setting_u8(value, direction_edges == HMI_BSP_BUTTON_TOP,
                                    min_v, max_v);
            held_key = direction_edges;
            pressed_at = now;
            repeat_at = now + pdMS_TO_TICKS(HOME_SETTING_HOLD_MS);
            redraw = true;
            button_click();
        } else if (direction == held_key && held_key != 0) {
            if ((int32_t)(now - repeat_at) >= 0) {
                value = wrap_setting_u8(value, held_key == HMI_BSP_BUTTON_TOP,
                                        min_v, max_v);
                uint32_t held_ms = (uint32_t)((now - pressed_at) * portTICK_PERIOD_MS);
                repeat_at = now + pdMS_TO_TICKS(setting_repeat_interval_ms(held_ms));
                redraw = true;
            }
        } else {
            held_key = 0;
        }

        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }
}

/* Inline VALUE providers for the two Display timing rows. Both are "seconds,
 * 0 = disabled", rendered in the shared "<Ns>"/"<OFF>" format. */
static void display_cycle_value(lcd_menu_t *menu, const lcd_menu_item_t *item,
                                char *buf, size_t buf_size, void *ctx)
{
    (void)menu; (void)item; (void)ctx;
    const config_manager_t *c = cfg_view();
    if (c == NULL) { snprintf(buf, buf_size, "< ?>  "); return; }
    uint32_t s = c->lcd_autocycle ? c->lcd_cycle_time_ms / 1000U : 0U;
    if (s == 0) snprintf(buf, buf_size, "<OFF>  ");
    else snprintf(buf, buf_size, "<%lus>  ", (unsigned long)s);
}

static void display_sleep_value(lcd_menu_t *menu, const lcd_menu_item_t *item,
                                char *buf, size_t buf_size, void *ctx)
{
    (void)menu; (void)item; (void)ctx;
    const config_manager_t *c = cfg_view();
    if (c == NULL) { snprintf(buf, buf_size, "< ?>  "); return; }
    uint32_t s = c->lcd_sleep_timeout_s;
    if (s == 0) snprintf(buf, buf_size, "<OFF>  ");
    else snprintf(buf, buf_size, "<%lus>  ", (unsigned long)s);
}

/* Open the seconds editor straight away — the value is already inline on the menu
 * row, so the old "Current: N s" gate added a screen and a keypress for nothing.
 * CENTER commits + persists + applies live (CONFIG_APPLY_LCD), LEFT discards. No
 * Done! flash: the row shows the new value on return. */
static esp_err_t menu_lcd_timed_setting(bool autocycle)
{
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) return ESP_ERR_NO_MEM;

    if (config_manager_get(cfg) != ESP_OK) {
        free(cfg);
        return ESP_OK;
    }

    uint32_t seconds = autocycle
        ? (cfg->lcd_autocycle ? cfg->lcd_cycle_time_ms / 1000U : 0U)
        : cfg->lcd_sleep_timeout_s;

    if (!edit_setting_seconds(autocycle ? "AUTO CYCLE" : "LCD SLEEP TIME",
                              seconds, HOME_SETTING_MIN_S, HOME_SETTING_MAX_S,
                              &seconds)) {
        free(cfg);
        return ESP_OK;
    }

    if (autocycle) {
        cfg->lcd_autocycle = seconds != 0;
        if (seconds != 0) cfg->lcd_cycle_time_ms = seconds * 1000U;
    } else {
        cfg->lcd_sleep_timeout_s = seconds;
    }

    (void)update_save_apply(cfg, CONFIG_APPLY_LCD);
    free(cfg);
    return ESP_OK;
}

static esp_err_t menu_autocycle(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;
    return menu_lcd_timed_setting(true);
}

static esp_err_t menu_auto_off(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;
    return menu_lcd_timed_setting(false);
}

typedef enum {
    ALARM_CFG_VLOW_EN = 1, ALARM_CFG_VHIGH_EN, ALARM_CFG_OC_EN,
    ALARM_CFG_PHASE_EN, ALARM_CFG_FREQ_EN, ALARM_CFG_VLOW,
    ALARM_CFG_VHIGH, ALARM_CFG_OC, ALARM_CFG_FLOW, ALARM_CFG_FHIGH,
    ALARM_CFG_NOMINAL, ALARM_CFG_TRIGGER, ALARM_CFG_CLEAR, ALARM_CFG_HYST
} alarm_cfg_field_t;

static int alarm_cfg_get_deci(const config_manager_t *c, alarm_cfg_field_t field)
{
    switch (field) {
    case ALARM_CFG_VLOW_EN: return c->alarm_voltage_low_enable;
    case ALARM_CFG_VHIGH_EN: return c->alarm_voltage_high_enable;
    case ALARM_CFG_OC_EN: return c->alarm_over_current_enable;
    case ALARM_CFG_PHASE_EN: return c->alarm_phase_loss_enable;
    case ALARM_CFG_FREQ_EN: return c->alarm_frequency_enable;
    case ALARM_CFG_VLOW: return (int)(c->alarm_voltage_low_v * 10.0f + 0.5f);
    case ALARM_CFG_VHIGH: return (int)(c->alarm_voltage_high_v * 10.0f + 0.5f);
    case ALARM_CFG_OC: return (int)(c->alarm_over_current_a * 10.0f + 0.5f);
    case ALARM_CFG_FLOW: return (int)(c->alarm_frequency_low_hz * 10.0f + 0.5f);
    case ALARM_CFG_FHIGH: return (int)(c->alarm_frequency_high_hz * 10.0f + 0.5f);
    case ALARM_CFG_NOMINAL: return c->alarm_nominal_frequency_hz;
    case ALARM_CFG_TRIGGER: return c->alarm_trigger_delay_s;
    case ALARM_CFG_CLEAR: return c->alarm_clear_delay_s;
    case ALARM_CFG_HYST: return (int)(c->alarm_hysteresis * 10.0f + 0.5f);
    default: return 0;
    }
}

static void alarm_cfg_limits(alarm_cfg_field_t field, int *min, int *max, int *step,
                             const char **unit)
{
    *unit = ""; *step = 1;
    switch (field) {
    case ALARM_CFG_VLOW_EN: case ALARM_CFG_VHIGH_EN: case ALARM_CFG_OC_EN:
    case ALARM_CFG_PHASE_EN: case ALARM_CFG_FREQ_EN: *min = 0; *max = 1; break;
    case ALARM_CFG_VLOW: case ALARM_CFG_VHIGH: *min = 10; *max = 10000; *step = 10; *unit = "V"; break;
    case ALARM_CFG_OC: *min = 1; *max = 10000; *unit = "A"; break;
    case ALARM_CFG_FLOW: case ALARM_CFG_FHIGH: *min = 400; *max = 700; *unit = "Hz"; break;
    case ALARM_CFG_NOMINAL: *min = 50; *max = 60; *step = 10; *unit = "Hz"; break;
    case ALARM_CFG_TRIGGER: case ALARM_CFG_CLEAR: *min = 0; *max = 300; *unit = "s"; break;
    case ALARM_CFG_HYST: *min = 0; *max = 1000; *unit = ""; break;
    default: *min = 0; *max = 0; break;
    }
}

static void alarm_cfg_format(char *out, size_t size, alarm_cfg_field_t field, int value)
{
    if (field <= ALARM_CFG_FREQ_EN) snprintf(out, size, "%s", value ? "On" : "Off");
    else if (field == ALARM_CFG_NOMINAL || field == ALARM_CFG_TRIGGER || field == ALARM_CFG_CLEAR)
        snprintf(out, size, "%d %s", value, field == ALARM_CFG_NOMINAL ? "Hz" : "s");
    else {
        const char *unit = (field == ALARM_CFG_VLOW || field == ALARM_CFG_VHIGH) ? "V" :
                           (field == ALARM_CFG_OC ? "A" :
                           (field == ALARM_CFG_FLOW || field == ALARM_CFG_FHIGH ? "Hz" : ""));
        snprintf(out, size, "%d.%d %s", value / 10, abs(value % 10), unit);
    }
}

static bool edit_alarm_value(const char *title, alarm_cfg_field_t field, int initial, int *result)
{
    int min, max, step; const char *unit;
    alarm_cfg_limits(field, &min, &max, &step, &unit); (void)unit;
    int value = initial;
    wait_buttons_released();
    uint8_t previous = 0;
    uint8_t held_key = 0;
    TickType_t pressed_at = 0;
    TickType_t repeat_at = 0;
    bool redraw = true;
    while (1) {
        if (redraw) {
            char text[32];
            alarm_cfg_format(text, sizeof(text), field, value);
            put_line_centre(0, title);
            put_line(1, "");
            put_line_centre(2, text);
            put_line(3, "");
            redraw = false;
        }

        uint8_t buttons = 0;
        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) buttons = 0;
        uint8_t edges = buttons & ~previous;
        previous = buttons;
        if (edges & HMI_BSP_BUTTON_LEFT) {
            button_click(); wait_buttons_released(); return false;
        }
        if (edges & HMI_BSP_BUTTON_CENTER) {
            button_click(); wait_buttons_released(); *result = value; return true;
        }

        const uint8_t adjust_mask = HMI_BSP_BUTTON_TOP | HMI_BSP_BUTTON_BOTTOM;
        uint8_t direction = buttons & adjust_mask;
        uint8_t direction_edges = edges & adjust_mask;
        TickType_t now = xTaskGetTickCount();
        bool change = false;

        if ((direction_edges == HMI_BSP_BUTTON_TOP ||
             direction_edges == HMI_BSP_BUTTON_BOTTOM) &&
            direction == direction_edges) {
            held_key = direction_edges;
            pressed_at = now;
            repeat_at = now + pdMS_TO_TICKS(HOME_SETTING_HOLD_MS);
            change = true;
            button_click();
        } else if (direction == held_key && held_key != 0) {
            if ((int32_t)(now - repeat_at) >= 0) {
                uint32_t held_ms = (uint32_t)((now - pressed_at) * portTICK_PERIOD_MS);
                repeat_at = now + pdMS_TO_TICKS(setting_repeat_interval_ms(held_ms));
                change = true;
            }
        } else {
            held_key = 0;
        }

        if (change) {
            value += (held_key == HMI_BSP_BUTTON_TOP) ? step : -step;
            if (value > max) value = min;
            else if (value < min) value = max;
            redraw = true;
        }

        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }
}

static void alarm_cfg_set(config_manager_t *c, alarm_cfg_field_t field, int v)
{
    switch (field) {
    case ALARM_CFG_VLOW_EN: c->alarm_voltage_low_enable = v; break;
    case ALARM_CFG_VHIGH_EN: c->alarm_voltage_high_enable = v; break;
    case ALARM_CFG_OC_EN: c->alarm_over_current_enable = v; break;
    case ALARM_CFG_PHASE_EN: c->alarm_phase_loss_enable = v; break;
    case ALARM_CFG_FREQ_EN: c->alarm_frequency_enable = v; break;
    case ALARM_CFG_VLOW: c->alarm_voltage_low_v = v / 10.0f; break;
    case ALARM_CFG_VHIGH: c->alarm_voltage_high_v = v / 10.0f; break;
    case ALARM_CFG_OC: c->alarm_over_current_a = v / 10.0f; break;
    case ALARM_CFG_FLOW: c->alarm_frequency_low_hz = v / 10.0f; break;
    case ALARM_CFG_FHIGH: c->alarm_frequency_high_hz = v / 10.0f; break;
    case ALARM_CFG_NOMINAL: c->alarm_nominal_frequency_hz = v; break;
    case ALARM_CFG_TRIGGER: c->alarm_trigger_delay_s = v; break;
    case ALARM_CFG_CLEAR: c->alarm_clear_delay_s = v; break;
    case ALARM_CFG_HYST: c->alarm_hysteresis = v / 10.0f; break;
    default: break;
    }
}

/* Compact inline value for an alarm field, in the shared "<val>" + 2 trailing
 * spaces format (1st space reserves the scroll-marker column, 2nd is the visual
 * gap). Units are dropped where the step makes them redundant: voltage steps by
 * 1.0 V so it is always whole, current/freq/hysteresis step by 0.1 so they keep
 * one decimal. */
static void alarm_inline_value(char *out, size_t size, alarm_cfg_field_t field, int v)
{
    switch (field) {
    case ALARM_CFG_VLOW_EN: case ALARM_CFG_VHIGH_EN: case ALARM_CFG_OC_EN:
    case ALARM_CFG_PHASE_EN: case ALARM_CFG_FREQ_EN:
        snprintf(out, size, "<%s>  ", v ? "ON" : "OFF");
        break;
    case ALARM_CFG_VLOW: case ALARM_CFG_VHIGH:
        snprintf(out, size, "<%dV>  ", v / 10);
        break;
    case ALARM_CFG_OC:
        snprintf(out, size, "<%d.%dA>  ", v / 10, abs(v % 10));
        break;
    case ALARM_CFG_FLOW: case ALARM_CFG_FHIGH:
        snprintf(out, size, "<%d.%dHz>  ", v / 10, abs(v % 10));
        break;
    case ALARM_CFG_TRIGGER: case ALARM_CFG_CLEAR:
        snprintf(out, size, "<%ds>  ", v);
        break;
    case ALARM_CFG_HYST:
        snprintf(out, size, "<%d.%d>  ", v / 10, abs(v % 10));
        break;
    default:
        snprintf(out, size, "< ?>  ");
        break;
    }
}

/* VALUE-row provider: read the cached snapshot and render the field inline. */
static void alarm_value_get(lcd_menu_t *menu, const lcd_menu_item_t *item,
                            char *buf, size_t buf_size, void *ctx)
{
    (void)menu; (void)ctx;
    alarm_cfg_field_t field = (alarm_cfg_field_t)(uintptr_t)item->user_data;
    const config_manager_t *c = cfg_view();
    if (c == NULL) {
        snprintf(buf, buf_size, "< ?>  ");
        return;
    }
    alarm_inline_value(buf, buf_size, field, alarm_cfg_get_deci(c, field));
}

/* One callback for every alarm row, dispatched on the field id in user_data.
 *
 * Enable flags (VLOW_EN..FREQ_EN) toggle straight on OK — no editor, no confirm,
 * no Done! flash — exactly like the Meter Setup WMode/LFreq inline toggles; the
 * <ON>/<OFF> on the row refreshes on the next render.
 *
 * Numeric fields open the step editor directly (no "Current: X" gate — the editor
 * shows the value on its own row). LEFT discards, CENTER commits and persists.
 * The menu row then shows the new value inline, so no result flash is needed.
 *
 * These write real, validated, persisted config (read back by the Data Point
 * Layer and saved to NVS; the web portal has no alarm section). What is not yet
 * wired is live detection — get_active_alarm_count() is a stub and the ALARMS
 * status page says so — so apply only logs; that honesty lives on the status
 * page, not as a gate here. */
static esp_err_t menu_alarm_config(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)ctx;
    alarm_cfg_field_t field = (alarm_cfg_field_t)(uintptr_t)item->user_data;
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) return ESP_ERR_NO_MEM;
    if (config_manager_get(cfg) != ESP_OK) { free(cfg); return ESP_OK; }

    int value = alarm_cfg_get_deci(cfg, field);

    if (field <= ALARM_CFG_FREQ_EN) {
        value = value ? 0 : 1;   /* toggle */
    } else if (!edit_alarm_value(item->label, field, value, &value)) {
        free(cfg);               /* LEFT = discard */
        return ESP_OK;
    }

    alarm_cfg_set(cfg, field, value);
    (void)update_save_apply(cfg, CONFIG_APPLY_ALARM);
    free(cfg);
    return ESP_OK;
}

/* ---- Calibration Export to SD ---- */
static esp_err_t menu_calib_export_sd(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;

    ESP_LOGI(TAG, "[ENGINEER] Calibration Export SD requested");

    if (!sd_card_is_inserted()) {
        ESP_LOGW(TAG, "[ENGINEER] Export failed: SD card not inserted");
        show_info("NO SD CARD", "Insert card", "", "");
        return ESP_OK;
    }
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "[ENGINEER] Export failed: SD card not mounted");
        show_info("SD NOT READY", "Wait or reinsert", "", "");
        return ESP_OK;
    }

    /* Export current calibration to SD card (pack + write done inside) */
    char path[64];
    esp_err_t ret = sd_card_calib_export_current(path, sizeof(path));
    if (ret == ESP_OK) {
        /* Extract basename for display */
        const char *basename = strrchr(path, '/');
        basename = basename ? basename + 1 : path;
        char display[HOME_LCD_WIDTH + 1];
        snprintf(display, sizeof(display), "%.20s", basename);
        ESP_LOGI(TAG, "[ENGINEER] Export succeeded: %s", path);
        show_info("EXPORTED", display, "", "");
    } else {
        ESP_LOGE(TAG, "[ENGINEER] Export failed: %s", esp_err_to_name(ret));
        show_action_result(false);
    }
    return ESP_OK;
}

/* ---- Calibration Load from SD ---- */
static esp_err_t menu_calib_import_sd(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;

    ESP_LOGI(TAG, "[ENGINEER] Calibration Load SD requested");

    if (!sd_card_is_inserted()) {
        ESP_LOGW(TAG, "[ENGINEER] Load failed: SD card not inserted");
        show_info("NO SD CARD", "Insert card", "", "");
        return ESP_OK;
    }
    if (!sd_card_is_mounted()) {
        ESP_LOGW(TAG, "[ENGINEER] Load failed: SD card not mounted");
        show_info("SD NOT READY", "Wait or reinsert", "", "");
        return ESP_OK;
    }

    /* Use static to avoid stack overflow (16 × 32 bytes = 512 bytes) */
    static sd_calib_entry_t files[16];
    size_t count;
    esp_err_t ret = sd_card_calib_list(files, 16, &count);
    if (ret != ESP_OK || count == 0) {
        ESP_LOGW(TAG, "[ENGINEER] Load failed: no calibration files found (ret=%s, count=%zu)",
                 esp_err_to_name(ret), count);
        show_info("NO FILES", "/sdcard/calib", "", "");
        return ESP_OK;
    }

    /* Custom file list UI (20×4 display) */
    size_t cursor = 0;
    uint8_t buttons = 0;
    uint8_t previous = 0;
    char line[HOME_LCD_WIDTH + 1];  /* Move outside loop to save stack */
    wait_buttons_released();

    while (1) {
        /* Render: row0=title, row1-3=files (cursor highlighted) */
        put_line_centre(0, "CALIB FILES");

        /* Calculate visible window and scroll indicators */
        size_t window_start = cursor - (cursor % 3);
        bool has_more_above = (window_start > 0);
        bool has_more_below = (window_start + 3 < count);

        for (size_t i = 0; i < 3; i++) {
            size_t idx = window_start + i;

            if (idx < count) {
                /* Display: "> Calib 01" (no 3W/4W prefix since we only calibrate 4W) */
                int written = snprintf(line, sizeof(line), "%c Calib %02d",
                                       (idx == cursor) ? '>' : ' ',
                                       files[idx].num);

                /* Pad remaining space (put_line uses strlen, so must pad explicitly) */
                if (written < HOME_LCD_WIDTH) {
                    memset(line + written, ' ', HOME_LCD_WIDTH - written);
                    line[HOME_LCD_WIDTH] = '\0';
                }

                /* Scroll indicator: | at last char when more items exist */
                if (i == 0 && has_more_above) {
                    line[HOME_LCD_WIDTH - 1] = '|';
                } else if (i == 2 && has_more_below) {
                    line[HOME_LCD_WIDTH - 1] = '|';
                }
            } else {
                /* Empty line */
                memset(line, ' ', HOME_LCD_WIDTH);
                line[HOME_LCD_WIDTH] = '\0';
            }

            put_line(1 + i, line);
        }

        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) buttons = 0;
        uint8_t edges = buttons & ~previous;
        previous = buttons;

        if (edges & HMI_BSP_BUTTON_TOP) {
            if (cursor > 0) cursor--;
            button_click();
        } else if (edges & HMI_BSP_BUTTON_BOTTOM) {
            if (cursor < count - 1) cursor++;
            button_click();
        } else if (edges & HMI_BSP_BUTTON_LEFT) {
            button_click();
            wait_buttons_released();
            return ESP_OK;  /* abort */
        } else if (edges & HMI_BSP_BUTTON_CENTER) {
            button_click();
            wait_buttons_released();
            break;  /* selected */
        }

        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }

    /* Confirm screen with current mode info */
    atm90e32as_calib_t current_calib;
    const char *current_mode_str = "?";
    if (energy_meter_get_calibration(&current_calib) == ESP_OK) {
        current_mode_str = (current_calib.wiring_mode == ATM90E32AS_WIRING_3P3W) ? "3W" : "4W";
    }

    const char *file_mode_str = (files[cursor].mode == 1) ? "3W" : "4W";
    char display[HOME_LCD_WIDTH + 1];
    /* Show the real basename from the card (short 8.3 or legacy long name). */
    snprintf(display, sizeof(display), "%.20s", files[cursor].filename);
    char confirm_msg[HOME_LCD_WIDTH + 1];
    snprintf(confirm_msg, sizeof(confirm_msg), "Apply to %s mode?", current_mode_str);

    put_line_centre(0, confirm_msg);
    put_line(1, display);
    put_line(2, "");
    /* wait_confirm_cancel() draws <Confirm>/<Cancel> on row 2 only; row 3 still
     * holds the last line of the file list unless cleared here. */
    put_line(3, "");
    if (!wait_confirm_cancel()) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "[ENGINEER] Load calibration: selected '%s' (mode=%s)",
             files[cursor].filename, file_mode_str);

    ret = sd_card_calib_import(files[cursor].filename);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "[ENGINEER] Import succeeded: %s", files[cursor].filename);
    } else {
        ESP_LOGE(TAG, "[ENGINEER] Import failed: %s (ret=%s)",
                 files[cursor].filename, esp_err_to_name(ret));
    }
    show_action_result(ret == ESP_OK);
    return ESP_OK;
}

/* ---- Factory Reset ----
 * Destructive: erases config_store + calibration NVS and reloads defaults. It was
 * a two-leaf submenu (Confirm Reset / Cancel) whose title screen "FACTORY RESET?"
 * was already the real confirm. Collapsed to one direct ACTION that does the
 * confirm itself; LEFT/Cancel aborts. Done!/Failed! flash stays — unlike an inline
 * toggle there is nothing on the row to show the outcome. */
static esp_err_t menu_factory_reset(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;
    put_line_centre(0, "FACTORY RESET?");
    /* One self-contained line. It used to read "Erases all saved" / "config +
     * calib" across two rows, but calibration is developer-only (it lives in the
     * engineering menu, not SETTINGS), so naming it here would advertise a
     * concept the end user is never meant to see or touch. */
    put_line_centre(1, "Erases all settings");
    put_line(2, "");
    put_line(3, "");
    if (!wait_confirm_cancel()) {
        return ESP_OK;
    }
    esp_err_t ret = config_manager_factory_reset();
    show_action_result(ret == ESP_OK);
    return ESP_OK;
}

/* ---- RTU Master (Settings submenu) ----
 * Info   : list used slots → drill into live readings
 * Active : toggle bus + each used slot (persisted + applied) */

static const char *rtu_type_short(uint8_t type)
{
    return (type == METER_DEV_EM07K) ? "EM07K" : "PM710";
}

/* (removed) rtu_sync_legacy: the legacy mb_device mirror is reconciled inside
 * config_manager_update() for every writer, and this helper used to also copy
 * slot[0]'s downstream address into cfg->mb_slave_id — silently reprogramming
 * this device's own RTU slave address whenever the master Active screen was
 * touched. mb_slave_id now has exactly one writer: menu_rtu_slave_set_id. */

/*
 * Info list row — leave 2 rightmost cells free (space + optional '|'):
 *   col0      pointer '>' / ' '
 *   col1      always ' '
 *   col2..17  body (slot/type/name), truncated from the end
 *   col18     always ' '
 *   col19     '|' only when mark_scroll; else ' '
 *
 * Examples (20 col):
 *   "> 0 PM710 Helloxin |"   (more items above/below)
 *   "> 0 PM710 Helloxin  "   (no scroll)
 */
static void rtu_put_list_row(uint8_t row, bool selected, const char *body, bool mark_scroll)
{
    char line[HOME_LCD_WIDTH + 1];
    memset(line, ' ', HOME_LCD_WIDTH);
    line[HOME_LCD_WIDTH] = '\0';
    line[0] = selected ? '>' : ' ';
    if (HOME_LCD_WIDTH > 1) {
        line[1] = ' ';
    }
    /* Body never enters the last 2 columns. */
    if (body != NULL && HOME_LCD_WIDTH > 4) {
        size_t max_body = (size_t)HOME_LCD_WIDTH - 4U;
        size_t n = strlen(body);
        if (n > max_body) {
            n = max_body;
        }
        memcpy(&line[2], body, n);
    }
    /* col18 stays ' '; col19 = '|' only when list can scroll. */
    if (mark_scroll && HOME_LCD_WIDTH > 0) {
        line[HOME_LCD_WIDTH - 1] = '|';
    }
    put_line(row, line);
}

/*
 * Active row — status is LEFT-ORIGIN at a fixed column (not right-aligned).
 *
 * 20-col layout:
 *   col0      pointer '>' / ' '
 *   col1      ' '
 *   col2..11  left label (slot+name or "Bus"), truncated, space-padded
 *   col12     separator ' '
 *   col13..18 status field left-aligned: "[OFF] " or "[ON]  " (6 cols)
 *   col19     '|' if mark_scroll else ' '
 *
 * Examples:
 *   "> 1 helloxin [OFF]|"
 *   "> 1 helloxin [ON] |"
 *   "> 1 helloxin [ON]  "   (no scroll)
 */
#define RTU_ACTIVE_LEFT_W    10  /* cols 2..11 */
#define RTU_ACTIVE_STATUS_W   6  /* cols 13..18 */
#define RTU_ACTIVE_LEFT_COL   2
#define RTU_ACTIVE_SEP_COL   12
#define RTU_ACTIVE_STATUS_COL 13

static void rtu_put_active_row(uint8_t row, bool selected, const char *left,
                               bool on, bool mark_scroll)
{
    char line[HOME_LCD_WIDTH + 1];
    memset(line, ' ', HOME_LCD_WIDTH);
    line[HOME_LCD_WIDTH] = '\0';

    line[0] = selected ? '>' : ' ';
    line[1] = ' ';

    if (left != NULL) {
        size_t n = strlen(left);
        if (n > RTU_ACTIVE_LEFT_W) {
            n = RTU_ACTIVE_LEFT_W; /* truncate name from the end */
        }
        memcpy(&line[RTU_ACTIVE_LEFT_COL], left, n);
    }

    line[RTU_ACTIVE_SEP_COL] = ' ';

    /* Fixed left origin for status; [ON] pads with trailing spaces to match [OFF]. */
    const char *tag = on ? "[ON]" : "[OFF]";
    size_t tag_len = strlen(tag);
    if (tag_len > RTU_ACTIVE_STATUS_W) {
        tag_len = RTU_ACTIVE_STATUS_W;
    }
    memcpy(&line[RTU_ACTIVE_STATUS_COL], tag, tag_len);
    /* remaining of status field already spaces from memset */

    if (mark_scroll) {
        line[HOME_LCD_WIDTH - 1] = '|';
    }
    put_line(row, line);
}

static void menu_rtu_slot_detail(uint8_t slot)
{
    char line[HOME_LCD_WIDTH + 1];
    char scratch[48];
    modbus_master_slot_status_t st;
    meter_readings_t r;
    bool have_st = (modbus_master_get_slot_status(slot, &st) == ESP_OK);
    bool have_r = (modbus_master_get_readings_slot(slot, &r) == ESP_OK);

    put_line_centre(0, "RTU SLOT");

    /* Row1: slot + type + name (no status clutter). */
    if (have_st) {
        snprintf(scratch, sizeof(scratch), "%u %s %s",
                 (unsigned)slot,
                 rtu_type_short(st.type),
                 st.name[0] ? st.name : "-");
    } else {
        snprintf(scratch, sizeof(scratch), "Slot %u", (unsigned)slot);
    }
    strlcpy(line, scratch, sizeof(line));
    put_line(1, line);

    /* Row2: slave id + device state + poll count.
     * ON = answering, OFF = enabled but missed 5 consecutive polls (device
     * down), --- = master not polling (bus/slot disabled or portal up). */
    if (have_st) {
        const char *link = st.state == MODBUS_MASTER_DEV_ON ? "ON" :
                           st.state == MODBUS_MASTER_DEV_OFF ? "OFF" : "---";
        snprintf(scratch, sizeof(scratch), "ID %u %s P%lu",
                 (unsigned)st.slave_id, link, (unsigned long)st.poll_count);
        strlcpy(line, scratch, sizeof(line));
        put_line(2, line);
    } else {
        put_line(2, "No status");
    }

    /* Row3: live readings or empty (no "OK: Back" hint). */
    if (have_r) {
        snprintf(scratch, sizeof(scratch), "V%.0f/%.0f/%.0f",
                 r.voltage[0], r.voltage[1], r.voltage[2]);
        strlcpy(line, scratch, sizeof(line));
        put_line(3, line);
    } else {
        put_line(3, "No readings yet");
    }
    (void)wait_modal_ok_or_back();
}

static bool rtu_load_slot_label(uint8_t slot, uint8_t *type_out,
                                char *name_out, size_t name_len)
{
    modbus_master_slot_status_t st;
    if (modbus_master_get_slot_status(slot, &st) == ESP_OK && st.used) {
        if (type_out) {
            *type_out = st.type;
        }
        if (name_out && name_len > 0) {
            if (st.name[0]) {
                strlcpy(name_out, st.name, name_len);
            } else {
                strlcpy(name_out, rtu_type_short(st.type), name_len);
            }
        }
        return true;
    }

    config_manager_t *cfg = malloc(sizeof(*cfg));
    bool ok = false;
    if (cfg != NULL && config_manager_get(cfg) == ESP_OK && cfg->mb_slots[slot].used) {
        if (type_out) {
            *type_out = cfg->mb_slots[slot].type;
        }
        if (name_out && name_len > 0) {
            if (cfg->mb_slots[slot].name[0]) {
                strlcpy(name_out, cfg->mb_slots[slot].name, name_len);
            } else {
                strlcpy(name_out, rtu_type_short(cfg->mb_slots[slot].type), name_len);
            }
        }
        ok = true;
    }
    free(cfg);
    return ok;
}

static esp_err_t menu_rtu_info(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu;
    (void)item;
    (void)ctx;

    uint8_t slots[CONFIG_MANAGER_MB_SLOT_COUNT];
    uint8_t n = 0;
    for (uint8_t i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
        modbus_master_slot_status_t st;
        if (modbus_master_get_slot_status(i, &st) == ESP_OK && st.used) {
            slots[n++] = i;
        }
    }

    if (n == 0) {
        config_manager_t *cfg = malloc(sizeof(*cfg));
        if (cfg != NULL && config_manager_get(cfg) == ESP_OK) {
            for (uint8_t i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
                if (cfg->mb_slots[i].used) {
                    slots[n++] = i;
                }
            }
        }
        free(cfg);
    }

    if (n == 0) {
        show_info("RTU DEVICES", "No devices", "Add via portal", "");
        return ESP_OK;
    }

    int cursor = 0;
    int top = 0;
    uint8_t previous = 0;
    wait_buttons_released();

    while (1) {
        put_line_centre(0, "RTU DEVICES");
        /* Match lcd_menu: '|' only when more items exist beyond the window. */
        bool has_above = (top > 0);
        bool has_below = (top + 3 < n);

        for (int row = 0; row < 3; row++) {
            int idx = top + row;
            uint8_t lcd_row = (uint8_t)(row + 1);
            if (idx >= n) {
                put_line(lcd_row, "");
                continue;
            }

            uint8_t slot = slots[idx];
            uint8_t type = 0;
            char name[CONFIG_MANAGER_MB_NAME_LEN];
            name[0] = '\0';
            (void)rtu_load_slot_label(slot, &type, name, sizeof(name));

            /* Outer list: slot + type + name only (id/poll live in detail). */
            char body[40];
            snprintf(body, sizeof(body), "%u %s %s",
                     (unsigned)slot, rtu_type_short(type),
                     name[0] ? name : "-");

            bool mark = false;
            if (row == 0 && has_above) {
                mark = true;          /* first visible row: more above */
            } else if (row == 2 && has_below) {
                mark = true;          /* last visible row: more below */
            }
            rtu_put_list_row(lcd_row, idx == cursor, body, mark);
        }

        uint8_t buttons = 0;
        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) {
            buttons = 0;
        }
        uint8_t edges = buttons & ~previous;
        previous = buttons;

        if (edges & HMI_BSP_BUTTON_TOP) {
            if (cursor > 0) {
                cursor--;
            }
            if (cursor < top) {
                top = cursor;
            }
            button_click();
        } else if (edges & HMI_BSP_BUTTON_BOTTOM) {
            if (cursor + 1 < n) {
                cursor++;
            }
            if (cursor > top + 2) {
                top = cursor - 2;
            }
            button_click();
        } else if (edges & HMI_BSP_BUTTON_CENTER) {
            button_click();
            wait_buttons_released();
            menu_rtu_slot_detail(slots[cursor]);
            previous = 0;
            wait_buttons_released();
        } else if (edges & HMI_BSP_BUTTON_LEFT) {
            button_click();
            wait_buttons_released();
            return ESP_OK;
        }

        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }
}

static esp_err_t menu_rtu_active(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu;
    (void)item;
    (void)ctx;

    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        show_action_result(false);
        return ESP_OK;
    }
    if (config_manager_get(cfg) != ESP_OK) {
        free(cfg);
        show_action_result(false);
        return ESP_OK;
    }

    /* rows: 0 = bus, then each used slot */
    uint8_t slots[CONFIG_MANAGER_MB_SLOT_COUNT];
    uint8_t n_slots = 0;
    for (uint8_t i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
        if (cfg->mb_slots[i].used) {
            slots[n_slots++] = i;
        }
    }
    int rows = 1 + (int)n_slots;
    int cursor = 0;
    int top = 0;
    uint8_t previous = 0;
    wait_buttons_released();

    while (1) {
        put_line_centre(0, "RTU ACTIVE");
        bool has_above = (top > 0);
        bool has_below = (top + 3 < rows);

        for (int row = 0; row < 3; row++) {
            int idx = top + row;
            uint8_t lcd_row = (uint8_t)(row + 1);
            if (idx >= rows) {
                put_line(lcd_row, "");
                continue;
            }

            char left[32];
            bool on;
            if (idx == 0) {
                strlcpy(left, "Bus", sizeof(left));
                on = cfg->mb_enabled;
            } else {
                uint8_t slot = slots[idx - 1];
                const config_mb_slot_t *s = &cfg->mb_slots[slot];
                const char *nm = s->name[0] ? s->name : rtu_type_short(s->type);
                /* left truncated to fixed width inside rtu_put_active_row */
                snprintf(left, sizeof(left), "%u %s", (unsigned)slot, nm);
                on = s->enabled;
            }

            bool mark = false;
            if (row == 0 && has_above) {
                mark = true;
            } else if (row == 2 && has_below) {
                mark = true;
            }
            rtu_put_active_row(lcd_row, idx == cursor, left, on, mark);
        }

        uint8_t buttons = 0;
        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) {
            buttons = 0;
        }
        uint8_t edges = buttons & ~previous;
        previous = buttons;

        if (edges & HMI_BSP_BUTTON_TOP) {
            if (cursor > 0) {
                cursor--;
            }
            if (cursor < top) {
                top = cursor;
            }
            button_click();
        } else if (edges & HMI_BSP_BUTTON_BOTTOM) {
            if (cursor + 1 < rows) {
                cursor++;
            }
            if (cursor > top + 2) {
                top = cursor - 2;
            }
            button_click();
        } else if (edges & HMI_BSP_BUTTON_CENTER) {
            button_click();
            if (cursor == 0) {
                cfg->mb_enabled = !cfg->mb_enabled;
            } else {
                uint8_t slot = slots[cursor - 1];
                cfg->mb_slots[slot].enabled = !cfg->mb_slots[slot].enabled;
            }
            /* Master-only reconfigure: toggling the bus or a slot is a master
             * concern. The slave stays on its current address / baud.
             * (The legacy mb_device mirror refreshes inside config_manager_update.) */
            esp_err_t ret = update_save_apply(cfg, CONFIG_APPLY_MODBUS_MASTER);
            if (ret != ESP_OK) {
                (void)config_manager_get(cfg);
            }
            wait_buttons_released();
            previous = 0;
        } else if (edges & HMI_BSP_BUTTON_LEFT) {
            button_click();
            wait_buttons_released();
            free(cfg);
            return ESP_OK;
        }

        alarm_tick();
        update_leds();
        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }
}

/* ---- Back ---- */
static esp_err_t menu_back(lcd_menu_t *menu, const lcd_menu_item_t *item, void *ctx)
{
    (void)menu; (void)item; (void)ctx;
    s_menu_exit = true;
    return ESP_OK;
}

/* ============================================================
 * MENU TREE (static const — no heap)
 * ============================================================ */

/* Buzzer sub-menu */

static esp_err_t save_buzzer_preferences(bool button, bool alarm)
{
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) return ESP_ERR_NO_MEM;
    esp_err_t ret = config_manager_get(cfg);
    if (ret == ESP_OK) {
        cfg->buzzer_enable = button;
        cfg->buzzer_alarm_enable = alarm;
        ret = update_save_apply(cfg, CONFIG_APPLY_LCD);
    }
    free(cfg);
    return ret;
}

/* Key-beep row (buzzer_enable): a click on every menu button. It moved out of
 * its own BUZZER submenu (the submenu held a single toggle) into DISPLAY & KEYS,
 * which now owns every panel-behaviour knob. OK toggles and persists
 * immediately; the row refreshes on the next render. The two flags (Beep here,
 * Alarm Sound under ALARM SETTINGS) share one save call, so each toggle passes
 * the other's current value through unchanged. */
static void key_beep_value(lcd_menu_t *m, const lcd_menu_item_t *it,
                           char *buf, size_t buf_size, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    snprintf(buf, buf_size, "<%s>  ", s_buzzer_button_val ? "ON" : "OFF");
}
static esp_err_t key_beep_toggle(lcd_menu_t *m, const lcd_menu_item_t *it, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    bool v = !s_buzzer_button_val;
    if (save_buzzer_preferences(v, s_buzzer_alarm_val) == ESP_OK) {
        s_buzzer_button_val = v;
        s_buzzer_button = v;
    }
    return ESP_OK;
}
/* "Sound" row: audible alarm beeps (buzzer_alarm_enable). It lives under
 * ALARM SETTINGS — one home per setting — but persists through
 * save_buzzer_preferences() because that is the single writer of both flags. */
static void alarm_sound_value(lcd_menu_t *m, const lcd_menu_item_t *it,
                              char *buf, size_t buf_size, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    snprintf(buf, buf_size, "<%s>  ", s_buzzer_alarm_val ? "ON" : "OFF");
}
static esp_err_t alarm_sound_toggle(lcd_menu_t *m, const lcd_menu_item_t *it, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    bool v = !s_buzzer_alarm_val;
    if (save_buzzer_preferences(s_buzzer_button_val, v) == ESP_OK) {
        s_buzzer_alarm_val = v;
        s_buzzer_alarm = v;
    }
    return ESP_OK;
}

/* DISPLAY & KEYS: every panel-behaviour knob in one screen. BUZZER used to be its
 * own submenu holding a single toggle — one screen for one option. Display owns
 * the screen half (auto rotate, sleep) and Beep the key half; the audible ALARM
 * flag lives with the other alarm options instead. Title is 14 chars: the
 * renderer reserves room for the "n/N" badge, so >16 gets clipped. */
static const lcd_menu_item_t s_items_display[] = {
    {.label = "Auto Cycle", .type = LCD_MENU_ITEM_VALUE,
     .value_get = display_cycle_value, .action = menu_autocycle},
    {.label = "Sleep",      .type = LCD_MENU_ITEM_VALUE,
     .value_get = display_sleep_value, .action = menu_auto_off},
    {.label = "Beep",       .type = LCD_MENU_ITEM_VALUE,
     .value_get = key_beep_value, .action = key_beep_toggle},
    {.label = "Back",         .type = LCD_MENU_ITEM_BACK},
};
static const lcd_menu_screen_t s_screen_display = {
    .title = "DISPLAY & KEYS",
    .items = s_items_display,
    .item_count = sizeof(s_items_display) / sizeof(s_items_display[0]),
};

/* One macro for all alarm rows: each is a VALUE item showing its current value
 * inline ("<ON>", "<220V>", "<5s>", ...) with OK dispatched to menu_alarm_config
 * on the field id. Enables toggle in place; numeric fields open the step editor. */
#define ALARM_ITEM(label_, field_) \
    {.label = label_, .type = LCD_MENU_ITEM_VALUE, .value_get = alarm_value_get, \
     .action = menu_alarm_config, .user_data = (void *)(uintptr_t)(field_)}

static const lcd_menu_item_t s_items_alarm_enable[] = {
    ALARM_ITEM("Voltage Low", ALARM_CFG_VLOW_EN),
    ALARM_ITEM("Voltage High", ALARM_CFG_VHIGH_EN),
    ALARM_ITEM("Over Current", ALARM_CFG_OC_EN),
    ALARM_ITEM("Phase Loss", ALARM_CFG_PHASE_EN),
    ALARM_ITEM("Frequency", ALARM_CFG_FREQ_EN),
    {.label = "Back", .type = LCD_MENU_ITEM_BACK},
};
static const lcd_menu_screen_t s_screen_alarm_enable = {
    .title = "ALARM DETECT", .items = s_items_alarm_enable,
    .item_count = sizeof(s_items_alarm_enable) / sizeof(s_items_alarm_enable[0]),
};

/* Voltage thresholds are only meaningful once a line-to-neutral / line-to-line
 * reference is chosen, which is not implemented yet (alarm_voltage_reference has
 * no consumer). The labels say so plainly instead of the old "(TBD)" placeholder
 * shipped to the operator. */
static const lcd_menu_item_t s_items_alarm_threshold[] = {
    ALARM_ITEM("V Low", ALARM_CFG_VLOW),
    ALARM_ITEM("V High", ALARM_CFG_VHIGH),
    ALARM_ITEM("Over Current", ALARM_CFG_OC),
    ALARM_ITEM("Frequency Low", ALARM_CFG_FLOW),
    ALARM_ITEM("Frequency High", ALARM_CFG_FHIGH),
    {.label = "Back", .type = LCD_MENU_ITEM_BACK},
};
static const lcd_menu_screen_t s_screen_alarm_threshold = {
    .title = "LIMITS", .items = s_items_alarm_threshold,
    .item_count = sizeof(s_items_alarm_threshold) / sizeof(s_items_alarm_threshold[0]),
};

/* Nominal Frequency is intentionally absent: energy_meter_set_line_freq() owns
 * alarm_nominal_frequency_hz (it rewrites the mirror on every Meter Setup > LFreq
 * toggle), so a second UI path here would silently fight the first. Set the grid
 * frequency under Meter Setup; this branch only keeps the alarm-specific timing
 * and the audible-alarm flag.
 *
 * Hysteresis has no row: the field stays at its internal default (deci 10 =
 * 1.0 unit) because an operator has no reason to tune it — the register and the
 * edit path remain for engineering use. */
static const lcd_menu_item_t s_items_alarm_settings[] = {
    {.label = "Detect", .type = LCD_MENU_ITEM_SUBMENU, .submenu = &s_screen_alarm_enable},
    {.label = "Limits", .type = LCD_MENU_ITEM_SUBMENU, .submenu = &s_screen_alarm_threshold},
    ALARM_ITEM("Alarm Delay", ALARM_CFG_TRIGGER),
    ALARM_ITEM("Reset Delay", ALARM_CFG_CLEAR),
    {.label = "Sound", .type = LCD_MENU_ITEM_VALUE,
     .value_get = alarm_sound_value, .action = alarm_sound_toggle},
    {.label = "Back", .type = LCD_MENU_ITEM_BACK},
};
static const lcd_menu_screen_t s_screen_alarm_settings = {
    .title = "ALARM SETTINGS", .items = s_items_alarm_settings,
    .item_count = sizeof(s_items_alarm_settings) / sizeof(s_items_alarm_settings[0]),
};

/* Calibration sub-menu — developer-only. Exposed (non-static) so the
 * engineering-mode menu (hmi_test_task.c) can link it as a submenu.
 * Calib Info is intentionally omitted: the same Wiring/Freq/PGA essentials are
 * already shown on the production home/status pages. */
static const lcd_menu_item_t s_items_calibration[] = {
    {.label = "Export to SD",   .type = LCD_MENU_ITEM_ACTION, .action = menu_calib_export_sd},
    {.label = "Load from SD",   .type = LCD_MENU_ITEM_ACTION, .action = menu_calib_import_sd},
    {.label = "Back",           .type = LCD_MENU_ITEM_BACK},
};
const lcd_menu_screen_t home_screen_calibration_screen = {
    .title = "CALIBRATION", .items = s_items_calibration,
    .item_count = sizeof(s_items_calibration) / sizeof(s_items_calibration[0]),
};

/* Meter Setup: wiring + line freq show their current value inline and toggle
 * straight on OK (no submenu, no confirm); CT Setup still opens the multi-field
 * draft editor. */
static const lcd_menu_item_t s_items_meter_setup[] = {
    {.label = "WMode",     .type = LCD_MENU_ITEM_VALUE, .value_get = wiring_mode_value, .action = menu_wiring_mode},
    {.label = "LFreq",     .type = LCD_MENU_ITEM_VALUE, .value_get = line_freq_value,   .action = menu_line_freq},
    {.label = "CT Setup",  .type = LCD_MENU_ITEM_ACTION, .action = menu_current_ct},
    {.label = "Back",        .type = LCD_MENU_ITEM_BACK},
};
static const lcd_menu_screen_t s_screen_meter_setup = {
    .title = "METER SETUP",
    .items = s_items_meter_setup,
    .item_count = sizeof(s_items_meter_setup) / sizeof(s_items_meter_setup[0]),
};

/* RTU Master submenu: Device list browses the configured downstream meters (the
 * old Info leaf), Active toggles the bus and each slot, and Poll edits the bus
 * cadence in whole seconds (5..60, same window as the web portal dropdown). */
#define HOME_RTU_POLL_MIN_S (CONFIG_MANAGER_MB_POLL_PERIOD_MIN_MS / 1000U)
#define HOME_RTU_POLL_MAX_S (CONFIG_MANAGER_MB_POLL_PERIOD_MAX_MS / 1000U)

static void rtu_poll_value(lcd_menu_t *m, const lcd_menu_item_t *it,
                           char *buf, size_t buf_size, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    const config_manager_t *c = cfg_view();
    if (c == NULL) { snprintf(buf, buf_size, "< ?>  "); return; }
    snprintf(buf, buf_size, "<%lus>  ",
             (unsigned long)(c->mb_poll_period_ms / 1000U));
}

static esp_err_t rtu_poll_edit(lcd_menu_t *m, const lcd_menu_item_t *it, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    const config_manager_t *c = cfg_view();
    if (c == NULL) return ESP_ERR_INVALID_STATE;
    uint32_t seconds = c->mb_poll_period_ms / 1000U;
    if (!edit_setting_seconds("RTU POLL", seconds,
                              HOME_RTU_POLL_MIN_S, HOME_RTU_POLL_MAX_S,
                              &seconds)) {
        return ESP_OK;   /* LEFT: discard, row keeps the old value */
    }
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) return ESP_ERR_NO_MEM;
    *cfg = *c;
    cfg->mb_poll_period_ms = seconds * 1000U;
    (void)update_save_apply(cfg, CONFIG_APPLY_MODBUS_MASTER);
    free(cfg);
    return ESP_OK;
}

static const lcd_menu_item_t s_items_rtu_master[] = {
    {.label = "Device list", .type = LCD_MENU_ITEM_ACTION, .action = menu_rtu_info},
    {.label = "Active",      .type = LCD_MENU_ITEM_ACTION, .action = menu_rtu_active},
    {.label = "Poll",        .type = LCD_MENU_ITEM_VALUE,
     .value_get = rtu_poll_value, .action = rtu_poll_edit},
    {.label = "Back",        .type = LCD_MENU_ITEM_BACK},
};
static const lcd_menu_screen_t s_screen_rtu_master = {
    .title = "RTU MASTER",
    .items = s_items_rtu_master,
    .item_count = sizeof(s_items_rtu_master) / sizeof(s_items_rtu_master[0]),
};

/* RTU Slave: ID and Baud show their value inline and open a single-step editor on
 * OK. The old Info leaf (a strict duplicate of these two values) and the two-row
 * cursor editor are gone. A saved change prompts for the reboot that makes it
 * live — the esp-modbus link has no safe hot rebuild. 8N1 is fixed. */
static const lcd_menu_item_t s_items_rtu_slave[] = {
    {.label = "Slave ID", .type = LCD_MENU_ITEM_VALUE,
     .value_get = rtu_slave_id_value, .action = menu_rtu_slave_set_id},
    {.label = "Baud",     .type = LCD_MENU_ITEM_VALUE,
     .value_get = rtu_slave_baud_value, .action = menu_rtu_slave_set_baud},
    {.label = "Back",     .type = LCD_MENU_ITEM_BACK},
};
static const lcd_menu_screen_t s_screen_rtu_slave = {
    .title = "RTU SLAVE",
    .items = s_items_rtu_slave,
    .item_count = sizeof(s_items_rtu_slave) / sizeof(s_items_rtu_slave[0]),
};

/* MQTT: the operator's only control over telemetry. Status is the device-wide
 * enable (the web portal deliberately has no such control — it configures the
 * broker, this toggle decides whether it is used at all), and Period is the
 * publish cadence in whole seconds, 5..60 to match the guard in
 * config_manager_update(). CONFIG_APPLY_MQTT rebuilds the client
 * asynchronously, so both rows take effect without a reboot; apply does not
 * notify the home screen, so Status also refreshes the s_mqtt_enabled mirror
 * that the home page renders. */
#define HOME_MQTT_PERIOD_MIN_S (CONFIG_MANAGER_MQTT_PERIOD_MIN_MS / 1000U)
#define HOME_MQTT_PERIOD_MAX_S (CONFIG_MANAGER_MQTT_PERIOD_MAX_MS / 1000U)

static void mqtt_status_value(lcd_menu_t *m, const lcd_menu_item_t *it,
                              char *buf, size_t buf_size, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    const config_manager_t *c = cfg_view();
    if (c == NULL) { snprintf(buf, buf_size, "< ?>  "); return; }
    snprintf(buf, buf_size, "<%s>  ", c->mqtt.enable ? "ON" : "OFF");
}

static esp_err_t mqtt_status_toggle(lcd_menu_t *m, const lcd_menu_item_t *it, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    const config_manager_t *c = cfg_view();
    if (c == NULL) return ESP_ERR_INVALID_STATE;
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) return ESP_ERR_NO_MEM;
    *cfg = *c;
    cfg->mqtt.enable = !cfg->mqtt.enable;
    esp_err_t ret = update_save_apply(cfg, CONFIG_APPLY_MQTT);
    if (ret == ESP_OK) {
        /* CONFIG_APPLY_MQTT does not raise s_cfg_pending, and only the
         * backlight is applied locally — keep the home-page mirror honest. */
        s_mqtt_enabled = cfg->mqtt.enable;
    }
    free(cfg);
    return ESP_OK;
}

static void mqtt_period_value(lcd_menu_t *m, const lcd_menu_item_t *it,
                              char *buf, size_t buf_size, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    const config_manager_t *c = cfg_view();
    if (c == NULL) { snprintf(buf, buf_size, "< ?>  "); return; }
    snprintf(buf, buf_size, "<%lus>  ",
             (unsigned long)(c->mqtt_publish_ms / 1000U));
}

static esp_err_t mqtt_period_edit(lcd_menu_t *m, const lcd_menu_item_t *it, void *ctx)
{
    (void)m; (void)it; (void)ctx;
    const config_manager_t *c = cfg_view();
    if (c == NULL) return ESP_ERR_INVALID_STATE;
    uint32_t seconds = c->mqtt_publish_ms / 1000U;
    if (!edit_setting_seconds("MQTT PERIOD", seconds,
                              HOME_MQTT_PERIOD_MIN_S, HOME_MQTT_PERIOD_MAX_S,
                              &seconds)) {
        return ESP_OK;   /* LEFT: discard, row keeps the old value */
    }
    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) return ESP_ERR_NO_MEM;
    *cfg = *c;
    cfg->mqtt_publish_ms = seconds * 1000U;
    (void)update_save_apply(cfg, CONFIG_APPLY_MQTT);
    free(cfg);
    return ESP_OK;
}

static const char *mqtt_tls_mode_str(mqtt_tls_mode_t mode)
{
    switch (mode) {
    case MQTT_TLS_DISABLE: return "None";
    case MQTT_TLS_CA_ONLY: return "TLS (CA)";
    case MQTT_TLS_MUTUAL: return "TLS (Mutual)";
    case MQTT_TLS_INSECURE: return "TLS (Insecure)";
    default: return "Unknown";
    }
}

static esp_err_t mqtt_info_show(lcd_menu_t *m, const lcd_menu_item_t *it, void *ctx)
{
    (void)m; (void)it; (void)ctx;

    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL || config_manager_get(cfg) != ESP_OK) {
        if (cfg) free(cfg);
        show_info("MQTT INFO", "Config Error", "", "");
        return ESP_ERR_NO_MEM;
    }

    /* Prepare info lines (label + value pairs) */
    char lines[5][HOME_LCD_WIDTH + 1];
    int total_lines = 0;

    /* Line 0: Broker Name (truncate to fit) */
    snprintf(lines[total_lines++], sizeof(lines[0]), "Name: %.13s", cfg->mqtt.name);

    /* Line 1: Server Address (truncate to fit) */
    snprintf(lines[total_lines++], sizeof(lines[0]), "Addr: %.13s", cfg->mqtt.broker);

    /* Line 2: Port */
    snprintf(lines[total_lines++], sizeof(lines[0]), "Port: %u", cfg->mqtt.port);

    /* Line 3: TLS Mode */
    snprintf(lines[total_lines++], sizeof(lines[0]), "TLS: %.14s", mqtt_tls_mode_str(cfg->mqtt.tls_mode));

    /* Line 4: Username */
    snprintf(lines[total_lines++], sizeof(lines[0]), "User: %.13s",
             cfg->mqtt.username[0] ? cfg->mqtt.username : "(none)");

    free(cfg);

    /* Multi-page navigation (3 lines per page, reserve 2 chars for indicators) */
    int cursor = 0;
    int top = 0;
    uint8_t previous = 0;
    wait_buttons_released();

    while (1) {
        put_line_centre(0, "MQTT INFO");

        /* Calculate pagination */
        bool has_above = (top > 0);
        bool has_below = (top + 3 < total_lines);

        for (int row = 0; row < 3; row++) {
            int idx = top + row;
            uint8_t lcd_row = (uint8_t)(row + 1);

            if (idx >= total_lines) {
                put_line(lcd_row, "");
                continue;
            }

            /* Build display line with cursor and indent */
            char display[HOME_LCD_WIDTH + 1];
            char cursor_char = (idx == cursor) ? '>' : ' ';

            /* Cursor + 1 space indent + content (truncate to fit 18 chars total) */
            snprintf(display, sizeof(display), "%c %.16s", cursor_char, lines[idx]);

            /* Add indicator if needed (reserve last 2 chars) */
            bool mark = false;
            if (row == 0 && has_above) {
                mark = true;
            } else if (row == 2 && has_below) {
                mark = true;
            }

            if (mark) {
                display[18] = '|';
                display[19] = ' ';
                display[20] = '\0';
            }

            put_line(lcd_row, display);
        }

        /* Button handling */
        uint8_t buttons = 0;
        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) {
            buttons = 0;
        }
        uint8_t edges = buttons & ~previous;
        previous = buttons;

        if (edges & HMI_BSP_BUTTON_TOP) {
            if (cursor > 0) {
                cursor--;
                if (cursor < top) {
                    top = cursor;
                }
            }
            button_click();
        } else if (edges & HMI_BSP_BUTTON_BOTTOM) {
            if (cursor < total_lines - 1) {
                cursor++;
                if (cursor >= top + 3) {
                    top = cursor - 2;
                }
            }
            button_click();
        } else if (edges & HMI_BSP_BUTTON_CENTER) {
            /* CENTER = exit */
            button_click();
            wait_buttons_released();
            return ESP_OK;
        } else if (edges & HMI_BSP_BUTTON_LEFT) {
            /* LEFT = back/exit */
            button_click();
            wait_buttons_released();
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static const lcd_menu_item_t s_items_mqtt[] = {
    {.label = "Info",   .type = LCD_MENU_ITEM_ACTION, .action = mqtt_info_show},
    {.label = "Status", .type = LCD_MENU_ITEM_VALUE,
     .value_get = mqtt_status_value, .action = mqtt_status_toggle},
    {.label = "Period", .type = LCD_MENU_ITEM_VALUE,
     .value_get = mqtt_period_value, .action = mqtt_period_edit},
    {.label = "Back",   .type = LCD_MENU_ITEM_BACK},
};
static const lcd_menu_screen_t s_screen_mqtt = {
    .title = "MQTT",
    .items = s_items_mqtt,
    .item_count = sizeof(s_items_mqtt) / sizeof(s_items_mqtt[0]),
};

/* Settings root — order matches agreed IA.
 * Config Portal, TCP Server and Factory Reset are direct ACTIONs, not submenus:
 * each has exactly one real destination, so the submenu level only added a
 * "pick the one option" screen. The destructive/disruptive ones confirm inside
 * their own callback. BUZZER went the same way once its Alarm row moved to
 * ALARM SETTINGS — the single remaining key-beep toggle now lives inside
 * DISPLAY & KEYS, so one toggle does not cost a screen of its own. */
static const lcd_menu_item_t s_items_settings[] = {
    {.label = "Meter Setup",     .type = LCD_MENU_ITEM_SUBMENU, .submenu = &s_screen_meter_setup},
    {.label = "Config Portal",   .type = LCD_MENU_ITEM_ACTION,  .action = menu_portal_start},
    {.label = "RTU Master",      .type = LCD_MENU_ITEM_SUBMENU, .submenu = &s_screen_rtu_master},
    {.label = "RTU Slave",       .type = LCD_MENU_ITEM_SUBMENU, .submenu = &s_screen_rtu_slave},
    {.label = "MQTT",            .type = LCD_MENU_ITEM_SUBMENU, .submenu = &s_screen_mqtt},
    {.label = "TCP Server",      .type = LCD_MENU_ITEM_ACTION,  .action = menu_tcp_server},
    {.label = "Alarm Settings",  .type = LCD_MENU_ITEM_SUBMENU, .submenu = &s_screen_alarm_settings},
    {.label = "Display & Keys",  .type = LCD_MENU_ITEM_SUBMENU, .submenu = &s_screen_display},
    {.label = "Factory Reset",   .type = LCD_MENU_ITEM_ACTION,  .action = menu_factory_reset},
    {.label = "Back",            .type = LCD_MENU_ITEM_BACK},
};
static const lcd_menu_screen_t s_screen_settings = {
    .title = "SETTINGS",
    .items = s_items_settings,
    .item_count = sizeof(s_items_settings) / sizeof(s_items_settings[0]),
};

/* Main menu root */
static const lcd_menu_item_t s_menu_items[] = {
    {.label = "Device Info",   .type = LCD_MENU_ITEM_ACTION,  .action = menu_device_info},
    {.label = "Settings",      .type = LCD_MENU_ITEM_SUBMENU, .submenu = &s_screen_settings},
    {.label = "Back",          .type = LCD_MENU_ITEM_BACK,    .action = menu_back},
};
static const lcd_menu_screen_t s_menu_root = {
    .title = "MENU",
    .items = s_menu_items,
    .item_count = sizeof(s_menu_items) / sizeof(s_menu_items[0]),
};

/* Map hardware button edges to lcd_menu keys. */
static lcd_menu_key_t button_to_key(uint8_t button)
{
    if (button & HMI_BSP_BUTTON_TOP)    return LCD_MENU_KEY_UP;
    if (button & HMI_BSP_BUTTON_BOTTOM) return LCD_MENU_KEY_DOWN;
    if (button & HMI_BSP_BUTTON_LEFT)   return LCD_MENU_KEY_LEFT;
    if (button & HMI_BSP_BUTTON_RIGHT)  return LCD_MENU_KEY_RIGHT;
    if (button & HMI_BSP_BUTTON_CENTER) return LCD_MENU_KEY_OK;
    return LCD_MENU_KEY_NONE;
}

/* ============================================================
 * MAIN TASK
 * ============================================================ */

static void home_screen_task(void *arg)
{
    (void)arg;

    home_mode_t mode = HOME_MODE_MAIN;
    int main_page = 0;
    int status_page = 0;
    uint8_t last_buttons = 0;
    uint32_t autocycle_ms = 0;
    uint32_t refresh_ms = 0;
    uint32_t status_timeout_ms = 0;
    uint32_t led_ms = 0;

    lcd_menu_t menu;
    lcd_menu_config_t menu_cfg = {
        .width = HOME_LCD_WIDTH,
        .height = HOME_LCD_HEIGHT,
        .pointer_char = '>',
        .wrap_cursor = true,
        .show_scroll_markers = true,
        .ask_save_on_exit = false,
        .show_position_counter = true,
        .write_line = menu_write_line,
    };

    ESP_LOGI(TAG, "Home screen started");
    lcd_settings_reload();

    /* Initial render. */
    k_main_pages[main_page].render();

    while (1) {
        /* ---- Config reload request ---- */
        if (s_cfg_pending) {
            s_cfg_pending = false;
            lcd_settings_reload();
        }

        /* ---- Read buttons ---- */
        uint8_t buttons = 0;
        if (hmi_bsp_read_buttons(&buttons) != ESP_OK) buttons = 0;
        uint8_t edges = buttons & ~last_buttons;
        last_buttons = buttons;

        lcd_idle_tick(edges != 0);
        if (edges != 0) button_click();

        /* ---- Alarm + LED periodic ----
         * alarm_tick() drives the alarm beep/blink timing. update_leds() is
         * refreshed on a fixed cadence that is finer than the 1 Hz status-blink
         * half-period (HOME_LED_BLINK_HALF_MS) so the time-derived phase renders
         * as a smooth blink rather than an aliased one. */
        alarm_tick();
        led_ms += HOME_POLL_MS;
        if (led_ms >= HOME_LED_REFRESH_MS) {
            led_ms = 0;
            update_leds();
        }

        /* ========== MODE: MAIN ========================================== */
        if (mode == HOME_MODE_MAIN) {
            bool redraw = false;

            /* LEFT/RIGHT → enter Status View */
            if (edges & HMI_BSP_BUTTON_LEFT) {
                mode = HOME_MODE_STATUS;
                status_page = 0;
                status_timeout_ms = 0;
                k_status_pages[status_page].render();
                vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
                continue;
            } else if (edges & HMI_BSP_BUTTON_RIGHT) {
                mode = HOME_MODE_STATUS;
                status_page = 0;
                status_timeout_ms = 0;
                k_status_pages[status_page].render();
                vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
                continue;
            } else if (edges & HMI_BSP_BUTTON_CENTER) {
                /* CENTER → enter Menu */
                mode = HOME_MODE_MENU;
                s_menu_exit = false;
                cfg_view_invalidate();   /* fresh values on entry */
                lcd_menu_init(&menu, &menu_cfg, &s_menu_root);
                lcd_menu_render(&menu);
                last_buttons = buttons;
                vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
                continue;
            } else if (edges & HMI_BSP_BUTTON_TOP) {
                /* TOP → prev page (manual) */
                main_page = (main_page == 0) ? (MAIN_PAGE_COUNT - 1) : (main_page - 1);
                autocycle_ms = 0;
                redraw = true;
            } else if (edges & HMI_BSP_BUTTON_BOTTOM) {
                /* BOTTOM → next page (manual) */
                main_page = (main_page + 1) % MAIN_PAGE_COUNT;
                autocycle_ms = 0;
                redraw = true;
            }

            /* Auto-cycle (only when autocycle enabled). */
            if (s_autocycle) {
                autocycle_ms += HOME_POLL_MS;
                if (autocycle_ms >= s_cycle_time_ms) {
                    main_page = (main_page + 1) % MAIN_PAGE_COUNT;
                    autocycle_ms = 0;
                    redraw = true;
                }
            }

            /* Periodic live refresh. */
            refresh_ms += HOME_POLL_MS;
            if (refresh_ms >= HOME_REFRESH_MS) {
                redraw = true;
                refresh_ms = 0;
            }

            if (redraw) {
                k_main_pages[main_page].render();
            }

        /* ========== MODE: STATUS ======================================== */
        } else if (mode == HOME_MODE_STATUS) {
            bool redraw = false;
            status_timeout_ms += HOME_POLL_MS;

            /* I/O page: handle output control (TOP/BOTTOM select, CENTER toggles). */
            if (status_page == 4) {  /* I/O is index 4 in k_status_pages */
                if (edges & HMI_BSP_BUTTON_TOP) {
                    s_io_mode = (s_io_mode == IO_MODE_VIEW) ? IO_MODE_OUT0_SEL :
                                (s_io_mode == IO_MODE_OUT0_SEL) ? IO_MODE_OUT1_SEL : IO_MODE_OUT0_SEL;
                    status_timeout_ms = 0;
                    redraw = true;
                } else if (edges & HMI_BSP_BUTTON_BOTTOM) {
                    s_io_mode = (s_io_mode == IO_MODE_VIEW) ? IO_MODE_OUT0_SEL :
                                (s_io_mode == IO_MODE_OUT0_SEL) ? IO_MODE_OUT1_SEL : IO_MODE_OUT0_SEL;
                    status_timeout_ms = 0;
                    redraw = true;
                } else if (edges & HMI_BSP_BUTTON_CENTER) {
                    if (s_io_mode == IO_MODE_OUT0_SEL) {
                        bool level = false;
                        if (io_expander_get_out0(&level) == ESP_OK) {
                            io_expander_set_out0(!level);
                        }
                        status_timeout_ms = 0;
                        redraw = true;
                    } else if (s_io_mode == IO_MODE_OUT1_SEL) {
                        bool level = false;
                        if (io_expander_get_out1(&level) == ESP_OK) {
                            io_expander_set_out1(!level);
                        }
                        status_timeout_ms = 0;
                        redraw = true;
                    } else {
                        /* VIEW mode: CENTER acts as BACK */
                        s_io_mode = IO_MODE_VIEW;
                        mode = HOME_MODE_MAIN;
                        autocycle_ms = 0;
                        refresh_ms = 0;
                        k_main_pages[main_page].render();
                        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
                        continue;
                    }
                }
            }

            if (edges & HMI_BSP_BUTTON_LEFT) {
                status_page = (status_page == 0)
                    ? (STATUS_PAGE_COUNT - 1) : (status_page - 1);
                status_timeout_ms = 0;
                s_io_mode = IO_MODE_VIEW;
                redraw = true;
            } else if (edges & HMI_BSP_BUTTON_RIGHT) {
                status_page = (status_page + 1) % STATUS_PAGE_COUNT;
                status_timeout_ms = 0;
                s_io_mode = IO_MODE_VIEW;
                redraw = true;
            } else if ((edges & HMI_BSP_BUTTON_CENTER) && status_page != 4) {
                /* BACK to main (except on I/O page where CENTER toggles) */
                s_io_mode = IO_MODE_VIEW;
                mode = HOME_MODE_MAIN;
                autocycle_ms = 0;
                refresh_ms = 0;
                k_main_pages[main_page].render();
                vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
                continue;
            } else if ((edges & (HMI_BSP_BUTTON_TOP | HMI_BSP_BUTTON_BOTTOM)) && status_page != 4) {
                /* TOP and BOTTOM both act as BACK (except on I/O page) */
                s_io_mode = IO_MODE_VIEW;
                mode = HOME_MODE_MAIN;
                autocycle_ms = 0;
                refresh_ms = 0;
                k_main_pages[main_page].render();
                vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
                continue;
            }

            /* Timeout → return to Main auto-cycle. */
            if (status_timeout_ms >= HOME_STATUS_TIMEOUT_MS) {
                s_io_mode = IO_MODE_VIEW;
                mode = HOME_MODE_MAIN;
                autocycle_ms = 0;
                refresh_ms = 0;
                k_main_pages[main_page].render();
                vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
                continue;
            }

            refresh_ms += HOME_POLL_MS;
            if (refresh_ms >= HOME_REFRESH_MS) {
                redraw = true;
                refresh_ms = 0;
            }

            if (redraw) {
                k_status_pages[status_page].render();
            }

        /* ========== MODE: MENU ========================================== */
        } else {
            lcd_menu_key_t key = button_to_key(edges);
            if (key != LCD_MENU_KEY_NONE) {
                bool at_root = (lcd_menu_get_current_screen(&menu) == &s_menu_root);
                if (key == LCD_MENU_KEY_BACK && at_root) {
                    s_menu_exit = true;
                } else {
                    lcd_menu_handle_key(&menu, key);
                    /* An action may have changed config; drop the cached view so
                     * this render re-reads it and inline VALUE rows show fresh
                     * values. Covers every write path, including ones that save
                     * directly without going through update_save_apply(). */
                    cfg_view_invalidate();
                    lcd_menu_render(&menu);
                }
            }

            if (s_menu_exit) {
                mode = HOME_MODE_MAIN;
                autocycle_ms = 0;
                refresh_ms = 0;
                k_main_pages[main_page].render();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(HOME_POLL_MS));
    }
}

/* ============================================================
 * PUBLIC API
 * ============================================================ */

esp_err_t home_screen_start(void)
{
    if (s_started) return ESP_OK;

    BaseType_t ok = xTaskCreate(home_screen_task,
                                "home_screen_task",
                                CONFIG_APP_HMI_TASK_STACK_SIZE,
                                NULL,
                                CONFIG_APP_HMI_TASK_PRIORITY,
                                NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "create home_screen_task failed");
    s_started = true;
    return ESP_OK;
}

esp_err_t home_screen_apply_config(void)
{
    ESP_RETURN_ON_FALSE(s_started, ESP_ERR_INVALID_STATE, TAG, "home screen not started");
    s_cfg_pending = true;
    return ESP_OK;
}
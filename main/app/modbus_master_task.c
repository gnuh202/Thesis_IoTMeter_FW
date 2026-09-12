#include "modbus_master_task.h"

#include <stdlib.h>
#include <string.h>
#include "config_manager.h"
#include "config_store.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbcontroller.h"
#include "modbus_meters.h"
#include "sdkconfig.h"
#include "system_status.h"

/*
 * Modbus RTU master — multi-slot.
 *
 * One shared RS485 bus (baud/parity/period). Each configured slot is polled
 * round-robin with a device-specific decode path into meter_readings_t.
 * Offline tracking is per-slot so one dead slave does not block the others.
 */

#ifndef CONFIG_APP_MB_MASTER_UART_PORT
#define CONFIG_APP_MB_MASTER_UART_PORT 2
#endif
#ifndef CONFIG_APP_MB_MASTER_RXD_GPIO
#define CONFIG_APP_MB_MASTER_RXD_GPIO 11
#endif
#ifndef CONFIG_APP_MB_MASTER_TXD_GPIO
#define CONFIG_APP_MB_MASTER_TXD_GPIO 12
#endif
#ifndef CONFIG_APP_MB_MASTER_TASK_STACK_SIZE
#define CONFIG_APP_MB_MASTER_TASK_STACK_SIZE 5120
#endif
#ifndef CONFIG_APP_MB_MASTER_TASK_PRIORITY
#define CONFIG_APP_MB_MASTER_TASK_PRIORITY 6
#endif

#define MB_MASTER_OFFLINE_THRESHOLD 5
#define MB_MASTER_OFFLINE_POLL_MS 30000
/* Wake often enough that LCD/console Bus OFF is applied without waiting a full
 * poll period (or the 30 s offline backoff). */
#define MB_MASTER_RECFG_POLL_MS 50

static const char *TAG = "modbus_master";

typedef struct {
    bool used;
    bool enabled;
    uint8_t type;
    uint8_t slave_id;
    char name[CONFIG_MANAGER_MB_NAME_LEN];
} mb_slot_cfg_t;

typedef struct {
    bool bus_enabled;
    uint8_t baud_code;
    uint8_t parity_code;
    uint32_t poll_period_ms;
    mb_slot_cfg_t slots[MODBUS_MASTER_SLOT_COUNT];
} mb_runtime_cfg_t;

typedef struct {
    meter_readings_t readings;
    bool readings_valid;
    bool online;
    uint32_t poll_count;
    uint32_t error_count;
} mb_slot_rt_t;

static void *s_master_handler;
static bool s_task_started;
static bool s_stack_up;

static SemaphoreHandle_t s_lock;
static mb_runtime_cfg_t s_cfg;
static mb_slot_rt_t s_slot_rt[MODBUS_MASTER_SLOT_COUNT];
static uint32_t s_cycle_count;
static volatile bool s_reconfigure_pending;

/* Sleep up to total_ms, but return early when a reconfigure is requested so
 * Bus/slot enable changes take effect within ~MB_MASTER_RECFG_POLL_MS. */
static void delay_interruptible(uint32_t total_ms)
{
    uint32_t left = total_ms;
    while (left > 0) {
        if (s_reconfigure_pending) {
            return;
        }
        uint32_t chunk = left > MB_MASTER_RECFG_POLL_MS ? MB_MASTER_RECFG_POLL_MS : left;
        vTaskDelay(pdMS_TO_TICKS(chunk));
        left -= chunk;
    }
}

static uint32_t baud_from_code(uint8_t code)
{
    switch (code) {
    case 1: return 19200;
    case 2: return 38400;
    case 3: return 57600;
    case 4: return 115200;
    default: return 9600;
    }
}

static uart_parity_t parity_from_code(uint8_t code)
{
    switch (code) {
    case 1: return UART_PARITY_EVEN;
    case 2: return UART_PARITY_ODD;
    default: return UART_PARITY_DISABLE;
    }
}

static void master_stack_stop(void)
{
    if (s_stack_up) {
        mbc_master_destroy();
        s_stack_up = false;
        s_master_handler = NULL;
    }
}

/*
 * Bring up the RTU master stack for the bus. Multi-slot polling uses raw
 * mbc_master_send_request (FC03) so no CID descriptor table is required —
 * each request carries its own slave address.
 */
static esp_err_t master_stack_start(const mb_runtime_cfg_t *cfg)
{
    esp_log_level_set("MB_CONTROLLER_MASTER", ESP_LOG_NONE);

    ESP_RETURN_ON_ERROR(mbc_master_init(MB_PORT_SERIAL_MASTER, &s_master_handler), TAG,
                        "master init failed");

    mb_communication_info_t comm = {
        .mode = MB_MODE_RTU,
        .port = CONFIG_APP_MB_MASTER_UART_PORT,
        .baudrate = baud_from_code(cfg->baud_code),
        .parity = parity_from_code(cfg->parity_code),
    };
    ESP_RETURN_ON_ERROR(mbc_master_setup(&comm), TAG, "master setup failed");

    ESP_RETURN_ON_ERROR(uart_set_pin(CONFIG_APP_MB_MASTER_UART_PORT,
                                     CONFIG_APP_MB_MASTER_TXD_GPIO,
                                     CONFIG_APP_MB_MASTER_RXD_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "set master UART pins failed");

    ESP_RETURN_ON_ERROR(mbc_master_start(), TAG, "master start failed");
    /* RS485 direction control is handled by the on-board auto-direction
     * transceiver — there is no DE/RE pin to drive from the ESP32. The HAL
     * UART_MODE_RS485_HALF_DUPLEX mode is for designs that wire DE/RE to the
     * RTS pin; using it here without an RTS pin would no-op or risk TX/RX
     * loopback, so we stay in plain UART mode. */
    ESP_LOGI(TAG, "master UART in normal mode (auto-direction RS485, no DE/RE pin)");

    s_stack_up = true;

    unsigned used = 0;
    for (unsigned i = 0; i < MODBUS_MASTER_SLOT_COUNT; i++) {
        if (cfg->slots[i].used) {
            used++;
        }
    }
    ESP_LOGI(TAG, "RTU master started: bus baud=%u parity=%u slots=%u port=%d RX=%d TX=%d",
             (unsigned)baud_from_code(cfg->baud_code), (unsigned)cfg->parity_code, used,
             CONFIG_APP_MB_MASTER_UART_PORT, CONFIG_APP_MB_MASTER_RXD_GPIO,
             CONFIG_APP_MB_MASTER_TXD_GPIO);
    return ESP_OK;
}

/* Read N holding registers starting at datasheet 1-based decimal address. */
static esp_err_t mb_read_holding(uint8_t slave_addr, uint16_t reg_dec, uint16_t n, uint16_t *out)
{
    mb_param_request_t req = {
        .slave_addr = slave_addr,
        .command = 0x03,
        .reg_start = (uint16_t)(reg_dec - 1U),
        .reg_size = n,
    };
    return mbc_master_send_request(&req, out);
}

/* IEEE-754 float from two Modbus registers, high word first (ABCD). */
static float float_from_regs_abcd(uint16_t hi, uint16_t lo)
{
    uint32_t bits = ((uint32_t)hi << 16) | (uint32_t)lo;
    float v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

static void commit_slot(uint8_t slot, const meter_readings_t *local, bool ok)
{
    bool went_online = false;
    bool went_offline = false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    mb_slot_rt_t *rt = &s_slot_rt[slot];
    rt->poll_count++;
    if (ok) {
        rt->readings = *local;
        rt->readings_valid = true;
        rt->error_count = 0;
        if (!rt->online) {
            rt->online = true;
            went_online = true;
        }
    } else {
        rt->error_count++;
        if (rt->error_count == MB_MASTER_OFFLINE_THRESHOLD && rt->online) {
            rt->online = false;
            went_offline = true;
        } else if (rt->error_count >= MB_MASTER_OFFLINE_THRESHOLD) {
            rt->online = false;
        }
    }

    /* Aggregate module status: READY if any online; OFFLINE if bus on but none. */
    bool any_online = false;
    bool any_enabled = false;
    for (unsigned i = 0; i < MODBUS_MASTER_SLOT_COUNT; i++) {
        if (s_cfg.slots[i].used && s_cfg.slots[i].enabled) {
            any_enabled = true;
            if (s_slot_rt[i].online) {
                any_online = true;
            }
        }
    }
    xSemaphoreGive(s_lock);

    if (went_online) {
        ESP_LOGI(TAG, "slot %u -> ONLINE (id=%u %s)", (unsigned)slot,
                 (unsigned)s_cfg.slots[slot].slave_id,
                 modbus_meters_device_name((meter_device_t)s_cfg.slots[slot].type));
    } else if (went_offline) {
        ESP_LOGW(TAG, "slot %u -> OFFLINE (id=%u)", (unsigned)slot,
                 (unsigned)s_cfg.slots[slot].slave_id);
    }

    if (!s_cfg.bus_enabled) {
        system_status_set(SYS_MODULE_RS485_MASTER, SYS_STATUS_OFFLINE);
    } else if (!any_enabled) {
        system_status_set(SYS_MODULE_RS485_MASTER, SYS_STATUS_WARNING);
    } else if (any_online) {
        system_status_set(SYS_MODULE_RS485_MASTER, SYS_STATUS_READY);
    } else {
        /* still probing or all offline */
        bool any_hard_offline = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (unsigned i = 0; i < MODBUS_MASTER_SLOT_COUNT; i++) {
            if (s_cfg.slots[i].used && s_cfg.slots[i].enabled &&
                s_slot_rt[i].error_count >= MB_MASTER_OFFLINE_THRESHOLD) {
                any_hard_offline = true;
                break;
            }
        }
        xSemaphoreGive(s_lock);
        system_status_set(SYS_MODULE_RS485_MASTER,
                          any_hard_offline ? SYS_STATUS_OFFLINE : SYS_STATUS_INIT);
    }
}

/*
 * PM710: float metered-data area (Appendix B Table B-2).
 * Power is kW/kVAR/kVA on the wire → convert to W/var/VA (*1000).
 * Energy is already kWh.
 */
static void poll_pm710_slot(uint8_t slot, uint8_t addr)
{
    meter_readings_t local = {0};
    uint32_t ok = 0;
    uint32_t fail = 0;
    uint16_t buf[2];

    struct {
        uint16_t reg;
        float *dst;
        float scale; /* multiply after float decode */
    } map[] = {
        {PM710_REG_V_AN, &local.voltage[0], 1.0f},
        {PM710_REG_V_BN, &local.voltage[1], 1.0f},
        {PM710_REG_V_CN, &local.voltage[2], 1.0f},
        {PM710_REG_I_A, &local.current[0], 1.0f},
        {PM710_REG_I_B, &local.current[1], 1.0f},
        {PM710_REG_I_C, &local.current[2], 1.0f},
        {PM710_REG_P_TOTAL_KW, &local.active_power, 1000.0f},
        {PM710_REG_Q_TOTAL_KVAR, &local.reactive_power, 1000.0f},
        {PM710_REG_S_TOTAL_KVA, &local.apparent_power, 1000.0f},
        {PM710_REG_PF_TOTAL, &local.power_factor, 1.0f},
        {PM710_REG_FREQ_HZ, &local.frequency, 1.0f},
        {PM710_REG_ENERGY_KWH, &local.active_energy, 1.0f},
    };

    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (mb_read_holding(addr, map[i].reg, 2, buf) == ESP_OK) {
            *map[i].dst = float_from_regs_abcd(buf[0], buf[1]) * map[i].scale;
            ok++;
        } else {
            fail++;
        }
    }
    (void)fail;
    commit_slot(slot, &local, ok > 0);
}

/*
 * EM-07K: U16 + VTR/CTR scale; energy float32 per phase (Wh) summed → kWh.
 */
static void poll_em07k_slot(uint8_t slot, uint8_t addr)
{
    meter_readings_t local = {0};
    uint32_t ok = 0;
    uint32_t fail = 0;

    uint16_t ratios[2] = {0};
    uint16_t v[3] = {0};
    uint16_t i[3] = {0};
    uint16_t p[3] = {0};
    uint16_t s[3] = {0};
    uint16_t f = 0;
    uint16_t e[6] = {0}; /* L1 hi/lo, L2 hi/lo, L3 hi/lo */

    if (mb_read_holding(addr, EM07K_REG_VTR, 2, ratios) == ESP_OK) {
        ok++;
    } else {
        fail++;
    }
    if (mb_read_holding(addr, EM07K_REG_VOLT_L1, 3, v) == ESP_OK) {
        ok++;
    } else {
        fail++;
    }
    if (mb_read_holding(addr, EM07K_REG_CURR_L1, 3, i) == ESP_OK) {
        ok++;
    } else {
        fail++;
    }
    if (mb_read_holding(addr, EM07K_REG_ACTIVE_L1, 3, p) == ESP_OK) {
        ok++;
    } else {
        fail++;
    }
    if (mb_read_holding(addr, EM07K_REG_APPARENT_L1, 3, s) == ESP_OK) {
        ok++;
    } else {
        fail++;
    }
    if (mb_read_holding(addr, EM07K_REG_FREQ_L1, 1, &f) == ESP_OK) {
        ok++;
    } else {
        fail++;
    }
    if (mb_read_holding(addr, EM07K_REG_ENERGY_L1_HI, 6, e) == ESP_OK) {
        ok++;
    } else {
        fail++;
    }

    float vtr = ratios[0] ? (float)ratios[0] : 1.0f;
    float ctr = ratios[1] ? (float)ratios[1] : 1.0f;

    for (int k = 0; k < 3; k++) {
        local.voltage[k] = (float)v[k] * 0.1f * vtr;
        local.current[k] = (float)i[k] * 0.01f * ctr;
    }
    /* Datasheet: per-phase Watt/VA × CTR × VTR; sum phases for totals (W/VA). */
    local.active_power = ((float)p[0] + (float)p[1] + (float)p[2]) * ctr * vtr;
    local.apparent_power = ((float)s[0] + (float)s[1] + (float)s[2]) * ctr * vtr;
    local.frequency = (float)f * 0.1f;
    /* No Q / PF on EM-07K. */

    float energy_wh = 0.0f;
    for (int ph = 0; ph < 3; ph++) {
        energy_wh += float_from_regs_abcd(e[ph * 2], e[ph * 2 + 1]);
    }
    local.active_energy = energy_wh / 1000.0f;

    (void)fail;
    commit_slot(slot, &local, ok > 0);
}

static void poll_slot(uint8_t slot)
{
    const mb_slot_cfg_t *sc = &s_cfg.slots[slot];
    if (!sc->used || !sc->enabled) {
        return;
    }
    if (sc->type == METER_DEV_EM07K) {
        poll_em07k_slot(slot, sc->slave_id);
    } else {
        poll_pm710_slot(slot, sc->slave_id);
    }
}

static void load_runtime_config(mb_runtime_cfg_t *out)
{
    memset(out, 0, sizeof(*out));

    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg != NULL && config_manager_get(cfg) == ESP_OK) {
        /* Master-bus-owned fields only. cfg->mb_slave_id / mb_slave_baud_code
         * configure this device's OWN slave link (UART1) and are deliberately
         * not read here. */
        out->bus_enabled = cfg->mb_enabled;
        out->baud_code = cfg->mb_baud_code;
        out->parity_code = cfg->mb_parity_code;
        out->poll_period_ms = cfg->mb_poll_period_ms;
        for (unsigned i = 0; i < MODBUS_MASTER_SLOT_COUNT; i++) {
            out->slots[i].used = cfg->mb_slots[i].used;
            out->slots[i].enabled = cfg->mb_slots[i].enabled;
            out->slots[i].type = cfg->mb_slots[i].type;
            out->slots[i].slave_id = cfg->mb_slots[i].slave_id;
            strlcpy(out->slots[i].name, cfg->mb_slots[i].name, sizeof(out->slots[i].name));
        }
        free(cfg);
        return;
    }
    if (cfg != NULL) {
        free(cfg);
    }

    /* Legacy single-device domain fallback (empty slots if no addr configured). */
    config_ext_meter_t ext;
    config_store_get_ext_meter(&ext);
    out->bus_enabled = ext.enabled;
    out->baud_code = ext.baud_code;
    out->parity_code = ext.parity_code;
    out->poll_period_ms = ext.poll_period_ms;
    if (ext.slave_addr >= 1U && ext.slave_addr <= 247U) {
        out->slots[0].used = true;
        out->slots[0].enabled = ext.enabled;
        out->slots[0].type = ext.device;
        out->slots[0].slave_id = ext.slave_addr;
        strlcpy(out->slots[0].name,
                ext.device == METER_DEV_EM07K ? "EM-07K" : "PM710",
                sizeof(out->slots[0].name));
    }
}

static void apply_config_snapshot(const mb_runtime_cfg_t *cfg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cfg = *cfg;
    for (unsigned i = 0; i < MODBUS_MASTER_SLOT_COUNT; i++) {
        s_slot_rt[i].online = false;
        s_slot_rt[i].error_count = 0;
        /* Keep last readings_valid data until overwritten — operators can still
         * glance at stale values after a brief reconfigure. */
    }
    xSemaphoreGive(s_lock);

    unsigned used = 0;
    for (unsigned i = 0; i < MODBUS_MASTER_SLOT_COUNT; i++) {
        if (cfg->slots[i].used) {
            used++;
        }
    }

    system_status_state_t state = !cfg->bus_enabled ? SYS_STATUS_OFFLINE :
                                  (used > 0 ? SYS_STATUS_INIT : SYS_STATUS_WARNING);
    system_status_set(SYS_MODULE_RS485_MASTER, state);
}

static void modbus_master_task(void *arg)
{
    (void)arg;

    while (1) {
        if (s_reconfigure_pending) {
            s_reconfigure_pending = false;
            /* Stop UART/stack first so TX LED stops immediately on Bus OFF. */
            master_stack_stop();

            mb_runtime_cfg_t cfg;
            load_runtime_config(&cfg);
            apply_config_snapshot(&cfg);

            if (cfg.bus_enabled) {
                esp_err_t ret = master_stack_start(&cfg);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "reconfigure start failed: %s", esp_err_to_name(ret));
                    master_stack_stop();
                    system_status_set(SYS_MODULE_RS485_MASTER, SYS_STATUS_ERROR);
                }
            } else {
                ESP_LOGI(TAG, "master bus disabled by config");
            }
        }

        if (s_stack_up && s_cfg.bus_enabled) {
            for (uint8_t i = 0; i < MODBUS_MASTER_SLOT_COUNT; i++) {
                /* Abort mid-cycle if LCD/console toggled Bus/slot enable. */
                if (s_reconfigure_pending) {
                    break;
                }
                if (s_cfg.slots[i].used && s_cfg.slots[i].enabled) {
                    poll_slot(i);
                }
            }
            if (!s_reconfigure_pending) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                s_cycle_count++;
                xSemaphoreGive(s_lock);
            }
        }

        /* Idle when bus is off: short sleep so next enable is snappy. */
        if (!s_cfg.bus_enabled || !s_stack_up) {
            delay_interruptible(MB_MASTER_RECFG_POLL_MS);
            continue;
        }

        uint32_t period = s_cfg.poll_period_ms > 0 ? s_cfg.poll_period_ms : 2000;
        /* If every enabled slot is hard-offline, slow the whole cycle. */
        bool all_offline = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_stack_up && s_cfg.bus_enabled) {
            unsigned enabled = 0;
            unsigned hard_off = 0;
            for (unsigned i = 0; i < MODBUS_MASTER_SLOT_COUNT; i++) {
                if (s_cfg.slots[i].used && s_cfg.slots[i].enabled) {
                    enabled++;
                    if (!s_slot_rt[i].online &&
                        s_slot_rt[i].error_count >= MB_MASTER_OFFLINE_THRESHOLD) {
                        hard_off++;
                    }
                }
            }
            all_offline = (enabled > 0 && hard_off == enabled);
        }
        xSemaphoreGive(s_lock);
        if (all_offline) {
            period = MB_MASTER_OFFLINE_POLL_MS;
        }
        /* Chunked delay: Bus OFF during the 2s/30s wait applies within ~50 ms. */
        delay_interruptible(period);
    }
}

esp_err_t modbus_master_reconfigure(void)
{
    ESP_RETURN_ON_FALSE(s_task_started, ESP_ERR_INVALID_STATE, TAG, "master task not started");
    s_reconfigure_pending = true;
    return ESP_OK;
}

uint8_t modbus_master_slot_capacity(void)
{
    return MODBUS_MASTER_SLOT_COUNT;
}

esp_err_t modbus_master_get_readings_slot(uint8_t slot, meter_readings_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(slot < MODBUS_MASTER_SLOT_COUNT, ESP_ERR_INVALID_ARG, TAG, "bad slot");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool valid = s_slot_rt[slot].readings_valid && s_cfg.slots[slot].used;
    if (valid) {
        *out = s_slot_rt[slot].readings;
    }
    xSemaphoreGive(s_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t modbus_master_get_slot_status(uint8_t slot, modbus_master_slot_status_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(slot < MODBUS_MASTER_SLOT_COUNT, ESP_ERR_INVALID_ARG, TAG, "bad slot");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(out, 0, sizeof(*out));
    out->used = s_cfg.slots[slot].used;
    out->enabled = s_cfg.slots[slot].enabled;
    out->type = s_cfg.slots[slot].type;
    out->slave_id = s_cfg.slots[slot].slave_id;
    strlcpy(out->name, s_cfg.slots[slot].name, sizeof(out->name));
    out->online = s_slot_rt[slot].online;
    out->readings_valid = s_slot_rt[slot].readings_valid;
    out->poll_count = s_slot_rt[slot].poll_count;
    out->error_count = s_slot_rt[slot].error_count;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t modbus_master_get_status(modbus_master_status_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(out, 0, sizeof(*out));
    out->enabled = s_cfg.bus_enabled;
    out->stack_up = s_stack_up;
    out->cycle_count = s_cycle_count;

    int first = -1;
    for (unsigned i = 0; i < MODBUS_MASTER_SLOT_COUNT; i++) {
        if (!s_cfg.slots[i].used) {
            continue;
        }
        out->slot_count++;
        if (s_slot_rt[i].online) {
            out->online_count++;
        }
        if (first < 0) {
            first = (int)i;
        }
    }
    if (first >= 0) {
        out->device = (meter_device_t)s_cfg.slots[first].type;
        out->online = s_slot_rt[first].online;
        out->configured = true;
        out->poll_count = s_slot_rt[first].poll_count;
        out->error_count = s_slot_rt[first].error_count;
    } else {
        out->configured = false;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t modbus_master_get_readings(meter_readings_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");

    /* Prefer first used+enabled+online slot; else first used with valid data. */
    int pick = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (unsigned i = 0; i < MODBUS_MASTER_SLOT_COUNT; i++) {
        if (s_cfg.slots[i].used && s_cfg.slots[i].enabled && s_slot_rt[i].online &&
            s_slot_rt[i].readings_valid) {
            pick = (int)i;
            break;
        }
    }
    if (pick < 0) {
        for (unsigned i = 0; i < MODBUS_MASTER_SLOT_COUNT; i++) {
            if (s_cfg.slots[i].used && s_slot_rt[i].readings_valid) {
                pick = (int)i;
                break;
            }
        }
    }
    bool ok = false;
    if (pick >= 0) {
        *out = s_slot_rt[pick].readings;
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t modbus_master_task_start(void)
{
    /* The RTU master task is always built. Whether it actually polls the bus
     * depends on the runtime config: cfg.mb_enabled (LCD/Web Portal/console) +
     * having at least one configured slot. The Kconfig has no "enable" flag
     * because the only meaningful switch is the bus enable in NVS. */
    if (s_task_started) {
        return ESP_OK;
    }

    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "create mutex failed");
    }

    mb_runtime_cfg_t cfg;
    load_runtime_config(&cfg);
    apply_config_snapshot(&cfg);

    if (cfg.bus_enabled) {
        esp_err_t ret = master_stack_start(&cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "master start failed: %s; task idle, retry on reconfigure",
                     esp_err_to_name(ret));
            master_stack_stop();
            system_status_set(SYS_MODULE_RS485_MASTER, SYS_STATUS_ERROR);
        }
    } else {
        ESP_LOGI(TAG, "RTU master bus disabled by config; task idle (no polling)");
        system_status_set(SYS_MODULE_RS485_MASTER, SYS_STATUS_INIT);
    }

    BaseType_t ok = xTaskCreate(modbus_master_task,
                                "modbus_master_task",
                                CONFIG_APP_MB_MASTER_TASK_STACK_SIZE,
                                NULL,
                                CONFIG_APP_MB_MASTER_TASK_PRIORITY,
                                NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_FAIL, TAG, "create master task failed");
    s_task_started = true;
    return ESP_OK;
}

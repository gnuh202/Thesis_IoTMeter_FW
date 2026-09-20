#include "modbus_slave_task.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "config_manager.h"
#include "driver/uart.h"
#include "energy_meter_task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "io_expander.h"
#include "mbcontroller.h"
#include "network_manager.h"
#include "sdkconfig.h"
#include "system_status.h"
#include "time_source.h"

/*
 * RTU transport follows the known-good uart_echo (4) implementation: ESP-IDF
 * RS485 half-duplex mode, fixed UART pins and a single owner task.  The owner
 * task is also the only context allowed to destroy/rebuild esp-modbus.
 */
#define MB_INPUT_REG_COUNT 114
#define MB_HOLDING_REG_COUNT 8
#define MB_REBOOT_MAGIC 0x5AA5
#define MB_REGISTER_REFRESH_MS 100

#define IR_VOLTAGE_A 0
#define IR_CURRENT_A 6
#define IR_CURRENT_N 12
#define IR_ACTIVE_A 14
#define IR_ACTIVE_TOTAL 20
#define IR_REACTIVE_A 22
#define IR_REACTIVE_TOTAL 28
#define IR_APPARENT_A 30
#define IR_APPARENT_TOTAL 36
#define IR_PF_A 38
#define IR_PF_TOTAL 44
#define IR_ANGLE_A 46
#define IR_FREQUENCY 52
#define IR_TEMPERATURE 54
#define IR_SYS_STATUS0 56
#define IR_SYS_STATUS1 57
#define IR_METER_STATUS0 58
#define IR_METER_STATUS1 59
#define IR_ENERGY_AI 60
#define IR_ENERGY_AE 62
#define IR_ENERGY_RI 64
#define IR_ENERGY_RE 66
#define IR_CURRENT_PEAK_A 68
#define IR_DEMAND 74
#define IR_DEMAND_MAX 76
#define IR_DEVICE_ID 100
#define IR_FW_VERSION 101
#define IR_HW_VERSION 102
#define IR_MEASURE_VALID 103
#define IR_UPTIME_H 104
#define IR_UPTIME_L 105

#define HR_WIRING_MODE 0
#define HR_LINE_FREQ_SEL 1
#define HR_DEMAND_WINDOW 2
#define HR_RESET_ENERGY 3
#define HR_RESET_DEMAND 4
#define HR_LAST_COMMAND 5
#define HR_LAST_RESULT 6
#define HR_REBOOT 7

#define IR_COMMAND_SEQUENCE 106
#define IR_COMMAND_ADDRESS 107
#define IR_COMMAND_ERROR 108
#define IR_COMMAND_STATUS 109
/* Wall clock, appended at the end so no existing address shifts. EPOCH is a
 * uint32 big-endian pair (H first, like IR_UPTIME). TIME_QUALITY carries the
 * ASCII flag from time_source: 'U' uptime-only, 'E' estimate, 'S' synced — a
 * master MUST check it before trusting EPOCH, which reads from 1970 until an
 * RTC is fitted. BOOT_COUNT separates power cycles while that is the case. */
#define IR_EPOCH_H 110
#define IR_EPOCH_L 111
#define IR_TIME_QUALITY 112
#define IR_BOOT_COUNT 113
#define MB_COMMAND_PENDING 1U
#define MB_COMMAND_SUCCESS 2U
#define MB_COMMAND_FAILED 3U
static const char *TAG = "modbus_slave";
static uint16_t s_input_regs[MB_INPUT_REG_COUNT];
static uint16_t s_holding_regs[MB_HOLDING_REG_COUNT];
static uint8_t s_coils;
static uint8_t s_discrete;
static TaskHandle_t s_owner_task;
static SemaphoreHandle_t s_reconfigure_done;
static SemaphoreHandle_t s_lifecycle_mutex;
static volatile bool s_stack_up;
static volatile bool s_reconfigure_pending;
static volatile esp_err_t s_reconfigure_result;
static config_manager_t s_active_cfg;
static bool s_active_cfg_valid;
static bool s_verbose_logging;
static uint32_t s_refresh_count;
static uint32_t s_command_sequence;
static uint32_t s_read_events;
static uint32_t s_events_holding_rd, s_events_holding_wr, s_events_input_rd;
static uint32_t s_events_coils_rd, s_events_coils_wr, s_events_discrete_rd;
static int64_t s_last_event_us;

static void store_float(uint16_t offset, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    s_input_regs[offset] = (uint16_t)(bits >> 16);
    s_input_regs[offset + 1] = (uint16_t)bits;
}

static void modbus_refresh_inputs(void)
{
    atm90e32as_measurements_t m;
    bool valid = energy_meter_get_latest(&m) == ESP_OK;
    s_input_regs[IR_MEASURE_VALID] = valid ? 1U : 0U;
    if (valid) {
        for (int i = 0; i < ATM90E32AS_PHASE_COUNT; ++i) {
            store_float(IR_VOLTAGE_A + i * 2, m.voltage[i]);
            store_float(IR_CURRENT_A + i * 2, m.current[i]);
            store_float(IR_ACTIVE_A + i * 2, m.active_power[i]);
            store_float(IR_REACTIVE_A + i * 2, m.reactive_power[i]);
            store_float(IR_APPARENT_A + i * 2, m.apparent_power[i]);
            store_float(IR_PF_A + i * 2, m.power_factor[i]);
            store_float(IR_ANGLE_A + i * 2, m.phase_angle[i]);
            store_float(IR_CURRENT_PEAK_A + i * 2, m.current_peak[i]);
        }
        store_float(IR_CURRENT_N, m.current_neutral);
        store_float(IR_ACTIVE_TOTAL, m.total_active_power);
        store_float(IR_REACTIVE_TOTAL, m.total_reactive_power);
        store_float(IR_APPARENT_TOTAL, m.total_apparent_power);
        store_float(IR_PF_TOTAL, m.total_power_factor);
        store_float(IR_FREQUENCY, m.frequency);
        store_float(IR_TEMPERATURE, m.temperature);
        s_input_regs[IR_SYS_STATUS0] = m.sys_status0;
        s_input_regs[IR_SYS_STATUS1] = m.sys_status1;
        s_input_regs[IR_METER_STATUS0] = m.meter_status0;
        s_input_regs[IR_METER_STATUS1] = m.meter_status1;
    }
    energy_meter_energy_t energy;
    if (energy_meter_get_energy(&energy) == ESP_OK) {
        store_float(IR_ENERGY_AI, energy.active_import_kwh);
        store_float(IR_ENERGY_AE, energy.active_export_kwh);
        store_float(IR_ENERGY_RI, energy.reactive_import_kvarh);
        store_float(IR_ENERGY_RE, energy.reactive_export_kvarh);
    }
    energy_meter_demand_t demand;
    if (energy_meter_get_demand(&demand) == ESP_OK) {
        store_float(IR_DEMAND, demand.active_power_demand_w);
        store_float(IR_DEMAND_MAX, demand.active_power_demand_max_w);
    }
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    s_input_regs[IR_UPTIME_H] = (uint16_t)(uptime_s >> 16);
    s_input_regs[IR_UPTIME_L] = (uint16_t)uptime_s;

    uint32_t epoch = (uint32_t)time_source_now();
    s_input_regs[IR_EPOCH_H] = (uint16_t)(epoch >> 16);
    s_input_regs[IR_EPOCH_L] = (uint16_t)epoch;
    s_input_regs[IR_TIME_QUALITY] = (uint16_t)(uint8_t)time_source_quality_char();
    s_input_regs[IR_BOOT_COUNT] = (uint16_t)time_source_boot_count();
}

static void modbus_refresh_runtime_holding(void)
{
    uint16_t window;
    if (energy_meter_get_demand_window_minutes(&window) == ESP_OK) {
        s_holding_regs[HR_DEMAND_WINDOW] = window;
    }
    atm90e32as_calib_t calib;
    if (energy_meter_get_calibration(&calib) == ESP_OK) {
        s_holding_regs[HR_WIRING_MODE] =
            calib.wiring_mode == ATM90E32AS_WIRING_3P3W ? 1U : 0U;
        s_holding_regs[HR_LINE_FREQ_SEL] =
            calib.line_freq == ATM90E32AS_LINE_FREQ_60HZ ? 1U : 0U;
    }
}

static void modbus_refresh_io(void)
{
    bool in0, in1, out0, out1;
    if (io_expander_get_in0(&in0) == ESP_OK && io_expander_get_in1(&in1) == ESP_OK) {
        s_discrete = (in0 ? 0x01U : 0U) | (in1 ? 0x02U : 0U);
    }
    if (io_expander_get_out0(&out0) == ESP_OK && io_expander_get_out1(&out1) == ESP_OK) {
        s_coils = (out0 ? 0x01U : 0U) | (out1 ? 0x02U : 0U);
    }
}

static esp_err_t modbus_apply_coils(void)
{
    esp_err_t err0 = io_expander_set_out0((s_coils & 0x01U) != 0);
    esp_err_t err1 = io_expander_set_out1((s_coils & 0x02U) != 0);
    return err0 != ESP_OK ? err0 : err1;
}

static bool range_contains(uint16_t start, uint16_t count, uint16_t reg)
{
    return count != 0U && reg >= start && (uint32_t)reg < (uint32_t)start + count;
}

static void modbus_record_command(uint16_t address, esp_err_t result)
{
    s_holding_regs[HR_LAST_COMMAND] = address;
    s_holding_regs[HR_LAST_RESULT] = result == ESP_OK ? 0U : 1U;
    s_input_regs[IR_COMMAND_SEQUENCE] = (uint16_t)++s_command_sequence;
    s_input_regs[IR_COMMAND_ADDRESS] = address;
    s_input_regs[IR_COMMAND_ERROR] = result == ESP_OK ? 0U : (uint16_t)(-result);
    s_input_regs[IR_COMMAND_STATUS] = result == ESP_OK ? MB_COMMAND_SUCCESS : MB_COMMAND_FAILED;
}

static void slave_stack_destroy(void)
{
    if (!s_stack_up) return;
    /* The esp-modbus destroy path deletes its worker task immediately. Give the
     * scheduler time to complete that deletion before the event group/UART can
     * be reused; otherwise the old worker can call xEventGroupSetBits() on the
     * freed event-group object during a live rebuild. */
    (void)uart_wait_tx_done(CONFIG_APP_MB_SLAVE_UART_PORT, pdMS_TO_TICKS(200));
    vTaskDelay(pdMS_TO_TICKS(5));
    esp_err_t err = mbc_slave_destroy();
    s_stack_up = false;
    vTaskDelay(pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Modbus stack destroy failed: %s", esp_err_to_name(err));
    }
}

static bool valid_slave_id(uint8_t value) { return value >= 1U && value <= 247U; }
static bool valid_baud_code(uint8_t value) { return value <= 4U; }
uint32_t modbus_slave_baud_from_code(uint8_t code)
{
    static const uint32_t rates[] = {9600, 19200, 38400, 57600, 115200};
    return code < 5U ? rates[code] : rates[0];
}

static esp_err_t slave_stack_start(const config_manager_t *cfg)
{
    modbus_refresh_runtime_holding();
    modbus_refresh_inputs();
    modbus_refresh_io();
    s_input_regs[IR_DEVICE_ID] = 0x9032;
    s_input_regs[IR_FW_VERSION] = 0x0100;
    s_input_regs[IR_HW_VERSION] = 0x0100;

    void *handler = NULL;
    ESP_RETURN_ON_ERROR(mbc_slave_init(MB_PORT_SERIAL_SLAVE, &handler), TAG, "init modbus slave failed");
    /* Own link settings: mb_slave_id + mb_slave_baud_code, edited only on the
     * LCD (Settings > RTU Slave). The master bus fields (mb_baud_code /
     * mb_parity_code) belong to UART2 and must not reach this UART1 link.
     * Framing is fixed 8N1 — the register-map doc already publishes parity
     * None / 1 stop as constants with no config path. */
    uint8_t baud_code = valid_baud_code(cfg->mb_slave_baud_code) ? cfg->mb_slave_baud_code : 0U;
    mb_communication_info_t comm = {
        .mode = MB_MODE_RTU, .slave_addr = valid_slave_id(cfg->mb_slave_id) ? cfg->mb_slave_id : CONFIG_APP_MB_SLAVE_ADDR,
        .port = CONFIG_APP_MB_SLAVE_UART_PORT,
        .baudrate = modbus_slave_baud_from_code(baud_code),
        .parity = MB_PARITY_NONE,
    };
    esp_err_t err = mbc_slave_setup(&comm);
    if (err != ESP_OK) { mbc_slave_destroy(); return err; }
    esp_log_level_set("MB_CONTROLLER_SLAVE", s_verbose_logging ? ESP_LOG_DEBUG : ESP_LOG_NONE);

    mb_register_area_descriptor_t area = {.start_offset = 0};
    area.type = MB_PARAM_INPUT; area.address = s_input_regs; area.size = sizeof(s_input_regs);
    if ((err = mbc_slave_set_descriptor(area)) != ESP_OK) goto fail;
    area.type = MB_PARAM_HOLDING; area.address = s_holding_regs; area.size = sizeof(s_holding_regs);
    if ((err = mbc_slave_set_descriptor(area)) != ESP_OK) goto fail;
    area.type = MB_PARAM_COIL; area.address = &s_coils; area.size = sizeof(s_coils);
    if ((err = mbc_slave_set_descriptor(area)) != ESP_OK) goto fail;
    area.type = MB_PARAM_DISCRETE; area.address = &s_discrete; area.size = sizeof(s_discrete);
    if ((err = mbc_slave_set_descriptor(area)) != ESP_OK) goto fail;
    if ((err = mbc_slave_start()) != ESP_OK) goto fail;
    if ((err = uart_set_pin(CONFIG_APP_MB_SLAVE_UART_PORT, CONFIG_APP_MB_SLAVE_TXD_GPIO,
                            CONFIG_APP_MB_SLAVE_RXD_GPIO, UART_PIN_NO_CHANGE,
                            UART_PIN_NO_CHANGE)) != ESP_OK) goto fail;
    if ((err = uart_set_mode(CONFIG_APP_MB_SLAVE_UART_PORT, UART_MODE_RS485_HALF_DUPLEX)) != ESP_OK) goto fail;
    s_stack_up = true;
    s_active_cfg = *cfg;
    s_active_cfg_valid = true;
    system_status_set(SYS_MODULE_RS485_SLAVE, SYS_STATUS_READY);
    ESP_LOGI(TAG, "RTU slave ready: addr=%u baud=%lu parity=none", (unsigned)comm.slave_addr,
             (unsigned long)comm.baudrate);
    return ESP_OK;
fail:
    mbc_slave_destroy();
    return err;
}

static esp_err_t slave_rebuild_from_current_config(void)
{
    config_manager_t requested;
    esp_err_t err = config_manager_get(&requested);
    if (err != ESP_OK) return err;
    slave_stack_destroy();
    err = slave_stack_start(&requested);
    if (err == ESP_OK) return ESP_OK;
    ESP_LOGE(TAG, "new RTU configuration failed (%s); restoring previous link", esp_err_to_name(err));
    if (s_active_cfg_valid) {
        (void)config_manager_update(&s_active_cfg);
        (void)config_manager_save();
        (void)slave_stack_start(&s_active_cfg);
    } else {
        system_status_set(SYS_MODULE_RS485_SLAVE, SYS_STATUS_ERROR);
    }
    return err;
}

static esp_err_t process_holding_write(const mb_param_info_t *info, uint16_t *out_cmd_addr)
{
    esp_err_t result = ESP_OK;
    uint16_t start = info->mb_offset;
    uint16_t count = (uint16_t)info->size;
    if (range_contains(start, count, HR_LAST_COMMAND) ||
        range_contains(start, count, HR_LAST_RESULT)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (range_contains(start, count, HR_WIRING_MODE)) {
        *out_cmd_addr = HR_WIRING_MODE;
        if (s_holding_regs[HR_WIRING_MODE] <= 1U) {
            esp_err_t err = energy_meter_set_wiring_mode(s_holding_regs[HR_WIRING_MODE] ? ATM90E32AS_WIRING_3P3W : ATM90E32AS_WIRING_3P4W, true);
            if (err != ESP_OK) result = err;
        } else {
            result = ESP_ERR_INVALID_ARG;
        }
        modbus_refresh_runtime_holding();
    }
    if (range_contains(start, count, HR_LINE_FREQ_SEL)) {
        *out_cmd_addr = HR_LINE_FREQ_SEL;
        if (s_holding_regs[HR_LINE_FREQ_SEL] <= 1U) {
            esp_err_t err = energy_meter_set_line_freq(s_holding_regs[HR_LINE_FREQ_SEL] ? ATM90E32AS_LINE_FREQ_60HZ : ATM90E32AS_LINE_FREQ_50HZ, true);
            if (err != ESP_OK) result = err;
        } else {
            result = ESP_ERR_INVALID_ARG;
        }
        modbus_refresh_runtime_holding();
    }
    if (range_contains(start, count, HR_DEMAND_WINDOW)) {
        *out_cmd_addr = HR_DEMAND_WINDOW;
        if (s_holding_regs[HR_DEMAND_WINDOW] != 0U) {
            esp_err_t err = energy_meter_set_demand_window_minutes(s_holding_regs[HR_DEMAND_WINDOW]);
            if (err != ESP_OK) result = err;
        } else {
            result = ESP_ERR_INVALID_ARG;
        }
        modbus_refresh_runtime_holding();
    }
    if (range_contains(start, count, HR_RESET_ENERGY) && s_holding_regs[HR_RESET_ENERGY] != 0U) {
        *out_cmd_addr = HR_RESET_ENERGY;
        s_holding_regs[HR_RESET_ENERGY] = 0U;
        esp_err_t err = energy_meter_reset_energy();
        if (err != ESP_OK) result = err;
    }
    if (range_contains(start, count, HR_RESET_DEMAND) && s_holding_regs[HR_RESET_DEMAND] != 0U) {
        *out_cmd_addr = HR_RESET_DEMAND;
        s_holding_regs[HR_RESET_DEMAND] = 0U;
        esp_err_t err = energy_meter_reset_demand();
        if (err != ESP_OK) result = err;
    }
    if (range_contains(start, count, HR_REBOOT)) {
        *out_cmd_addr = HR_REBOOT;
        if (s_holding_regs[HR_REBOOT] == MB_REBOOT_MAGIC) {
            s_holding_regs[HR_REBOOT] = 0U;
            /* Commit the RAM energy accumulators first — see
             * energy_meter_flush_persist(). */
            energy_meter_flush_persist();
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else {
            s_holding_regs[HR_REBOOT] = 0U;
            result = ESP_ERR_INVALID_ARG;
        }
    }
    return result;
}

static void modbus_slave_task(void *arg)
{
    (void)arg;
    TickType_t last_refresh = xTaskGetTickCount();
    while (true) {
        if (s_reconfigure_pending) {
            s_reconfigure_pending = false;
            s_reconfigure_result = slave_rebuild_from_current_config();
            if (s_reconfigure_done != NULL) xSemaphoreGive(s_reconfigure_done);
            continue;
        }
        /* Config portal active: the operator is doing settings. Pause frame
         * servicing (cooperative; resumes within ~50 ms of portal close).
         * The reconfigure branch above still runs, so an Apply made during
         * the portal is never lost. */
        if (network_manager_is_config_mode()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        /* Do not block indefinitely in mbc_slave_check_event(): a reconfigure
         * request must be able to take ownership even while the bus is idle.
         * The controller callback has already queued mb_param_info_t for each
         * access; timeout polling consumes that queue and bounds reconfigure
         * latency to one refresh period. MB_CONTROLLER_SLAVE is muted when this
         * normal empty-queue timeout is reported by the library. */
        mb_param_info_t info;
        if (mbc_slave_get_param_info(&info, pdMS_TO_TICKS(MB_REGISTER_REFRESH_MS)) == ESP_OK) {
            s_last_event_us = esp_timer_get_time();
            if (info.type & MB_EVENT_HOLDING_REG_RD) { ++s_events_holding_rd; ++s_read_events; }
            if (info.type & MB_EVENT_HOLDING_REG_WR) {
                ++s_events_holding_wr;
                uint16_t cmd_addr = info.mb_offset;
                esp_err_t holding_err = process_holding_write(&info, &cmd_addr);
                modbus_record_command(cmd_addr, holding_err);
            }
            if (info.type & MB_EVENT_INPUT_REG_RD) { ++s_events_input_rd; ++s_read_events; }
            if (info.type & MB_EVENT_COILS_RD) { ++s_events_coils_rd; ++s_read_events; }
            if (info.type & MB_EVENT_COILS_WR) {
            ++s_events_coils_wr;
            esp_err_t coil_err = modbus_apply_coils();
            modbus_record_command(info.mb_offset, coil_err);
        }
            if (info.type & MB_EVENT_DISCRETE_RD) { ++s_events_discrete_rd; ++s_read_events; }
        }
        if ((xTaskGetTickCount() - last_refresh) >= pdMS_TO_TICKS(MB_REGISTER_REFRESH_MS)) {
            last_refresh = xTaskGetTickCount();
            modbus_refresh_inputs(); modbus_refresh_runtime_holding(); modbus_refresh_io(); ++s_refresh_count;
        }
    }
}

esp_err_t modbus_slave_task_start(void)
{
    if (s_owner_task != NULL) return ESP_OK;
    config_manager_t cfg;
    if (config_manager_get(&cfg) != ESP_OK) {
        memset(&cfg, 0, sizeof(cfg)); cfg.mb_slave_id = CONFIG_APP_MB_SLAVE_ADDR;
    }
    memset(s_input_regs, 0, sizeof(s_input_regs)); memset(s_holding_regs, 0, sizeof(s_holding_regs));
    s_reconfigure_done = xSemaphoreCreateBinary();
    s_lifecycle_mutex = xSemaphoreCreateMutex();
    if (s_reconfigure_done == NULL || s_lifecycle_mutex == NULL) {
        if (s_reconfigure_done != NULL) vSemaphoreDelete(s_reconfigure_done);
        if (s_lifecycle_mutex != NULL) vSemaphoreDelete(s_lifecycle_mutex);
        s_reconfigure_done = NULL;
        s_lifecycle_mutex = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = slave_stack_start(&cfg);
    if (err != ESP_OK) {
        vSemaphoreDelete(s_reconfigure_done);
        vSemaphoreDelete(s_lifecycle_mutex);
        s_reconfigure_done = NULL;
        s_lifecycle_mutex = NULL;
        return err;
    }
    if (xTaskCreate(modbus_slave_task, "modbus_slave_task", CONFIG_APP_MB_SLAVE_TASK_STACK_SIZE,
                    NULL, CONFIG_APP_MB_SLAVE_TASK_PRIORITY, &s_owner_task) != pdPASS) {
        slave_stack_destroy(); vSemaphoreDelete(s_reconfigure_done); s_reconfigure_done = NULL; return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t modbus_slave_reconfigure(void)
{
    /* Live esp-modbus teardown is intentionally unsupported: the library deletes
     * workers blocked on FreeRTOS event objects and can corrupt scheduler lists.
     * Configuration takes effect from a clean boot instead. */
    ESP_LOGI(TAG, "live reconfigure deferred; restart required for new link settings");
    return ESP_ERR_NOT_SUPPORTED;
}

static void modbus_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(300));
    energy_meter_flush_persist();
    esp_restart();
    vTaskDelete(NULL);
}

esp_err_t modbus_slave_request_restart(void)
{
    static bool requested;
    if (requested) return ESP_OK;
    requested = true;
    if (xTaskCreate(modbus_restart_task, "mb_restart", 2048, NULL, 2, NULL) != pdPASS) {
        requested = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void modbus_slave_set_verbose_logging(bool enable)
{
    s_verbose_logging = enable;
    esp_log_level_set("MB_CONTROLLER_SLAVE", enable ? ESP_LOG_DEBUG : ESP_LOG_NONE);
}
bool modbus_slave_get_verbose_logging(void) { return s_verbose_logging; }
void modbus_slave_get_diag(modbus_slave_diag_t *out)
{
    if (out == NULL) return;
    *out = (modbus_slave_diag_t){.events_holding_rd=s_events_holding_rd, .events_holding_wr=s_events_holding_wr,
        .events_input_rd=s_events_input_rd, .events_coils_rd=s_events_coils_rd, .events_coils_wr=s_events_coils_wr,
        .events_discrete_rd=s_events_discrete_rd, .queue_overflow_drops=0,
        .time_since_last_req_ms=s_last_event_us ? (uint32_t)((esp_timer_get_time()-s_last_event_us)/1000ULL) : UINT32_MAX,
        .stack_up=s_stack_up ? 1U : 0U};
}

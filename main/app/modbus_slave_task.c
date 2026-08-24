#include "modbus_slave_task.h"

#include <stdlib.h>
#include <string.h>
#include "config_apply.h"
#include "config_manager.h"
#include "driver/uart.h"
#include "energy_meter_task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbcontroller.h"
#include "register_access.h"
#include "system_status.h"
#include "sdkconfig.h"

/*
 * Modbus RTU slave for the ATM90E32AS-based 3-phase meter.
 * Register map is documented in docs/modbus_slave_register_map.md.
 *
 * Input registers hold measured values (float = 2 regs, high word first).
 * Holding registers hold configuration.
 * Coils drive PCF8574 outputs, discrete inputs read PCF8574 inputs.
 *
 * The Modbus stack accesses the backing arrays directly. A refresh task
 * copies cached meter data into the input register area periodically.
 *
 * Runtime configuration (slave address, baud rate) is sourced from
 * config_manager (NVS) rather than Kconfig defaults — operators edit ID and
 * baud from the LCD, and modbus_slave_reconfigure() tears down + rebuilds the
 * RTU stack so the new parameters take effect without a reboot.
 *
 * RS485 direction control: the on-board transceiver is auto-direction
 * (no DE/RE pin to drive from the ESP32). The HAL UART_MODE_RS485_HALF_DUPLEX
 * mode is intentionally NOT used here — that mode requires an RTS pin to
 * toggle direction, and without one the HAL can loop TX back into RX.
 */

#define MB_INPUT_REG_COUNT 110
#define MB_HOLDING_REG_COUNT 16
#define MB_COIL_COUNT 8
#define MB_DISCRETE_COUNT 8

/* Input register offsets (in 16-bit words). */
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

/* Holding register offsets. */
#define HR_SLAVE_ADDRESS 0
#define HR_BAUD_CODE 1
#define HR_PARITY_CODE 2
#define HR_WIRING_MODE 3
#define HR_LINE_FREQ_SEL 4
#define HR_DEMAND_WINDOW 5
#define HR_APPLY_CONFIG 10
#define HR_RESET_ENERGY 11
#define HR_RESET_DEMAND 12
#define HR_REBOOT 13

#define MB_REBOOT_MAGIC 0x5AA5
#define MB_REGISTER_REFRESH_MS 100

static const char *TAG = "modbus_slave";

static uint16_t s_input_regs[MB_INPUT_REG_COUNT];
static uint16_t s_holding_regs[MB_HOLDING_REG_COUNT];
static uint8_t s_coils;
static uint8_t s_discrete;

static uint32_t s_refresh_count;
static uint32_t s_read_events;
static int64_t s_last_log_us;

/* Verbose driver logs are enabled inside slave_stack_start() so they get re-armed
 * after every rebuild — esp-modbus' default level is restored on tear-down.
 * Tags used:
 *   MB_SERIAL          — portserial.c (RX/TX bytes, FIFO overflow, frame error)
 *   MB_CONTROLLER_SLAVE — mbc_serial_slave.c (controller init/destroy errors)
 *   MBS_TIMER          — T3.5 timer (frame spacing)
 */

static TaskHandle_t s_refresh_task_handle;
static TaskHandle_t s_event_task_handle;
static bool s_stack_up;
static volatile bool s_reconfigure_pending;

/* Uptime in milliseconds (monotonic), for log correlation across tasks. */
static inline uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* Store a float into two 16-bit registers, high word first (ABCD order). */
static void store_float(uint16_t offset, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    s_input_regs[offset] = (uint16_t)(bits >> 16);
    s_input_regs[offset + 1] = (uint16_t)(bits & 0xFFFF);
}

/* Read one float data point via the Data Point Layer into an input-register
 * pair. On a read error (e.g. no measurement stored yet) the register pair is
 * left unchanged — same "keep last / start at zero" behavior as before. */
static void store_float_dp(uint16_t offset, data_point_id_t id)
{
    float value = 0.0f;
    if (data_point_read(id, &value, sizeof(value)) == ESP_OK) {
        store_float(offset, value);
    }
}

static void modbus_refresh_inputs(void)
{
    /* Measurement + Energy: sourced through the Data Point Layer, no longer
     * read from the ATM90 driver directly. Each MEAS_/ENERGY_ read self-guards
     * on validity, so no outer valid check is needed for these. */
    uint8_t meas_valid = 0;
    (void)data_point_read(MEAS_VALID, &meas_valid, sizeof(meas_valid));
    s_input_regs[IR_MEASURE_VALID] = meas_valid ? 1 : 0;

    store_float_dp(IR_VOLTAGE_A + 0, MEAS_VOLTAGE_L1);
    store_float_dp(IR_VOLTAGE_A + 2, MEAS_VOLTAGE_L2);
    store_float_dp(IR_VOLTAGE_A + 4, MEAS_VOLTAGE_L3);
    store_float_dp(IR_CURRENT_A + 0, MEAS_CURRENT_L1);
    store_float_dp(IR_CURRENT_A + 2, MEAS_CURRENT_L2);
    store_float_dp(IR_CURRENT_A + 4, MEAS_CURRENT_L3);
    store_float_dp(IR_CURRENT_N, MEAS_CURRENT_NEUTRAL);
    store_float_dp(IR_ACTIVE_A + 0, MEAS_POWER_ACTIVE_L1);
    store_float_dp(IR_ACTIVE_A + 2, MEAS_POWER_ACTIVE_L2);
    store_float_dp(IR_ACTIVE_A + 4, MEAS_POWER_ACTIVE_L3);
    store_float_dp(IR_ACTIVE_TOTAL, MEAS_POWER_ACTIVE_TOTAL);
    store_float_dp(IR_REACTIVE_A + 0, MEAS_POWER_REACTIVE_L1);
    store_float_dp(IR_REACTIVE_A + 2, MEAS_POWER_REACTIVE_L2);
    store_float_dp(IR_REACTIVE_A + 4, MEAS_POWER_REACTIVE_L3);
    store_float_dp(IR_REACTIVE_TOTAL, MEAS_POWER_REACTIVE_TOTAL);
    store_float_dp(IR_APPARENT_A + 0, MEAS_POWER_APPARENT_L1);
    store_float_dp(IR_APPARENT_A + 2, MEAS_POWER_APPARENT_L2);
    store_float_dp(IR_APPARENT_A + 4, MEAS_POWER_APPARENT_L3);
    store_float_dp(IR_APPARENT_TOTAL, MEAS_POWER_APPARENT_TOTAL);
    store_float_dp(IR_PF_A + 0, MEAS_PF_L1);
    store_float_dp(IR_PF_A + 2, MEAS_PF_L2);
    store_float_dp(IR_PF_A + 4, MEAS_PF_L3);
    store_float_dp(IR_PF_TOTAL, MEAS_PF_TOTAL);
    store_float_dp(IR_FREQUENCY, MEAS_FREQUENCY);
    store_float_dp(IR_TEMPERATURE, MEAS_TEMP_ATM90);
    store_float_dp(IR_ENERGY_AI, ENERGY_ACTIVE_IMPORT);
    store_float_dp(IR_ENERGY_AE, ENERGY_ACTIVE_EXPORT);
    store_float_dp(IR_ENERGY_RI, ENERGY_REACTIVE_IMPORT);
    store_float_dp(IR_ENERGY_RE, ENERGY_REACTIVE_EXPORT);

    /* TODO: fields with no Data Point yet (phase angle, current peak, raw ATM90
     * status words, demand). These are still read straight from the driver until
     * the Data Dictionary / Data Point Layer covers them. */
    atm90e32as_measurements_t m;
    if (energy_meter_get_latest(&m) == ESP_OK) {
        for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
            store_float(IR_ANGLE_A + i * 2, m.phase_angle[i]);
            store_float(IR_CURRENT_PEAK_A + i * 2, m.current_peak[i]);
        }
        s_input_regs[IR_SYS_STATUS0] = m.sys_status0;
        s_input_regs[IR_SYS_STATUS1] = m.sys_status1;
        s_input_regs[IR_METER_STATUS0] = m.meter_status0;
        s_input_regs[IR_METER_STATUS1] = m.meter_status1;
    }

    energy_meter_demand_t demand;
    if (energy_meter_get_demand(&demand) == ESP_OK) {
        store_float(IR_DEMAND, demand.active_power_demand_w);
        store_float(IR_DEMAND_MAX, demand.active_power_demand_max_w);
    }

    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    s_input_regs[IR_UPTIME_H] = (uint16_t)(uptime_s >> 16);
    s_input_regs[IR_UPTIME_L] = (uint16_t)(uptime_s & 0xFFFF);
}

static void modbus_apply_discrete_inputs(void)
{
    /* Digital inputs via the Data Point Layer (bool -> 1 byte each). Only update
     * the packed discrete byte if both reads succeed, as before. */
    uint8_t in0 = 0;
    uint8_t in1 = 0;
    if (data_point_read(DI_INPUT0_STATE, &in0, sizeof(in0)) == ESP_OK &&
        data_point_read(DI_INPUT1_STATE, &in1, sizeof(in1)) == ESP_OK) {
        s_discrete = (uint8_t)((in0 ? 0x01 : 0x00) | (in1 ? 0x02 : 0x00));
    }
}

static void modbus_refresh_coils(void)
{
    /* Refresh coil state from hardware. Called before Modbus stack serves a READ,
     * so a relay toggled via LCD/MQTT/Web is visible to the Modbus master. */
    uint8_t out0 = 0;
    uint8_t out1 = 0;
    if (data_point_read(DO_RELAY0_STATE, &out0, sizeof(out0)) == ESP_OK &&
        data_point_read(DO_RELAY1_STATE, &out1, sizeof(out1)) == ESP_OK) {
        s_coils = (uint8_t)((out0 ? 0x01 : 0x00) | (out1 ? 0x02 : 0x00));
    }
}

static void modbus_apply_coils(void)
{
    /* Digital outputs via the Data Point Layer (bool -> 1 byte each). */
    uint8_t out0 = (s_coils & 0x01) != 0;
    uint8_t out1 = (s_coils & 0x02) != 0;
    data_point_write(DO_RELAY0_STATE, &out0, sizeof(out0));
    data_point_write(DO_RELAY1_STATE, &out1, sizeof(out1));
}

static void modbus_handle_holding_write(void)
{
    if (s_holding_regs[HR_RESET_ENERGY] != 0) {
        s_holding_regs[HR_RESET_ENERGY] = 0;
        energy_meter_reset_energy();
        ESP_LOGI(TAG, "energy accumulators reset via Modbus");
    }
    if (s_holding_regs[HR_RESET_DEMAND] != 0) {
        s_holding_regs[HR_RESET_DEMAND] = 0;
        energy_meter_reset_demand();
        ESP_LOGI(TAG, "demand reset via Modbus");
    }
    if (s_holding_regs[HR_DEMAND_WINDOW] != 0) {
        energy_meter_set_demand_window_minutes(s_holding_regs[HR_DEMAND_WINDOW]);
    }
    if (s_holding_regs[HR_REBOOT] == MB_REBOOT_MAGIC) {
        ESP_LOGW(TAG, "reboot requested via Modbus");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    /* HR_APPLY_CONFIG: master wrote new comm params into the holding regs and
     * is asking the slave to persist them and restart the RTU stack with the
     * new address / baud. Slave addr 0 / baud > 4 are rejected — those would
     * break the link entirely. */
    if (s_holding_regs[HR_APPLY_CONFIG] != 0) {
        uint8_t new_addr = (uint8_t)s_holding_regs[HR_SLAVE_ADDRESS];
        uint8_t new_baud_code = (uint8_t)s_holding_regs[HR_BAUD_CODE];
        s_holding_regs[HR_APPLY_CONFIG] = 0;
        if (new_addr < 1U || new_addr > 247U || new_baud_code > 4U) {
            ESP_LOGW(TAG, "ApplyConfig rejected: addr=%u baud_code=%u (out of range)",
                     (unsigned)new_addr, (unsigned)new_baud_code);
            return;
        }
        config_manager_t cfg;
        if (config_manager_get(&cfg) != ESP_OK) {
            ESP_LOGE(TAG, "ApplyConfig: config_manager_get failed");
            return;
        }
        cfg.mb_slave_id = new_addr;
        cfg.mb_baud_code = new_baud_code;
        esp_err_t upd = config_manager_update(&cfg);
        if (upd != ESP_OK) {
            ESP_LOGE(TAG, "ApplyConfig: config_manager_update failed (%s)", esp_err_to_name(upd));
            return;
        }
        esp_err_t ap = config_apply(CONFIG_APPLY_MODBUS_SLAVE);
        esp_err_t sv = (ap == ESP_OK) ? config_manager_save() : ESP_OK;
        if (upd == ESP_OK && ap == ESP_OK && sv == ESP_OK) {
            ESP_LOGI(TAG, "slave comm reconfig via Modbus: addr=0x%02X baud_code=%u",
                     (unsigned)new_addr, (unsigned)new_baud_code);
        } else {
            ESP_LOGE(TAG, "slave comm reconfig failed: update=%s apply=%s save=%s",
                     esp_err_to_name(upd), esp_err_to_name(ap), esp_err_to_name(sv));
        }
    }
}

static void modbus_register_refresh_task(void *arg)
{
    (void)arg;

    while (1) {
        /* The Modbus stack serves these backing areas directly. Keep them as a
         * continuously updated projection of the Data Point backends so the first
         * request after an idle period never receives the previous snapshot. */
        modbus_refresh_inputs();
        modbus_apply_discrete_inputs();
        modbus_refresh_coils();
        s_refresh_count++;
        vTaskDelay(pdMS_TO_TICKS(MB_REGISTER_REFRESH_MS));
    }
}

static void slave_stack_destroy(void);
static esp_err_t slave_stack_start(const config_manager_t *cfg);

static void modbus_slave_task(void *arg)
{
    (void)arg;

    while (1) {
        /* Reconfigure takes priority over the event loop: tear down the stack,
         * read the latest config_manager snapshot, then rebuild with the new
         * address / baud. Runs in this task's context, so it can safely call
         * mbc_slave_destroy() (which deletes the UART) and vTaskDelete() on
         * the sibling refresh task without lock-ordering hazards.
         *
         * The [%u] timestamp lets us correlate a teardown with any malformed
         * response observed on the bus — if the bad bytes were emitted while
         * this branch was running, the destroy log + a stack-down + rebuild
         * pair will bracket the event. */
        if (s_reconfigure_pending) {
            s_reconfigure_pending = false;
            ESP_LOGW(TAG, "[%u] reconfigure: stack-down requested (rt=%llu rds=%llu s_up=%d)",
                     (unsigned)uptime_ms(),
                     (unsigned long long)s_refresh_count,
                     (unsigned long long)s_read_events,
                     (int)s_stack_up);
            slave_stack_destroy();
            config_manager_t *cfg = malloc(sizeof(*cfg));
            if (cfg == NULL) {
                ESP_LOGE(TAG, "reconfigure: no memory for snapshot");
                system_status_set(SYS_MODULE_RS485_SLAVE, SYS_STATUS_ERROR);
            } else {
                esp_err_t rcfg = config_manager_get(cfg);
                if (rcfg != ESP_OK) {
                    ESP_LOGE(TAG, "reconfigure: config_manager_get failed (%s)",
                             esp_err_to_name(rcfg));
                    free(cfg);
                    system_status_set(SYS_MODULE_RS485_SLAVE, SYS_STATUS_ERROR);
                } else {
                    esp_err_t rs = slave_stack_start(cfg);
                    free(cfg);
                    if (rs != ESP_OK) {
                        ESP_LOGE(TAG, "reconfigure: slave_stack_start failed (%s)",
                                 esp_err_to_name(rs));
                        system_status_set(SYS_MODULE_RS485_SLAVE, SYS_STATUS_ERROR);
                    } else {
                        ESP_LOGI(TAG, "reconfigure: stack rebuilt with current config");
                    }
                }
            }
            continue;
        }

        (void)mbc_slave_check_event(MB_EVENT_HOLDING_REG_WR | MB_EVENT_COILS_WR |
                                    MB_EVENT_INPUT_REG_RD | MB_EVENT_DISCRETE_RD);

        mb_param_info_t reg_info;
        if (mbc_slave_get_param_info(&reg_info, 0) == ESP_OK) {
            if (reg_info.type & (MB_EVENT_INPUT_REG_RD | MB_EVENT_DISCRETE_RD)) {
                s_read_events++;
                ESP_LOGI(TAG, "master READ: type=0x%x mb_offset=%u size=%u addr=%p",
                         (unsigned)reg_info.type, (unsigned)reg_info.mb_offset,
                         (unsigned)reg_info.size, reg_info.address);
            }
            if (reg_info.type & MB_EVENT_COILS_WR) {
                modbus_apply_coils();
            }
            if (reg_info.type & MB_EVENT_HOLDING_REG_WR) {
                modbus_handle_holding_write();
            }
        }

        int64_t now = esp_timer_get_time();
        if (now - s_last_log_us >= 2000000) {
            s_last_log_us = now;
            ESP_LOGI(TAG,
                     "diag[%u]: s_up=%d coils=0x%02X addr=0x%02X baud=%u refresh=%u reads=%u valid=%u DevID[100]=0x%04X V_A[0..1]=0x%04X 0x%04X uptime[104..105]=0x%04X 0x%04X",
                     (unsigned)uptime_ms(),
                     (int)s_stack_up, (unsigned)s_coils,
                     (unsigned)s_holding_regs[HR_SLAVE_ADDRESS],
                     (unsigned)s_holding_regs[HR_BAUD_CODE],
                     (unsigned)s_refresh_count, (unsigned)s_read_events,
                     s_input_regs[IR_MEASURE_VALID], s_input_regs[IR_DEVICE_ID],
                     s_input_regs[IR_VOLTAGE_A], s_input_regs[IR_VOLTAGE_A + 1],
                     s_input_regs[IR_UPTIME_H], s_input_regs[IR_UPTIME_L]);
        }
    }
}

static uart_parity_t modbus_parity_from_code(uint16_t code)
{
    switch (code) {
    case 1: return UART_PARITY_EVEN;
    case 2: return UART_PARITY_ODD;
    default: return UART_PARITY_DISABLE;
    }
}

/* Public — exposed via header so LCD can render the current baud. */
uint32_t modbus_slave_baud_from_code(uint8_t code)
{
    switch (code) {
    case 1: return 19200;
    case 2: return 38400;
    case 3: return 57600;
    case 4: return 115200;
    default: return 9600;
    }
}

/*
 * Tear down the RTU slave stack + backing tasks so the next slave_stack_start()
 * can re-install on the same UART with different comm params.
 *
 * Order matters: stop the event task first so it stops calling into the stack,
 * then stop the refresh task, then destroy the controller (which calls
 * uart_driver_delete internally).
 */
static void slave_stack_destroy(void)
{
    const uint32_t ts = uptime_ms();
    const bool was_up = s_stack_up;
    ESP_LOGW(TAG, "[%u] stack_destroy: enter (s_stack_up=%d)", ts, (int)was_up);
    if (s_event_task_handle != NULL) {
        vTaskDelete(s_event_task_handle);
        s_event_task_handle = NULL;
    }
    if (s_refresh_task_handle != NULL) {
        vTaskDelete(s_refresh_task_handle);
        s_refresh_task_handle = NULL;
    }
    if (s_stack_up) {
        mbc_slave_destroy();
        s_stack_up = false;
    }
    memset(s_input_regs, 0, sizeof(s_input_regs));
    memset(s_holding_regs, 0, sizeof(s_holding_regs));
    s_coils = 0;
    s_discrete = 0;
    s_refresh_count = 0;
    s_read_events = 0;
    ESP_LOGW(TAG, "[%u] stack_destroy: exit (was_up=%d)", ts, (int)was_up);
}

/*
 * Bring up the RTU slave stack on UART1 with the comm params read from cfg.
 * Kconfig pins + uart port stay compile-time fixed; only address/baud/parity
 * vary at runtime. Kconfig defaults are used as a fallback for the very first
 * boot (when config_manager has no value yet).
 */
static esp_err_t slave_stack_start(const config_manager_t *cfg)
{
    uint8_t addr = cfg->mb_slave_id;
    if (addr < 1U || addr > 247U) {
        addr = (uint8_t)CONFIG_APP_MB_SLAVE_ADDR;
        if (addr < 1U || addr > 247U) {
            addr = 1U;
        }
    }
    uint8_t baud_code = cfg->mb_baud_code;
    if (baud_code > 4U) {
        baud_code = 0;
    }
    /* Parity is not exposed in config_manager / LCD yet — kept 0 (none) by
     * design (8N1). Wire in here when a UI for parity is added. */
    uint8_t parity_code = 0;

    s_holding_regs[HR_SLAVE_ADDRESS] = addr;
    s_holding_regs[HR_BAUD_CODE] = baud_code;
    s_holding_regs[HR_PARITY_CODE] = parity_code;
    s_holding_regs[HR_DEMAND_WINDOW] = 15;
    s_input_regs[IR_DEVICE_ID] = 0x9032;
    s_input_regs[IR_FW_VERSION] = 0x0100;
    s_input_regs[IR_HW_VERSION] = 0x0100;

    /* Seed the backing areas before the stack can serve its first request. */
    modbus_refresh_inputs();
    modbus_apply_discrete_inputs();
    modbus_refresh_coils();

    void *handler = NULL;
    ESP_RETURN_ON_ERROR(mbc_slave_init(MB_PORT_SERIAL_SLAVE, &handler), TAG, "init modbus slave failed");

    /* Arm verbose esp-modbus logging so RX/TX byte counts, FIFO overflow,
     * frame-error and parity-error events from portserial.c surface in the
     * firmware log. Critical for correlating a malformed response (e.g. an
     * observed "0A 40 00 2A FD" with FC 0x40 + exception 0x00 + bad CRC)
     * with an underlying RX/TX fault. Idempotent — re-arm on every rebuild. */
    esp_log_level_set("MB_SERIAL", ESP_LOG_DEBUG);
    esp_log_level_set("MB_CONTROLLER_SLAVE", ESP_LOG_DEBUG);
    esp_log_level_set("MBS_TIMER", ESP_LOG_DEBUG);
    ESP_LOGI(TAG, "[%u] stack_start: verbose MB_SERIAL/MB_CONTROLLER_SLAVE/MBS_TIMER logs enabled",
             (unsigned)uptime_ms());

    mb_communication_info_t comm = {
        .mode = MB_MODE_RTU,
        .slave_addr = addr,
        .port = CONFIG_APP_MB_SLAVE_UART_PORT,
        .baudrate = modbus_slave_baud_from_code(baud_code),
        .parity = modbus_parity_from_code(parity_code),
    };
    esp_err_t err = mbc_slave_setup(&comm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup modbus slave failed: %s", esp_err_to_name(err));
        mbc_slave_destroy();
        return err;
    }

    mb_register_area_descriptor_t area;

    area.type = MB_PARAM_INPUT;
    area.start_offset = 0;
    area.address = s_input_regs;
    area.size = sizeof(s_input_regs);
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(area), TAG, "set input descriptor failed");

    area.type = MB_PARAM_HOLDING;
    area.start_offset = 0;
    area.address = s_holding_regs;
    area.size = sizeof(s_holding_regs);
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(area), TAG, "set holding descriptor failed");

    area.type = MB_PARAM_COIL;
    area.start_offset = 0;
    area.address = &s_coils;
    area.size = sizeof(s_coils);
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(area), TAG, "set coil descriptor failed");

    area.type = MB_PARAM_DISCRETE;
    area.start_offset = 0;
    area.address = &s_discrete;
    area.size = sizeof(s_discrete);
    ESP_RETURN_ON_ERROR(mbc_slave_set_descriptor(area), TAG, "set discrete descriptor failed");

    ESP_RETURN_ON_ERROR(mbc_slave_start(), TAG, "start modbus slave failed");

    ESP_RETURN_ON_ERROR(uart_set_pin(CONFIG_APP_MB_SLAVE_UART_PORT,
                                     CONFIG_APP_MB_SLAVE_TXD_GPIO,
                                     CONFIG_APP_MB_SLAVE_RXD_GPIO,
                                     UART_PIN_NO_CHANGE,
                                     UART_PIN_NO_CHANGE),
                        TAG, "set modbus UART pins failed");

    /* RS485 direction control is handled by the on-board auto-direction
     * transceiver — there is no DE/RE pin to drive from the ESP32. The HAL
     * UART_MODE_RS485_HALF_DUPLEX mode is for designs that wire DE/RE to the
     * RTS pin; using it here would either no-op (RTS pin unconfigured) or
     * loop TX back into RX (if any other path enables that), so we stay in
     * plain UART mode. */
    ESP_LOGI(TAG, "slave UART in normal mode (auto-direction RS485, no DE/RE pin)");

    BaseType_t ret = xTaskCreate(modbus_register_refresh_task,
                                 "modbus_reg_refresh",
                                 CONFIG_APP_MB_SLAVE_TASK_STACK_SIZE,
                                 NULL,
                                 CONFIG_APP_MB_SLAVE_TASK_PRIORITY,
                                 &s_refresh_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create register refresh task failed");
        mbc_slave_destroy();
        s_stack_up = false;
        return ESP_FAIL;
    }

    ret = xTaskCreate(modbus_slave_task,
                      "modbus_slave_task",
                      CONFIG_APP_MB_SLAVE_TASK_STACK_SIZE,
                      NULL,
                      CONFIG_APP_MB_SLAVE_TASK_PRIORITY,
                      &s_event_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "create modbus slave task failed");
        vTaskDelete(s_refresh_task_handle);
        s_refresh_task_handle = NULL;
        mbc_slave_destroy();
        s_stack_up = false;
        return ESP_FAIL;
    }

    s_stack_up = true;

    ESP_LOGI(TAG, "[%u] stack_start: ready (addr=0x%02X baud_code=%u port=%d)",
             (unsigned)uptime_ms(),
             (unsigned)addr, (unsigned)baud_code,
             (int)CONFIG_APP_MB_SLAVE_UART_PORT);
    ESP_LOGI(TAG, "Modbus RTU slave started: addr=0x%02X baud_code=%u port=%d",
             (unsigned)addr, (unsigned)baud_code,
             (int)CONFIG_APP_MB_SLAVE_UART_PORT);

    system_status_set(SYS_MODULE_RS485_SLAVE, SYS_STATUS_READY);
    return ESP_OK;
}

esp_err_t modbus_slave_task_start(void)
{
    if (s_stack_up) {
        return ESP_OK;
    }

    config_manager_t *cfg = malloc(sizeof(*cfg));
    if (cfg == NULL) {
        ESP_LOGE(TAG, "out of memory reading config; falling back to Kconfig defaults");
        config_manager_t local = {0};
        local.mb_slave_id = (uint8_t)CONFIG_APP_MB_SLAVE_ADDR;
        local.mb_baud_code = 0;
        return slave_stack_start(&local);
    }
    if (config_manager_get(cfg) != ESP_OK) {
        ESP_LOGW(TAG, "config_manager not ready; falling back to Kconfig defaults");
        cfg->mb_slave_id = (uint8_t)CONFIG_APP_MB_SLAVE_ADDR;
        cfg->mb_baud_code = 0;
    }
    esp_err_t err = slave_stack_start(cfg);
    free(cfg);
    return err;
}

/*
 * Tear down + bring up the RTU stack with the current config_manager values.
 * Used by config_apply when the operator edits slave address / baud rate from
 * the LCD, console, or via the HR_APPLY_CONFIG holding register. The
 * reconfigure runs in the slave task's own context so we don't need to
 * coordinate with the destroy-from-ISR hazard.
 */
esp_err_t modbus_slave_reconfigure(void)
{
    if (!s_stack_up) {
        return ESP_ERR_INVALID_STATE;
    }
    s_reconfigure_pending = true;
    return ESP_OK;
}

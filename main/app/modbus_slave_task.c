#include "modbus_slave_task.h"

#include <string.h>
#include "driver/uart.h"
#include "energy_meter_task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "io_expander.h"
#include "mbcontroller.h"
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

static const char *TAG = "modbus_slave";

static uint16_t s_input_regs[MB_INPUT_REG_COUNT];
static uint16_t s_holding_regs[MB_HOLDING_REG_COUNT];
static uint8_t s_coils;
static uint8_t s_discrete;

static uint32_t s_refresh_count;
static uint32_t s_read_events;
static int64_t s_last_log_us;

/* Store a float into two 16-bit registers, high word first (ABCD order). */
static void store_float(uint16_t offset, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    s_input_regs[offset] = (uint16_t)(bits >> 16);
    s_input_regs[offset + 1] = (uint16_t)(bits & 0xFFFF);
}

static void modbus_refresh_inputs(void)
{
    atm90e32as_measurements_t m;
    bool valid = (energy_meter_get_latest(&m) == ESP_OK);

    s_input_regs[IR_MEASURE_VALID] = valid ? 1 : 0;

    if (valid) {
        for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
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
    s_input_regs[IR_UPTIME_L] = (uint16_t)(uptime_s & 0xFFFF);
}

static void modbus_apply_discrete_inputs(void)
{
    bool in0 = false;
    bool in1 = false;
    if (io_expander_get_in0(&in0) == ESP_OK && io_expander_get_in1(&in1) == ESP_OK) {
        s_discrete = (uint8_t)((in0 ? 0x01 : 0x00) | (in1 ? 0x02 : 0x00));
    }
}

static void modbus_apply_coils(void)
{
    io_expander_set_out0((s_coils & 0x01) != 0);
    io_expander_set_out1((s_coils & 0x02) != 0);
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
    /* HR_APPLY_CONFIG for address/baud/parity change would be persisted here
     * (NVS + stack restart). Left as a stub until required, to avoid dropping
     * the active link unexpectedly. */
    if (s_holding_regs[HR_APPLY_CONFIG] != 0) {
        s_holding_regs[HR_APPLY_CONFIG] = 0;
        ESP_LOGW(TAG, "ApplyConfig requested (comm reconfig not yet implemented)");
    }
}

static void modbus_slave_task(void *arg)
{
    while (1) {
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

        modbus_refresh_inputs();
        modbus_apply_discrete_inputs();
        s_refresh_count++;

        int64_t now = esp_timer_get_time();
        if (now - s_last_log_us >= 2000000) {
            s_last_log_us = now;
            ESP_LOGI(TAG,
                     "diag: refresh=%u reads=%u valid=%u DevID[100]=0x%04X V_A[0..1]=0x%04X 0x%04X uptime[104..105]=0x%04X 0x%04X",
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

static uint32_t modbus_baud_from_code(uint16_t code)
{
    switch (code) {
    case 1: return 19200;
    case 2: return 38400;
    case 3: return 57600;
    case 4: return 115200;
    default: return 9600;
    }
}

esp_err_t modbus_slave_task_start(void)
{
    memset(s_input_regs, 0, sizeof(s_input_regs));
    memset(s_holding_regs, 0, sizeof(s_holding_regs));

    s_holding_regs[HR_SLAVE_ADDRESS] = CONFIG_APP_MB_SLAVE_ADDR;
    s_holding_regs[HR_BAUD_CODE] = 0;
    s_holding_regs[HR_PARITY_CODE] = 0;
    s_holding_regs[HR_DEMAND_WINDOW] = 15;
    s_input_regs[IR_DEVICE_ID] = 0x9032;
    s_input_regs[IR_FW_VERSION] = 0x0100;
    s_input_regs[IR_HW_VERSION] = 0x0100;

    void *handler = NULL;
    ESP_RETURN_ON_ERROR(mbc_slave_init(MB_PORT_SERIAL_SLAVE, &handler), TAG, "init modbus slave failed");

    mb_communication_info_t comm = {
        .mode = MB_MODE_RTU,
        .slave_addr = CONFIG_APP_MB_SLAVE_ADDR,
        .port = CONFIG_APP_MB_SLAVE_UART_PORT,
        .baudrate = modbus_baud_from_code(s_holding_regs[HR_BAUD_CODE]),
        .parity = modbus_parity_from_code(s_holding_regs[HR_PARITY_CODE]),
    };
    ESP_RETURN_ON_ERROR(mbc_slave_setup(&comm), TAG, "setup modbus slave failed");

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
    ESP_RETURN_ON_ERROR(uart_set_mode(CONFIG_APP_MB_SLAVE_UART_PORT, UART_MODE_RS485_HALF_DUPLEX),
                        TAG, "set RS485 half duplex failed");

    BaseType_t ret = xTaskCreate(modbus_slave_task,
                                 "modbus_slave_task",
                                 CONFIG_APP_MB_SLAVE_TASK_STACK_SIZE,
                                 NULL,
                                 CONFIG_APP_MB_SLAVE_TASK_PRIORITY,
                                 NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_FAIL, TAG, "create modbus slave task failed");

    ESP_LOGI(TAG, "Modbus RTU slave started: addr=0x%02X port=%d RX=%d TX=%d",
             CONFIG_APP_MB_SLAVE_ADDR, CONFIG_APP_MB_SLAVE_UART_PORT,
             CONFIG_APP_MB_SLAVE_RXD_GPIO, CONFIG_APP_MB_SLAVE_TXD_GPIO);
    return ESP_OK;
}

#include "atm90e32as.h"

#include <stdlib.h>
#include <string.h>
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spi_bus_shared.h"

#define ATM90E32AS_SPI_READ_BIT 0x8000
#define ATM90E32AS_SPI_TIMEOUT_MS 100
#define ATM90E32AS_POWER_LSB 0.00032f

#define REG_METER_EN 0x00
#define REG_SAG_PEAK_DET_CFG 0x05
#define REG_OV_TH 0x06
#define REG_ZX_CONFIG 0x07
#define REG_SAG_TH 0x08
#define REG_FREQ_LO_TH 0x0C
#define REG_FREQ_HI_TH 0x0D
#define REG_PL_CONST_H 0x31
#define REG_PL_CONST_L 0x32
#define REG_MMODE0 0x33
#define REG_MMODE1 0x34
#define REG_P_START_TH 0x35
#define REG_Q_START_TH 0x36
#define REG_S_START_TH 0x37
#define REG_P_PHASE_TH 0x38
#define REG_Q_PHASE_TH 0x39
#define REG_S_PHASE_TH 0x3A
#define REG_P_OFFSET_A 0x41
#define REG_Q_OFFSET_A 0x42
#define REG_P_OFFSET_B 0x43
#define REG_Q_OFFSET_B 0x44
#define REG_P_OFFSET_C 0x45
#define REG_Q_OFFSET_C 0x46
#define REG_PQ_GAIN_A 0x47
#define REG_PHI_A 0x48
#define REG_PQ_GAIN_B 0x49
#define REG_PHI_B 0x4A
#define REG_PQ_GAIN_C 0x4B
#define REG_PHI_C 0x4C
#define REG_P_OFFSET_AF 0x51
#define REG_P_OFFSET_BF 0x52
#define REG_P_OFFSET_CF 0x53
#define REG_P_GAIN_AF 0x54
#define REG_P_GAIN_BF 0x55
#define REG_P_GAIN_CF 0x56
#define REG_U_GAIN_A 0x61
#define REG_I_GAIN_A 0x62
#define REG_U_OFFSET_A 0x63
#define REG_I_OFFSET_A 0x64
#define REG_U_GAIN_B 0x65
#define REG_I_GAIN_B 0x66
#define REG_U_OFFSET_B 0x67
#define REG_I_OFFSET_B 0x68
#define REG_U_GAIN_C 0x69
#define REG_I_GAIN_C 0x6A
#define REG_U_OFFSET_C 0x6B
#define REG_I_OFFSET_C 0x6C
#define REG_SOFT_RESET 0x70
#define REG_EMM_STATE0 0x71
#define REG_EMM_STATE1 0x72
#define REG_EMM_INT_STATE0 0x73
#define REG_EMM_INT_STATE1 0x74
#define REG_EMM_INT_EN0 0x75
#define REG_EMM_INT_EN1 0x76
#define REG_LAST_SPI_DATA 0x78
#define REG_CFG_REG_ACC_EN 0x7F
#define REG_PMEAN_T 0xB0
#define REG_PMEAN_A 0xB1
#define REG_PMEAN_B 0xB2
#define REG_PMEAN_C 0xB3
#define REG_QMEAN_T 0xB4
#define REG_QMEAN_A 0xB5
#define REG_QMEAN_B 0xB6
#define REG_QMEAN_C 0xB7
#define REG_SMEAN_T 0xB8
#define REG_SMEAN_A 0xB9
#define REG_SMEAN_B 0xBA
#define REG_SMEAN_C 0xBB
#define REG_PFMEAN_T 0xBC
#define REG_PFMEAN_A 0xBD
#define REG_PFMEAN_B 0xBE
#define REG_PFMEAN_C 0xBF
#define REG_PMEAN_T_LSB 0xC0
#define REG_PMEAN_A_LSB 0xC1
#define REG_PMEAN_B_LSB 0xC2
#define REG_PMEAN_C_LSB 0xC3
#define REG_QMEAN_T_LSB 0xC4
#define REG_QMEAN_A_LSB 0xC5
#define REG_QMEAN_B_LSB 0xC6
#define REG_QMEAN_C_LSB 0xC7
#define REG_SMEAN_T_LSB 0xC8
#define REG_SMEAN_A_LSB 0xC9
#define REG_SMEAN_B_LSB 0xCA
#define REG_SMEAN_C_LSB 0xCB
#define REG_URMS_A 0xD9
#define REG_URMS_B 0xDA
#define REG_URMS_C 0xDB
#define REG_IRMS_N 0xDC
#define REG_IRMS_A 0xDD
#define REG_IRMS_B 0xDE
#define REG_IRMS_C 0xDF
#define REG_IPEAK_A 0xF5
#define REG_IPEAK_B 0xF6
#define REG_IPEAK_C 0xF7
#define REG_FREQ 0xF8
#define REG_PANGLE_A 0xF9
#define REG_PANGLE_B 0xFA
#define REG_PANGLE_C 0xFB
#define REG_TEMP 0xFC
#define REG_APENERGY_T 0x80
#define REG_ANENERGY_T 0x84
#define REG_RPENERGY_T 0x88
#define REG_RNENERGY_T 0x8C

struct atm90e32as_dev_t {
    spi_device_handle_t spi;
    atm90e32as_config_t config;
};

static const char *TAG = "atm90e32as";

static esp_err_t atm90e32as_transfer16(atm90e32as_handle_t handle, uint16_t address, uint16_t tx_value, uint16_t *rx_value)
{
    uint8_t tx[4] = {
        (uint8_t)(address >> 8),
        (uint8_t)(address & 0xFF),
        (uint8_t)(tx_value >> 8),
        (uint8_t)(tx_value & 0xFF),
    };
    uint8_t rx[4] = {0};

    spi_transaction_t transaction = {
        .length = 32,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };

    esp_rom_delay_us(10);
    esp_err_t ret = spi_device_transmit(handle->spi, &transaction);
    esp_rom_delay_us(10);
    if (ret != ESP_OK) {
        return ret;
    }

    if (rx_value != NULL) {
        *rx_value = ((uint16_t)rx[2] << 8) | rx[3];
    }
    return ESP_OK;
}

void atm90e32as_get_default_calib(atm90e32as_calib_t *calib)
{
    if (calib == NULL) {
        return;
    }

    memset(calib, 0, sizeof(*calib));
    calib->line_freq = ATM90E32AS_LINE_FREQ_50HZ;
    calib->wiring_mode = ATM90E32AS_WIRING_3P4W;
    calib->pga_gain = ATM90E32AS_PGA_GAIN_1X;

    for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
        calib->phase[i].voltage_gain = 7305;
        calib->phase[i].current_gain = 27961;
        calib->phase[i].reference_voltage = 220.0f;
        calib->phase[i].reference_current = 5.0f;
    }
}

esp_err_t atm90e32as_create(const atm90e32as_config_t *config, atm90e32as_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    atm90e32as_handle_t dev = calloc(1, sizeof(struct atm90e32as_dev_t));
    ESP_RETURN_ON_FALSE(dev != NULL, ESP_ERR_NO_MEM, TAG, "no memory for device");
    dev->config = *config;

    ESP_RETURN_ON_ERROR(spi_bus_shared_init(), TAG, "init shared SPI bus failed");

    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = config->spi_clock_hz > 0 ? config->spi_clock_hz : 200000,
        .mode = 3,
        .spics_io_num = config->cs_gpio,
        .queue_size = 1,
    };

    esp_err_t ret = spi_bus_add_device(spi_bus_shared_get_host(), &dev_config, &dev->spi);
    if (ret != ESP_OK) {
        free(dev);
        return ret;
    }

    *handle = dev;
    return ESP_OK;
}

esp_err_t atm90e32as_delete(atm90e32as_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    esp_err_t ret = spi_bus_remove_device(handle->spi);
    free(handle);
    return ret;
}

esp_err_t atm90e32as_read_register(atm90e32as_handle_t handle, uint16_t reg, uint16_t *value)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "value is NULL");
    return atm90e32as_transfer16(handle, reg | ATM90E32AS_SPI_READ_BIT, 0xFFFF, value);
}

esp_err_t atm90e32as_write_register(atm90e32as_handle_t handle, uint16_t reg, uint16_t value)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    return atm90e32as_transfer16(handle, reg, value, NULL);
}

static esp_err_t atm90e32as_read_s32(atm90e32as_handle_t handle, uint16_t reg_hi, uint16_t reg_lo, int32_t *value)
{
    uint16_t hi = 0;
    uint16_t lo = 0;
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, reg_hi, &hi), TAG, "read hi failed");
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, reg_lo, &lo), TAG, "read lo failed");
    *value = ((int32_t)(int16_t)hi << 16) | lo;
    return ESP_OK;
}

static uint16_t atm90e32as_build_mmode0(const atm90e32as_calib_t *calib)
{
    uint16_t mmode0 = 0x0087;
    if (calib->line_freq == ATM90E32AS_LINE_FREQ_60HZ) {
        mmode0 |= (1U << 12);
    }
    if (calib->wiring_mode == ATM90E32AS_WIRING_3P3W) {
        mmode0 |= (1U << 8);
        mmode0 &= (uint16_t)~(1U << 1);
    }
    return mmode0;
}

static esp_err_t atm90e32as_write_calibration_registers(atm90e32as_handle_t handle)
{
    const atm90e32as_calib_t *c = &handle->config.calib;
    const uint16_t ugain[3] = {REG_U_GAIN_A, REG_U_GAIN_B, REG_U_GAIN_C};
    const uint16_t igain[3] = {REG_I_GAIN_A, REG_I_GAIN_B, REG_I_GAIN_C};
    const uint16_t uoffs[3] = {REG_U_OFFSET_A, REG_U_OFFSET_B, REG_U_OFFSET_C};
    const uint16_t ioffs[3] = {REG_I_OFFSET_A, REG_I_OFFSET_B, REG_I_OFFSET_C};
    const uint16_t poffs[3] = {REG_P_OFFSET_A, REG_P_OFFSET_B, REG_P_OFFSET_C};
    const uint16_t qoffs[3] = {REG_Q_OFFSET_A, REG_Q_OFFSET_B, REG_Q_OFFSET_C};
    const uint16_t pqgain[3] = {REG_PQ_GAIN_A, REG_PQ_GAIN_B, REG_PQ_GAIN_C};
    const uint16_t phi[3] = {REG_PHI_A, REG_PHI_B, REG_PHI_C};
    const uint16_t pgainf[3] = {REG_P_GAIN_AF, REG_P_GAIN_BF, REG_P_GAIN_CF};

    for (int i = 0; i < 3; i++) {
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, ugain[i], c->phase[i].voltage_gain), TAG, "write Ugain failed");
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, igain[i], c->phase[i].current_gain), TAG, "write Igain failed");
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, uoffs[i], (uint16_t)c->phase[i].voltage_offset), TAG, "write Uoffset failed");
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, ioffs[i], (uint16_t)c->phase[i].current_offset), TAG, "write Ioffset failed");
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, poffs[i], (uint16_t)c->phase[i].active_power_offset), TAG, "write Poffset failed");
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, qoffs[i], (uint16_t)c->phase[i].reactive_power_offset), TAG, "write Qoffset failed");
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, pqgain[i], c->phase[i].pq_gain), TAG, "write PQ gain failed");
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, phi[i], (uint16_t)c->phase[i].phase_comp), TAG, "write phase comp failed");
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, pgainf[i], c->phase[i].fundamental_power_gain), TAG, "write fundamental gain failed");
    }

    return ESP_OK;
}

esp_err_t atm90e32as_get_calibration(atm90e32as_handle_t handle, atm90e32as_calib_t *calib)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(calib != NULL, ESP_ERR_INVALID_ARG, TAG, "calib is NULL");

    *calib = handle->config.calib;
    return ESP_OK;
}

esp_err_t atm90e32as_apply_calibration(atm90e32as_handle_t handle, const atm90e32as_calib_t *calib)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(calib != NULL, ESP_ERR_INVALID_ARG, TAG, "calib is NULL");

    handle->config.calib = *calib;
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_CFG_REG_ACC_EN, 0x55AA), TAG, "enable config failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_calibration_registers(handle), TAG, "write calibration registers failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_MMODE0, atm90e32as_build_mmode0(calib)), TAG, "write mmode0 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_MMODE1, calib->pga_gain), TAG, "write mmode1 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_CFG_REG_ACC_EN, 0x0000), TAG, "disable config failed");
    return ESP_OK;
}

esp_err_t atm90e32as_set_calibration(atm90e32as_handle_t handle, const atm90e32as_calib_t *calib, bool apply)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(calib != NULL, ESP_ERR_INVALID_ARG, TAG, "calib is NULL");

    if (apply) {
        return atm90e32as_apply_calibration(handle, calib);
    }

    handle->config.calib = *calib;
    return ESP_OK;
}

esp_err_t atm90e32as_init(atm90e32as_handle_t handle)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");

    const atm90e32as_calib_t *calib = &handle->config.calib;
    uint16_t freq_hi = calib->line_freq == ATM90E32AS_LINE_FREQ_60HZ ? 6300 : 5300;
    uint16_t freq_lo = calib->line_freq == ATM90E32AS_LINE_FREQ_60HZ ? 5700 : 4700;

    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_SOFT_RESET, 0x789A), TAG, "soft reset failed");
    vTaskDelay(pdMS_TO_TICKS(6));
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_CFG_REG_ACC_EN, 0x55AA), TAG, "enable config failed");

    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_METER_EN, 0x0001), TAG, "enable meter failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_SAG_PEAK_DET_CFG, 0xFF3F), TAG, "write sag peak config failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_SAG_TH, 0x0000), TAG, "write sag threshold failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_OV_TH, 0xFFFF), TAG, "write overvoltage threshold failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_FREQ_HI_TH, freq_hi), TAG, "write freq high failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_FREQ_LO_TH, freq_lo), TAG, "write freq low failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_EMM_INT_EN0, 0xB76F), TAG, "write int en0 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_EMM_INT_EN1, 0xDDFD), TAG, "write int en1 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_EMM_INT_STATE0, 0x0001), TAG, "clear int state0 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_EMM_INT_STATE1, 0x0001), TAG, "clear int state1 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_ZX_CONFIG, 0xD654), TAG, "write zx config failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_PL_CONST_H, 0x0861), TAG, "write PL high failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_PL_CONST_L, 0xC468), TAG, "write PL low failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_MMODE0, atm90e32as_build_mmode0(calib)), TAG, "write mmode0 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_MMODE1, calib->pga_gain), TAG, "write mmode1 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_P_START_TH, 0x1D4C), TAG, "write P start failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_Q_START_TH, 0x1D4C), TAG, "write Q start failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_S_START_TH, 0x1D4C), TAG, "write S start failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_P_PHASE_TH, 0x02EE), TAG, "write P phase failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_Q_PHASE_TH, 0x02EE), TAG, "write Q phase failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_S_PHASE_TH, 0x02EE), TAG, "write S phase failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_P_OFFSET_AF, 0x0000), TAG, "write P offset AF failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_P_OFFSET_BF, 0x0000), TAG, "write P offset BF failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_P_OFFSET_CF, 0x0000), TAG, "write P offset CF failed");

    ESP_RETURN_ON_ERROR(atm90e32as_write_calibration_registers(handle), TAG, "apply calibration failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_CFG_REG_ACC_EN, 0x0000), TAG, "disable config failed");

    return ESP_OK;
}

static float read_u16_scaled(uint16_t raw, float scale)
{
    return (float)raw / scale;
}

static float read_s16_scaled(uint16_t raw, float scale)
{
    return (float)(int16_t)raw / scale;
}

esp_err_t atm90e32as_read_measurements(atm90e32as_handle_t handle, atm90e32as_measurements_t *out)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    memset(out, 0, sizeof(*out));

    const uint16_t urms[3] = {REG_URMS_A, REG_URMS_B, REG_URMS_C};
    const uint16_t irms[3] = {REG_IRMS_A, REG_IRMS_B, REG_IRMS_C};
    const uint16_t p_hi[4] = {REG_PMEAN_T, REG_PMEAN_A, REG_PMEAN_B, REG_PMEAN_C};
    const uint16_t p_lo[4] = {REG_PMEAN_T_LSB, REG_PMEAN_A_LSB, REG_PMEAN_B_LSB, REG_PMEAN_C_LSB};
    const uint16_t q_hi[4] = {REG_QMEAN_T, REG_QMEAN_A, REG_QMEAN_B, REG_QMEAN_C};
    const uint16_t q_lo[4] = {REG_QMEAN_T_LSB, REG_QMEAN_A_LSB, REG_QMEAN_B_LSB, REG_QMEAN_C_LSB};
    const uint16_t s_hi[4] = {REG_SMEAN_T, REG_SMEAN_A, REG_SMEAN_B, REG_SMEAN_C};
    const uint16_t s_lo[4] = {REG_SMEAN_T_LSB, REG_SMEAN_A_LSB, REG_SMEAN_B_LSB, REG_SMEAN_C_LSB};
    const uint16_t pf[4] = {REG_PFMEAN_T, REG_PFMEAN_A, REG_PFMEAN_B, REG_PFMEAN_C};
    const uint16_t angle[3] = {REG_PANGLE_A, REG_PANGLE_B, REG_PANGLE_C};

    uint16_t raw = 0;
    for (int i = 0; i < 3; i++) {
        ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, urms[i], &raw), TAG, "read voltage failed");
        out->voltage[i] = read_u16_scaled(raw, 100.0f);
        ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, irms[i], &raw), TAG, "read current failed");
        out->current[i] = read_u16_scaled(raw, 1000.0f);
        ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, pf[i + 1], &raw), TAG, "read PF failed");
        out->power_factor[i] = read_s16_scaled(raw, 1000.0f);
        ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, angle[i], &raw), TAG, "read phase angle failed");
        out->phase_angle[i] = read_u16_scaled(raw, 10.0f);
    }

    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_IRMS_N, &raw), TAG, "read neutral current failed");
    out->current_neutral = read_u16_scaled(raw, 1000.0f);

    int32_t power_raw = 0;
    ESP_RETURN_ON_ERROR(atm90e32as_read_s32(handle, p_hi[0], p_lo[0], &power_raw), TAG, "read total P failed");
    out->total_active_power = power_raw * ATM90E32AS_POWER_LSB;
    ESP_RETURN_ON_ERROR(atm90e32as_read_s32(handle, q_hi[0], q_lo[0], &power_raw), TAG, "read total Q failed");
    out->total_reactive_power = power_raw * ATM90E32AS_POWER_LSB;
    ESP_RETURN_ON_ERROR(atm90e32as_read_s32(handle, s_hi[0], s_lo[0], &power_raw), TAG, "read total S failed");
    out->total_apparent_power = power_raw * ATM90E32AS_POWER_LSB;

    for (int i = 0; i < 3; i++) {
        ESP_RETURN_ON_ERROR(atm90e32as_read_s32(handle, p_hi[i + 1], p_lo[i + 1], &power_raw), TAG, "read phase P failed");
        out->active_power[i] = power_raw * ATM90E32AS_POWER_LSB;
        ESP_RETURN_ON_ERROR(atm90e32as_read_s32(handle, q_hi[i + 1], q_lo[i + 1], &power_raw), TAG, "read phase Q failed");
        out->reactive_power[i] = power_raw * ATM90E32AS_POWER_LSB;
        ESP_RETURN_ON_ERROR(atm90e32as_read_s32(handle, s_hi[i + 1], s_lo[i + 1], &power_raw), TAG, "read phase S failed");
        out->apparent_power[i] = power_raw * ATM90E32AS_POWER_LSB;
    }

    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, pf[0], &raw), TAG, "read total PF failed");
    out->total_power_factor = read_s16_scaled(raw, 1000.0f);
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_FREQ, &raw), TAG, "read frequency failed");
    out->frequency = read_u16_scaled(raw, 100.0f);
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_TEMP, &raw), TAG, "read temperature failed");
    out->temperature = (float)(int16_t)raw;
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_EMM_STATE0, &out->sys_status0), TAG, "read state0 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_EMM_STATE1, &out->sys_status1), TAG, "read state1 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_EMM_INT_STATE0, &out->meter_status0), TAG, "read int state0 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_EMM_INT_STATE1, &out->meter_status1), TAG, "read int state1 failed");

    const uint16_t ipeak[3] = {REG_IPEAK_A, REG_IPEAK_B, REG_IPEAK_C};
    for (int i = 0; i < 3; i++) {
        ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, ipeak[i], &raw), TAG, "read current peak failed");
        int16_t peak = (int16_t)raw;
        if (peak < 0) {
            peak = (int16_t)(-peak);
        }
        out->current_peak[i] = ((float)peak * (float)handle->config.calib.phase[i].current_gain) / 8192000.0f;
    }

    return ESP_OK;
}

esp_err_t atm90e32as_read_energy_counts(atm90e32as_handle_t handle, atm90e32as_energy_counts_t *out)
{
    ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_INVALID_ARG, TAG, "handle is NULL");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    /* These total-energy registers are read-to-clear; each read returns the
     * increment accumulated since the previous read. */
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_APENERGY_T, &out->active_import), TAG, "read active import energy failed");
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_ANENERGY_T, &out->active_export), TAG, "read active export energy failed");
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_RPENERGY_T, &out->reactive_import), TAG, "read reactive import energy failed");
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_RNENERGY_T, &out->reactive_export), TAG, "read reactive export energy failed");

    return ESP_OK;
}

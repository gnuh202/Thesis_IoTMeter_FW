#include "atm90e32as.h"

#include <limits.h>
#include <math.h>
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
#define ATM90E32AS_CFG_UNLOCK 0x55AAU
#define ATM90E32AS_MMODE0_3P4W_50HZ 0x0087U
#define ATM90E32AS_MMODE0_3P3W_50HZ 0x0185U
#define ATM90E32AS_MMODE0_FREQ_60HZ (1U << 12)
#define ATM90E32AS_MMODE1_PGA_IA_SHIFT 0U
#define ATM90E32AS_MMODE1_PGA_IB_SHIFT 2U
#define ATM90E32AS_MMODE1_PGA_IC_SHIFT 4U

#define REG_METER_EN 0x00
#define REG_SAG_PEAK_DET_CFG 0x05
#define REG_OV_TH 0x06
#define REG_ZX_CONFIG 0x07
#define REG_SAG_TH 0x08
#define REG_PHASE_LOSS_TH 0x09
#define REG_IN_WARN_TH 0x0A
#define REG_OI_TH 0x0B
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

esp_err_t atm90e32as_validate_calibration(const atm90e32as_calib_t *calib)
{
    ESP_RETURN_ON_FALSE(calib != NULL, ESP_ERR_INVALID_ARG, TAG, "calibration is NULL");
    ESP_RETURN_ON_FALSE(calib->line_freq == ATM90E32AS_LINE_FREQ_50HZ ||
                        calib->line_freq == ATM90E32AS_LINE_FREQ_60HZ,
                        ESP_ERR_INVALID_ARG, TAG, "invalid line frequency");
    ESP_RETURN_ON_FALSE(calib->wiring_mode == ATM90E32AS_WIRING_3P4W ||
                        calib->wiring_mode == ATM90E32AS_WIRING_3P3W,
                        ESP_ERR_INVALID_ARG, TAG, "invalid wiring mode");
    ESP_RETURN_ON_FALSE(calib->pga_gain >= ATM90E32AS_PGA_GAIN_1X &&
                        calib->pga_gain <= ATM90E32AS_PGA_GAIN_4X,
                        ESP_ERR_INVALID_ARG, TAG, "invalid PGA gain");
    for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
        ESP_RETURN_ON_FALSE(calib->phase[i].voltage_gain != 0 &&
                            calib->phase[i].current_gain != 0,
                            ESP_ERR_INVALID_ARG, TAG, "phase %d has zero gain", i);
    }
    return ESP_OK;
}

esp_err_t atm90e32as_calculate_gain(uint16_t old_gain, float reference, float measured,
                                    uint16_t *new_gain)
{
    ESP_RETURN_ON_FALSE(new_gain != NULL, ESP_ERR_INVALID_ARG, TAG, "new_gain is NULL");
    ESP_RETURN_ON_FALSE(old_gain != 0 && isfinite(reference) && reference > 0.0f &&
                        isfinite(measured) && measured > 0.0f,
                        ESP_ERR_INVALID_ARG, TAG, "invalid gain calculation input");

    /* new_gain = round(old_gain * reference / measured), evaluated in integers.
     * Both operands are scaled to micro-units so the scale cancels in the ratio;
     * ESP32-S3 has no double FPU, so this keeps the calibration deterministic and
     * off the software double-precision path. num <= 65535 * ~5e8 < 2^63. */
    int64_t ref_u = llround((double)reference * 1000000.0);
    int64_t meas_u = llround((double)measured * 1000000.0);
    ESP_RETURN_ON_FALSE(ref_u > 0 && meas_u > 0, ESP_ERR_INVALID_ARG, TAG,
                        "gain calculation input too small to scale");

    int64_t numerator = (int64_t)old_gain * ref_u;
    int64_t quotient = numerator / meas_u;
    int64_t remainder = numerator % meas_u;
    if (2 * remainder >= meas_u) quotient++;   /* numerator >= 0: round half-up */

    ESP_RETURN_ON_FALSE(quotient >= 1 && quotient <= UINT16_MAX,
                        ESP_ERR_INVALID_SIZE, TAG, "calculated gain is out of range");
    *new_gain = (uint16_t)quotient;
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

    /* UGAIN/IGAIN power-on defaults are 0x8000. PQGain and fundamental
     * energy gain are signed correction registers; zero means no correction. */
    for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
        calib->phase[i].voltage_gain = 0x8000U;
        calib->phase[i].current_gain = 0x8000U;
        calib->phase[i].pq_gain = 0;
        calib->phase[i].fundamental_power_gain = 0;
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

esp_err_t atm90e32as_write_warning_thresholds(atm90e32as_handle_t handle,
                                              const atm90e32as_warning_thresholds_t *th)
{
    ESP_RETURN_ON_FALSE(handle != NULL && th != NULL, ESP_ERR_INVALID_ARG, TAG, "invalid arg");

    /* 0x05..0x0D live in the config space behind the CFG_REG_ACC_EN unlock
     * window (same protocol as atm90e32as_init). */
    esp_err_t err = atm90e32as_write_register(handle, REG_CFG_REG_ACC_EN, 0x55AA);
    if (err != ESP_OK) {
        return err;
    }
    err = atm90e32as_write_register(handle, REG_OV_TH, th->ov_th);
    if (err == ESP_OK) {
        err = atm90e32as_write_register(handle, REG_SAG_TH, th->sag_th);
    }
    if (err == ESP_OK) {
        err = atm90e32as_write_register(handle, REG_PHASE_LOSS_TH, th->phase_loss_th);
    }
    if (err == ESP_OK && th->write_oi_th) {
        err = atm90e32as_write_register(handle, REG_OI_TH, th->oi_th);
    }
    if (err == ESP_OK) {
        err = atm90e32as_write_register(handle, REG_FREQ_LO_TH, th->freq_lo_th);
    }
    if (err == ESP_OK) {
        err = atm90e32as_write_register(handle, REG_FREQ_HI_TH, th->freq_hi_th);
    }
    esp_err_t lock_err = atm90e32as_write_register(handle, REG_CFG_REG_ACC_EN, 0x0000);
    return err != ESP_OK ? err : lock_err;
}

esp_err_t atm90e32as_read_raw_rms(atm90e32as_handle_t handle, uint16_t urms[3], uint16_t irms[3])
{
    ESP_RETURN_ON_FALSE(handle != NULL && urms != NULL && irms != NULL, ESP_ERR_INVALID_ARG,
                        TAG, "invalid arg");
    esp_err_t err = ESP_OK;
    for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
        esp_err_t e = atm90e32as_read_register(handle, REG_URMS_A + p, &urms[p]);
        if (e != ESP_OK) {
            err = e;
        }
    }
    for (int p = 0; p < ATM90E32AS_PHASE_COUNT; p++) {
        esp_err_t e = atm90e32as_read_register(handle, REG_IRMS_A + p, &irms[p]);
        if (e != ESP_OK) {
            err = e;
        }
    }
    return err;
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
    uint16_t mmode0 = calib->wiring_mode == ATM90E32AS_WIRING_3P3W
                          ? ATM90E32AS_MMODE0_3P3W_50HZ
                          : ATM90E32AS_MMODE0_3P4W_50HZ;
    if (calib->line_freq == ATM90E32AS_LINE_FREQ_60HZ) {
        mmode0 |= ATM90E32AS_MMODE0_FREQ_60HZ;
    }
    return mmode0;
}

static uint16_t atm90e32as_build_mmode1(atm90e32as_pga_gain_t gain)
{
    uint16_t field = (uint16_t)gain;
    return (uint16_t)((field << ATM90E32AS_MMODE1_PGA_IA_SHIFT) |
                      (field << ATM90E32AS_MMODE1_PGA_IB_SHIFT) |
                      (field << ATM90E32AS_MMODE1_PGA_IC_SHIFT));
}

static void atm90e32as_frequency_thresholds(atm90e32as_line_freq_t frequency,
                                            uint16_t *low, uint16_t *high)
{
    if (frequency == ATM90E32AS_LINE_FREQ_60HZ) {
        *low = 5700;
        *high = 6300;
    } else {
        *low = 4700;
        *high = 5300;
    }
}

static uint16_t atm90e32as_encode_phi(int16_t phase_comp)
{
    int32_t magnitude = phase_comp < 0 ? -(int32_t)phase_comp : phase_comp;
    if (magnitude > 0xFF) {
        return 0xFFFFU;
    }
    return (uint16_t)magnitude | (phase_comp < 0 ? 0x8000U : 0U);
}

static esp_err_t atm90e32as_validate_phase_comp(int16_t phase_comp)
{
    int32_t magnitude = phase_comp < 0 ? -(int32_t)phase_comp : phase_comp;
    ESP_RETURN_ON_FALSE(magnitude <= 0xFF, ESP_ERR_INVALID_ARG, TAG,
                        "phase compensation exceeds 8-bit delay range");
    return ESP_OK;
}

static esp_err_t atm90e32as_write_calibration_registers(atm90e32as_handle_t handle,
                                                         const atm90e32as_calib_t *c)
{
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
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, pqgain[i], (uint16_t)c->phase[i].pq_gain), TAG, "write PQ gain failed");
        ESP_RETURN_ON_ERROR(atm90e32as_validate_phase_comp(c->phase[i].phase_comp), TAG, "invalid phase compensation");
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, phi[i], atm90e32as_encode_phi(c->phase[i].phase_comp)), TAG, "write phase comp failed");
        ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, pgainf[i], c->phase[i].fundamental_power_gain), TAG, "write fundamental gain failed");
    }

    return ESP_OK;
}

static esp_err_t atm90e32as_verify_calibration_registers(atm90e32as_handle_t handle,
                                                           const atm90e32as_calib_t *c)
{
    const uint16_t regs[3][9] = {
        {REG_U_GAIN_A, REG_I_GAIN_A, REG_U_OFFSET_A, REG_I_OFFSET_A, REG_P_OFFSET_A, REG_Q_OFFSET_A, REG_PQ_GAIN_A, REG_PHI_A, REG_P_GAIN_AF},
        {REG_U_GAIN_B, REG_I_GAIN_B, REG_U_OFFSET_B, REG_I_OFFSET_B, REG_P_OFFSET_B, REG_Q_OFFSET_B, REG_PQ_GAIN_B, REG_PHI_B, REG_P_GAIN_BF},
        {REG_U_GAIN_C, REG_I_GAIN_C, REG_U_OFFSET_C, REG_I_OFFSET_C, REG_P_OFFSET_C, REG_Q_OFFSET_C, REG_PQ_GAIN_C, REG_PHI_C, REG_P_GAIN_CF},
    };
    for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) {
        const uint16_t expected[9] = {
            c->phase[i].voltage_gain, c->phase[i].current_gain,
            (uint16_t)c->phase[i].voltage_offset, (uint16_t)c->phase[i].current_offset,
            (uint16_t)c->phase[i].active_power_offset, (uint16_t)c->phase[i].reactive_power_offset,
            (uint16_t)c->phase[i].pq_gain, atm90e32as_encode_phi(c->phase[i].phase_comp),
            (uint16_t)c->phase[i].fundamental_power_gain,
        };
        for (int j = 0; j < 9; j++) {
            uint16_t actual = 0;
            ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, regs[i][j], &actual), TAG,
                                "readback calibration register failed");
            if (actual != expected[j]) {
                ESP_LOGE(TAG, "calibration readback mismatch phase=%d reg=0x%02X expected=0x%04X actual=0x%04X",
                         i, regs[i][j], expected[j], actual);
                return ESP_ERR_INVALID_RESPONSE;
            }
        }
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
    ESP_RETURN_ON_ERROR(atm90e32as_validate_calibration(calib), TAG, "invalid calibration");

    esp_err_t ret = atm90e32as_write_register(handle, REG_CFG_REG_ACC_EN, ATM90E32AS_CFG_UNLOCK);
    if (ret != ESP_OK) {
        return ret;
    }

    uint16_t freq_low = 0;
    uint16_t freq_high = 0;
    atm90e32as_frequency_thresholds(calib->line_freq, &freq_low, &freq_high);

    ret = atm90e32as_write_calibration_registers(handle, calib);
    if (ret == ESP_OK) ret = atm90e32as_write_register(handle, REG_MMODE0, atm90e32as_build_mmode0(calib));
    if (ret == ESP_OK) ret = atm90e32as_write_register(handle, REG_MMODE1, atm90e32as_build_mmode1(calib->pga_gain));
    if (ret == ESP_OK) ret = atm90e32as_write_register(handle, REG_FREQ_LO_TH, freq_low);
    if (ret == ESP_OK) ret = atm90e32as_write_register(handle, REG_FREQ_HI_TH, freq_high);

    /* Re-enable meter to force DSP to reload calibration parameters.
     * Without this, PQGain/PGainF changes are written to registers but the
     * DSP pipeline continues using stale values until next soft reset. */
    if (ret == ESP_OK) ret = atm90e32as_write_register(handle, REG_METER_EN, 0x0001);

    if (ret == ESP_OK) {
        ret = atm90e32as_verify_calibration_registers(handle, calib);
    }

    esp_err_t lock_ret = atm90e32as_write_register(handle, REG_CFG_REG_ACC_EN, 0x0000);
    if (ret == ESP_OK) ret = lock_ret;
    if (ret == ESP_OK) {
        handle->config.calib = *calib;
    }
    return ret;
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
    ESP_RETURN_ON_ERROR(atm90e32as_validate_calibration(calib), TAG, "invalid calibration");
    uint16_t freq_hi = 0;
    uint16_t freq_lo = 0;
    atm90e32as_frequency_thresholds(calib->line_freq, &freq_lo, &freq_hi);

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
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_MMODE1, atm90e32as_build_mmode1(calib->pga_gain)), TAG, "write mmode1 failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_P_START_TH, 0x1D4C), TAG, "write P start failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_Q_START_TH, 0x1D4C), TAG, "write Q start failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_S_START_TH, 0x1D4C), TAG, "write S start failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_P_PHASE_TH, 0x02EE), TAG, "write P phase failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_Q_PHASE_TH, 0x02EE), TAG, "write Q phase failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_S_PHASE_TH, 0x02EE), TAG, "write S phase failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_P_OFFSET_AF, 0x0000), TAG, "write P offset AF failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_P_OFFSET_BF, 0x0000), TAG, "write P offset BF failed");
    ESP_RETURN_ON_ERROR(atm90e32as_write_register(handle, REG_P_OFFSET_CF, 0x0000), TAG, "write P offset CF failed");

    ESP_RETURN_ON_ERROR(atm90e32as_write_calibration_registers(handle, calib), TAG, "apply calibration failed");
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

    out->wiring_mode = handle->config.calib.wiring_mode;
    if (out->wiring_mode == ATM90E32AS_WIRING_3P3W) {
        out->voltage_valid[0] = true;
        out->voltage_valid[1] = false;
        out->voltage_valid[2] = true;
        out->voltage_semantic[0] = ATM90E32AS_VOLTAGE_UAB;
        out->voltage_semantic[1] = ATM90E32AS_VOLTAGE_UBN;
        out->voltage_semantic[2] = ATM90E32AS_VOLTAGE_UCB;
    } else {
        for (int i = 0; i < ATM90E32AS_PHASE_COUNT; i++) out->voltage_valid[i] = true;
        out->voltage_semantic[0] = ATM90E32AS_VOLTAGE_UAN;
        out->voltage_semantic[1] = ATM90E32AS_VOLTAGE_UBN;
        out->voltage_semantic[2] = ATM90E32AS_VOLTAGE_UCN;
    }

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

esp_err_t atm90e32as_read_power_raw(atm90e32as_handle_t handle, atm90e32as_phase_t phase,
                                    bool reactive, int32_t *value)
{
    ESP_RETURN_ON_FALSE(handle != NULL && value != NULL && phase < ATM90E32AS_PHASE_COUNT,
                        ESP_ERR_INVALID_ARG, TAG, "invalid power raw request");
    const uint16_t p_hi[3] = {REG_PMEAN_A, REG_PMEAN_B, REG_PMEAN_C};
    const uint16_t p_lo[3] = {REG_PMEAN_A_LSB, REG_PMEAN_B_LSB, REG_PMEAN_C_LSB};
    const uint16_t q_hi[3] = {REG_QMEAN_A, REG_QMEAN_B, REG_QMEAN_C};
    const uint16_t q_lo[3] = {REG_QMEAN_A_LSB, REG_QMEAN_B_LSB, REG_QMEAN_C_LSB};
    return atm90e32as_read_s32(handle, reactive ? q_hi[phase] : p_hi[phase],
                               reactive ? q_lo[phase] : p_lo[phase], value);
}
esp_err_t atm90e32as_read_energy_counts(atm90e32as_handle_t handle,
                                          atm90e32as_energy_counts_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");

    /* These total-energy registers are read-to-clear; each read returns the
     * increment accumulated since the previous read. */
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_APENERGY_T, &out->active_import), TAG, "read active import energy failed");
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_ANENERGY_T, &out->active_export), TAG, "read active export energy failed");
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_RPENERGY_T, &out->reactive_import), TAG, "read reactive import energy failed");
    ESP_RETURN_ON_ERROR(atm90e32as_read_register(handle, REG_RNENERGY_T, &out->reactive_export), TAG, "read reactive export energy failed");

    return ESP_OK;
}

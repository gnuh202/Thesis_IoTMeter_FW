#include "io_expander.h"

#include <stdio.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gpio_isr_service.h"
#include "i2c_bus.h"
#include "pcf8574.h"
#include "sdkconfig.h"

/* PCF8574 port bit map. Driven entirely by Kconfig so the firmware can be
 * matched to the schematic without touching C. The defaults reproduce the
 * historical hard-coded map (RST=P0, IN1=P2, IN0=P5, OUT1=P6, OUT0=P7). */
#define IO_EXPANDER_PIN_W5500_RST CONFIG_APP_PCF8574_W5500_RST_PIN
#define IO_EXPANDER_PIN_IN1 CONFIG_APP_PCF8574_IN1_PIN
#define IO_EXPANDER_PIN_IN0 CONFIG_APP_PCF8574_IN0_PIN
#define IO_EXPANDER_PIN_OUT1 CONFIG_APP_PCF8574_OUT1_PIN
#define IO_EXPANDER_PIN_OUT0 CONFIG_APP_PCF8574_OUT0_PIN

#define IO_EXPANDER_INPUT_MASK ((1U << IO_EXPANDER_PIN_IN1) | (1U << IO_EXPANDER_PIN_IN0))

/* Initial PCF8574 latch written at io_expander_init():
 *  - OUT0, OUT1 (relays) start OFF (bit = 0).
 *  - RST (W5500 nRESET) starts released (not asserted) so the W5500 can boot
 *    as soon as its own power is stable. The reset pulse happens later in
 *    ethernet_driver_init() after SPI is configured.
 *  - All other pins (IN0, IN1, unused) float HIGH via PCF8574 pull-ups (bit=1). */
#define IO_EXPANDER_INITIAL_LATCH (0xFF & ~(1U << IO_EXPANDER_PIN_OUT1) & ~(1U << IO_EXPANDER_PIN_OUT0))

/* Level driven on the reset bit to assert W5500 reset. */
#if CONFIG_APP_W5500_RESET_ACTIVE_LOW
#define IO_EXPANDER_W5500_RST_ASSERT_LEVEL false
#else
#define IO_EXPANDER_W5500_RST_ASSERT_LEVEL true
#endif

#define IO_EXPANDER_I2C_SCAN_MAX 16

/* How long the debug reset path waits for RESETn to actually reach the released
 * level. Measured rise on this board is ~2ms against the PCF8574's weak
 * pull-up; 50ms is a generous fault threshold and is kept independent of
 * APP_W5500_RESET_SETTLE_MS so tuning settle time cannot cause false alarms. */
#define IO_EXPANDER_RISE_POLL_MS 50

static const char *TAG = "io_expander";

static pcf8574_handle_t s_pcf8574;
static bool s_initialized;   /* true only after the full init sequence succeeded */
static SemaphoreHandle_t s_int_sem;
static bool s_started;
static bool s_last_in0;
static bool s_last_in1;
static bool s_out0_level;   /* cached last-set output levels (write-only pins) */
static bool s_out1_level;

static void IRAM_ATTR io_expander_isr_handler(void *arg)
{
    BaseType_t high_task_woken = pdFALSE;
    if (s_int_sem != NULL) {
        xSemaphoreGiveFromISR(s_int_sem, &high_task_woken);
    }
    if (high_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t io_expander_read_inputs(bool *in0, bool *in1)
{
    ESP_RETURN_ON_ERROR(pcf8574_read_pin(s_pcf8574, IO_EXPANDER_PIN_IN0, in0), TAG, "read IN0 failed");
    ESP_RETURN_ON_ERROR(pcf8574_read_pin(s_pcf8574, IO_EXPANDER_PIN_IN1, in1), TAG, "read IN1 failed");
    return ESP_OK;
}

static void io_expander_task(void *arg)
{
    while (1) {
        if (xSemaphoreTake(s_int_sem, portMAX_DELAY) == pdTRUE) {
            bool in0 = false;
            bool in1 = false;
            esp_err_t ret = io_expander_read_inputs(&in0, &in1);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "read inputs failed: %s", esp_err_to_name(ret));
                continue;
            }

            if (in0 != s_last_in0 || in1 != s_last_in1) {
                s_last_in0 = in0;
                s_last_in1 = in1;
                ESP_LOGI(TAG, "PCF8574 inputs changed: IN0=%d IN1=%d", in0, in1);
            }
        }
    }
}

/* Tear the device back down so the next call retries from scratch. Without this,
 * a handle created before a later setup step failed would make io_expander_init()
 * report success while the direction mask was never applied - which silently
 * turns the W5500 reset pin into an input and swallows the reset pulse. */
static void io_expander_teardown(void)
{
    if (s_pcf8574 != NULL) {
        pcf8574_delete(s_pcf8574);
        s_pcf8574 = NULL;
    }
    s_initialized = false;
}

static esp_err_t io_expander_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* A previous attempt may have left a half-configured device behind. */
    io_expander_teardown();

    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "init I2C bus failed");
    ESP_RETURN_ON_ERROR(pcf8574_create(i2c_bus_get_handle(), CONFIG_APP_PCF8574_I2C_ADDR, &s_pcf8574), TAG, "create PCF8574 failed");

    esp_err_t ret = pcf8574_set_direction_mask(s_pcf8574, IO_EXPANDER_INPUT_MASK);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "set PCF8574 pin modes failed");

    ret = pcf8574_write_port(s_pcf8574, IO_EXPANDER_INITIAL_LATCH);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "set PCF8574 initial latch failed");

    uint8_t latch = 0;
    ret = pcf8574_get_latch(s_pcf8574, &latch);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "read PCF8574 latch failed");
    ESP_LOGI(TAG, "PCF8574 initialized: addr=0x%02X latch=0x%02X input_mask=0x%02X",
             CONFIG_APP_PCF8574_I2C_ADDR, latch, (unsigned)IO_EXPANDER_INPUT_MASK);

    ret = io_expander_read_inputs(&s_last_in0, &s_last_in1);
    ESP_GOTO_ON_ERROR(ret, fail, TAG, "read initial inputs failed");
    ESP_LOGI(TAG, "PCF8574 initial inputs: IN0=%d IN1=%d", s_last_in0, s_last_in1);

    s_initialized = true;
    return ESP_OK;

fail:
    io_expander_teardown();
    return ret;
}

static esp_err_t io_expander_interrupt_init(void)
{
    if (s_int_sem == NULL) {
        s_int_sem = xSemaphoreCreateBinary();
        ESP_RETURN_ON_FALSE(s_int_sem != NULL, ESP_ERR_NO_MEM, TAG, "create INT semaphore failed");
    }

    gpio_config_t int_gpio_config = {
        .pin_bit_mask = 1ULL << CONFIG_APP_PCF8574_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_gpio_config), TAG, "config PCF8574 INT GPIO failed");

    esp_err_t ret = gpio_isr_service_ensure_installed(0);
    ESP_RETURN_ON_ERROR(ret, TAG, "install GPIO ISR service failed");

    ret = gpio_isr_handler_add(CONFIG_APP_PCF8574_INT_GPIO, io_expander_isr_handler, NULL);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "add PCF8574 INT ISR handler failed");
    }

    return ESP_OK;
}

esp_err_t io_expander_start(void)
{
    ESP_RETURN_ON_ERROR(io_expander_init(), TAG, "init IO expander failed");
    ESP_RETURN_ON_ERROR(io_expander_interrupt_init(), TAG, "init IO expander interrupt failed");

    if (!s_started) {
        BaseType_t ret = xTaskCreate(io_expander_task,
                                     "io_expander_task",
                                     CONFIG_APP_IO_EXPANDER_TASK_STACK_SIZE,
                                     NULL,
                                     CONFIG_APP_IO_EXPANDER_TASK_PRIORITY,
                                     NULL);
        ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_FAIL, TAG, "create IO expander task failed");
        s_started = true;
    }

    xSemaphoreGive(s_int_sem);
    return ESP_OK;
}

/* Read the physical port for logging. Returns 0xFF-safe sentinel handling: on
 * error the caller prints the error instead of a bogus value. */
static esp_err_t io_expander_read_port_hw(uint8_t *port)
{
    return pcf8574_read_port(s_pcf8574, port);
}

esp_err_t io_expander_diag_dump(void)
{
    ESP_LOGI(TAG, "[cfg] i2c: port=%d sda=%d scl=%d clk=%dHz",
             CONFIG_APP_I2C_PORT_NUM, CONFIG_APP_I2C_SDA_IO,
             CONFIG_APP_I2C_SCL_IO, CONFIG_APP_I2C_CLK_SPEED_HZ);
    ESP_LOGI(TAG, "[cfg] pcf8574: addr=0x%02X int_gpio=%d",
             CONFIG_APP_PCF8574_I2C_ADDR, CONFIG_APP_PCF8574_INT_GPIO);
    ESP_LOGI(TAG, "[cfg] pcf8574 map: RST=P%d IN0=P%d IN1=P%d OUT0=P%d OUT1=P%d input_mask=0x%02X",
             IO_EXPANDER_PIN_W5500_RST, IO_EXPANDER_PIN_IN0, IO_EXPANDER_PIN_IN1,
             IO_EXPANDER_PIN_OUT0, IO_EXPANDER_PIN_OUT1, (unsigned)IO_EXPANDER_INPUT_MASK);
    ESP_LOGI(TAG, "[cfg] w5500 reset: pin=P%d active_low=%d assert=%dms settle=%dms",
             IO_EXPANDER_PIN_W5500_RST, CONFIG_APP_W5500_RESET_ACTIVE_LOW ? 1 : 0,
             CONFIG_APP_W5500_RESET_ASSERT_MS, CONFIG_APP_W5500_RESET_SETTLE_MS);

    uint8_t addrs[IO_EXPANDER_I2C_SCAN_MAX];
    size_t found = 0;
    esp_err_t ret = i2c_bus_scan(addrs, IO_EXPANDER_I2C_SCAN_MAX, &found);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[scan] failed: %s", esp_err_to_name(ret));
    } else if (found == 0) {
        ESP_LOGE(TAG, "[scan] no I2C devices responded - check wiring, pull-ups, power");
    } else {
        char line[IO_EXPANDER_I2C_SCAN_MAX * 5 + 1];
        size_t shown = found < IO_EXPANDER_I2C_SCAN_MAX ? found : IO_EXPANDER_I2C_SCAN_MAX;
        int off = 0;
        for (size_t i = 0; i < shown; i++) {
            off += snprintf(line + off, sizeof(line) - off, "0x%02X ", addrs[i]);
        }
        ESP_LOGI(TAG, "[scan] found %u device(s): %s", (unsigned)found, line);

        bool expander_present = false;
        for (size_t i = 0; i < shown; i++) {
            if (addrs[i] == CONFIG_APP_PCF8574_I2C_ADDR) {
                expander_present = true;
                break;
            }
        }
        if (!expander_present) {
            ESP_LOGE(TAG, "[scan] PCF8574 0x%02X did NOT respond - wrong address or unpowered",
                     CONFIG_APP_PCF8574_I2C_ADDR);
        }
    }

    ret = io_expander_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[state] IO expander not initialized: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t latch = 0;
    uint8_t mask = 0;
    uint8_t port = 0;
    pcf8574_get_latch(s_pcf8574, &latch);
    pcf8574_get_input_mask(s_pcf8574, &mask);

    esp_err_t rd = io_expander_read_port_hw(&port);
    if (rd == ESP_OK) {
        ESP_LOGI(TAG, "[state] latch(cached)=0x%02X input_mask=0x%02X port(hw)=0x%02X",
                 latch, mask, port);
        ESP_LOGI(TAG, "[state] RST=%d IN0=%d IN1=%d OUT0(cached)=%d OUT1(cached)=%d",
                 (port >> IO_EXPANDER_PIN_W5500_RST) & 1,
                 (port >> IO_EXPANDER_PIN_IN0) & 1,
                 (port >> IO_EXPANDER_PIN_IN1) & 1,
                 s_out0_level, s_out1_level);
    } else {
        ESP_LOGE(TAG, "[state] latch(cached)=0x%02X input_mask=0x%02X port(hw) read failed: %s",
                 latch, mask, esp_err_to_name(rd));
    }

    return ESP_OK;
}

esp_err_t io_expander_w5500_reset_pulse(void)
{
    const uint8_t pin = IO_EXPANDER_PIN_W5500_RST;
    const bool assert_level = IO_EXPANDER_W5500_RST_ASSERT_LEVEL;

    esp_err_t ret = io_expander_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[reset] IO expander init failed: %s - no reset pulse issued",
                 esp_err_to_name(ret));
        return ret;
    }

#if CONFIG_APP_IO_EXPANDER_DEBUG
    uint8_t port = 0;

    ret = pcf8574_write_pin_verify(s_pcf8574, pin, assert_level, &port);
    ESP_LOGI(TAG, "[reset] assert  P%d -> %d : %s  port(hw)=0x%02X",
             pin, assert_level ? 1 : 0, esp_err_to_name(ret), port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[reset] assert failed - W5500 was never held in reset");
        return ret;
    }
    if ((((port >> pin) & 1) != 0) != assert_level) {
        ESP_LOGE(TAG, "[reset] P%d did NOT reach the asserted level on the device - "
                      "pin held externally, wrong bit, or wrong IC", pin);
    }

    vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_W5500_RESET_ASSERT_MS));
    if (io_expander_read_port_hw(&port) == ESP_OK) {
        ESP_LOGI(TAG, "[reset] hold    %dms      port(hw)=0x%02X",
                 CONFIG_APP_W5500_RESET_ASSERT_MS, port);
    }

    int64_t release_us = esp_timer_get_time();
    ret = pcf8574_write_pin_verify(s_pcf8574, pin, !assert_level, &port);
    ESP_LOGI(TAG, "[reset] release P%d -> %d : %s  port(hw)=0x%02X",
             pin, assert_level ? 0 : 1, esp_err_to_name(ret), port);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[reset] release failed - W5500 is stuck in reset");
        return ret;
    }

    /* The PCF8574 sinks hard but sources only ~100uA, so a capacitive or
     * resistively-loaded RESETn rises slowly and the verify read above can still
     * sample the old level. Poll until the pin actually reaches the deasserted
     * level and report how long it took - a slow or never-completing rise is the
     * difference between a boot that finds the chip and one that reads 0x00.
     *
     * The poll deadline is deliberately independent of SETTLE_MS: settle time is
     * tuned for W5500 startup and may legitimately be shortened, but a rise that
     * takes longer than IO_EXPANDER_RISE_POLL_MS is a hardware fault worth
     * reporting either way. */
    int64_t rise_us = -1;
    while ((esp_timer_get_time() - release_us) < (IO_EXPANDER_RISE_POLL_MS * 1000)) {
        if (io_expander_read_port_hw(&port) == ESP_OK &&
            ((((port >> pin) & 1) != 0) == !assert_level)) {
            rise_us = esp_timer_get_time() - release_us;
            break;
        }
        vTaskDelay(1);
    }
    if (rise_us < 0) {
        ESP_LOGE(TAG, "[reset] P%d NEVER reached %d within %dms (port(hw)=0x%02X) - "
                      "RESETn held low: pull-down, big cap, or a shorted net",
                 pin, assert_level ? 0 : 1, IO_EXPANDER_RISE_POLL_MS, port);
    } else {
        ESP_LOGI(TAG, "[reset] rise    P%d reached %d after %lldus  port(hw)=0x%02X",
                 pin, assert_level ? 0 : 1, rise_us, port);
        if (rise_us > 1000) {
            ESP_LOGW(TAG, "[reset] slow rise (%lldus) - weak PCF8574 pull-up against a "
                          "capacitive/resistive load on RESETn", rise_us);
        }
    }

    int64_t remaining_us = (int64_t)CONFIG_APP_W5500_RESET_SETTLE_MS * 1000 -
                           (esp_timer_get_time() - release_us);
    if (remaining_us > 0) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)(remaining_us / 1000) + 1));
    }
    ESP_LOGI(TAG, "[reset] settle  %dms  -> ESP_OK", CONFIG_APP_W5500_RESET_SETTLE_MS);
#else
    ESP_RETURN_ON_ERROR(pcf8574_write_pin(s_pcf8574, pin, assert_level), TAG, "assert W5500 reset failed");
    vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_W5500_RESET_ASSERT_MS));
    ESP_RETURN_ON_ERROR(pcf8574_write_pin(s_pcf8574, pin, !assert_level), TAG, "release W5500 reset failed");
    vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_W5500_RESET_SETTLE_MS));
#endif

    return ESP_OK;
}

esp_err_t io_expander_set_out0(bool level)
{
    ESP_RETURN_ON_ERROR(io_expander_init(), TAG, "init IO expander failed");
    ESP_RETURN_ON_ERROR(pcf8574_write_pin(s_pcf8574, IO_EXPANDER_PIN_OUT0, level), TAG, "write OUT0 failed");
    s_out0_level = level;
    return ESP_OK;
}

esp_err_t io_expander_set_out1(bool level)
{
    ESP_RETURN_ON_ERROR(io_expander_init(), TAG, "init IO expander failed");
    ESP_RETURN_ON_ERROR(pcf8574_write_pin(s_pcf8574, IO_EXPANDER_PIN_OUT1, level), TAG, "write OUT1 failed");
    s_out1_level = level;
    return ESP_OK;
}

esp_err_t io_expander_get_out0(bool *level)
{
    ESP_RETURN_ON_FALSE(level != NULL, ESP_ERR_INVALID_ARG, TAG, "level is NULL");
    *level = s_out0_level;
    return ESP_OK;
}

esp_err_t io_expander_get_out1(bool *level)
{
    ESP_RETURN_ON_FALSE(level != NULL, ESP_ERR_INVALID_ARG, TAG, "level is NULL");
    *level = s_out1_level;
    return ESP_OK;
}

esp_err_t io_expander_get_in0(bool *level)
{
    ESP_RETURN_ON_ERROR(io_expander_init(), TAG, "init IO expander failed");
    return pcf8574_read_pin(s_pcf8574, IO_EXPANDER_PIN_IN0, level);
}

esp_err_t io_expander_get_in1(bool *level)
{
    ESP_RETURN_ON_ERROR(io_expander_init(), TAG, "init IO expander failed");
    return pcf8574_read_pin(s_pcf8574, IO_EXPANDER_PIN_IN1, level);
}

esp_err_t io_expander_read_port(uint8_t *port)
{
    ESP_RETURN_ON_FALSE(port != NULL, ESP_ERR_INVALID_ARG, TAG, "port is NULL");
    ESP_RETURN_ON_ERROR(io_expander_init(), TAG, "init IO expander failed");
    return io_expander_read_port_hw(port);
}

esp_err_t io_expander_write_pin_raw(uint8_t pin, bool level, uint8_t *port_after)
{
    ESP_RETURN_ON_ERROR(io_expander_init(), TAG, "init IO expander failed");
    return pcf8574_write_pin_verify(s_pcf8574, pin, level, port_after);
}

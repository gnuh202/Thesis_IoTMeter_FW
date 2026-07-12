#include "io_expander.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gpio_isr_service.h"
#include "i2c_bus.h"
#include "pcf8574.h"
#include "sdkconfig.h"

#define IO_EXPANDER_PIN_W5500_RST 0
#define IO_EXPANDER_PIN_IN1 2
#define IO_EXPANDER_PIN_IN0 5
#define IO_EXPANDER_PIN_OUT1 6
#define IO_EXPANDER_PIN_OUT0 7

#define IO_EXPANDER_INPUT_MASK ((1U << IO_EXPANDER_PIN_IN1) | (1U << IO_EXPANDER_PIN_IN0))
#define IO_EXPANDER_INITIAL_LATCH (0xFF & ~(1U << IO_EXPANDER_PIN_OUT1) & ~(1U << IO_EXPANDER_PIN_OUT0))

static const char *TAG = "io_expander";

static pcf8574_handle_t s_pcf8574;
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

static esp_err_t io_expander_init(void)
{
    if (s_pcf8574 != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "init I2C bus failed");
    ESP_RETURN_ON_ERROR(pcf8574_create(i2c_bus_get_handle(), CONFIG_APP_PCF8574_I2C_ADDR, &s_pcf8574), TAG, "create PCF8574 failed");
    ESP_RETURN_ON_ERROR(pcf8574_set_direction_mask(s_pcf8574, IO_EXPANDER_INPUT_MASK), TAG, "set PCF8574 pin modes failed");
    ESP_RETURN_ON_ERROR(pcf8574_write_port(s_pcf8574, IO_EXPANDER_INITIAL_LATCH), TAG, "set PCF8574 initial latch failed");

    uint8_t latch = 0;
    ESP_RETURN_ON_ERROR(pcf8574_get_latch(s_pcf8574, &latch), TAG, "read PCF8574 latch failed");
    ESP_LOGI(TAG, "PCF8574 initialized: latch=0x%02X", latch);

    ESP_RETURN_ON_ERROR(io_expander_read_inputs(&s_last_in0, &s_last_in1), TAG, "read initial inputs failed");
    ESP_LOGI(TAG, "PCF8574 initial inputs: IN0=%d IN1=%d", s_last_in0, s_last_in1);

    return ESP_OK;
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

esp_err_t io_expander_w5500_reset_pulse(void)
{
    ESP_RETURN_ON_ERROR(io_expander_init(), TAG, "init IO expander failed");
    ESP_RETURN_ON_ERROR(pcf8574_write_pin(s_pcf8574, IO_EXPANDER_PIN_W5500_RST, false), TAG, "assert W5500 reset failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(pcf8574_write_pin(s_pcf8574, IO_EXPANDER_PIN_W5500_RST, true), TAG, "release W5500 reset failed");
    vTaskDelay(pdMS_TO_TICKS(250));
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

#include "gpio_isr_service.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "gpio_isr_service";
static SemaphoreHandle_t s_mutex;
static bool s_installed;

esp_err_t gpio_isr_service_ensure_installed(int intr_alloc_flags)
{
    if (s_installed) {
        return ESP_OK;
    }

    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_mutex != NULL, ESP_ERR_NO_MEM, TAG, "create mutex failed");
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (!s_installed) {
        esp_err_t ret = gpio_install_isr_service(intr_alloc_flags);
        if (ret == ESP_ERR_INVALID_STATE) {
            /* Someone installed it outside this helper before we got here. Treat
             * as ready; future callers will not call gpio_install_isr_service()
             * again, so the duplicate-install error log appears at most once. */
            ret = ESP_OK;
        }
        if (ret == ESP_OK) {
            s_installed = true;
        }
        xSemaphoreGive(s_mutex);
        return ret;
    }
    xSemaphoreGive(s_mutex);

    return ESP_OK;
}

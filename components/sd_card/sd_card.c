#include "sd_card.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "sdkconfig.h"

static const char *TAG = "sd_card";
static bool s_sd_detect_initialized;

esp_err_t sd_card_detect_init(void)
{
    if (s_sd_detect_initialized) {
        return ESP_OK;
    }

    gpio_config_t io_config = {
        .pin_bit_mask = 1ULL << CONFIG_APP_SD_DET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_RETURN_ON_ERROR(gpio_config(&io_config), TAG, "configure SD detect pin failed");
    s_sd_detect_initialized = true;
    return ESP_OK;
}

bool sd_card_is_inserted(void)
{
    if (!s_sd_detect_initialized) {
        return false;
    }

    return gpio_get_level(CONFIG_APP_SD_DET_GPIO) == 0;
}

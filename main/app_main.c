#include "app/app_tasks.h"

#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "app_main";

void app_main(void)
{
    esp_err_t ret = app_tasks_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "start app tasks failed: %s", esp_err_to_name(ret));
    }
}


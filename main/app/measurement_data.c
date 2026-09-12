#include "measurement_data.h"

#include <string.h>
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * Pure data model: a mutex-protected snapshot of the latest measurements. No
 * hardware access here — the measurement task fills it via update(), consumers
 * read via get().
 */

static const char *TAG = "meas_data";

static SemaphoreHandle_t s_lock;
static measurement_data_t s_data;

esp_err_t measurement_data_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "create mutex failed");
    }
    return ESP_OK;
}

esp_err_t measurement_data_update(const measurement_data_t *in)
{
    ESP_RETURN_ON_FALSE(in != NULL, ESP_ERR_INVALID_ARG, TAG, "in is NULL");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_data = *in;
    s_data.valid = true;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t measurement_data_get(measurement_data_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_data;
    bool valid = s_data.valid;
    xSemaphoreGive(s_lock);
    return valid ? ESP_OK : ESP_ERR_INVALID_STATE;
}

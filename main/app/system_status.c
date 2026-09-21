#include "system_status.h"

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if CONFIG_APP_STATUS_DEBUG
#include "esp_log.h"
#endif

/*
 * Pure data model: a mutex-protected table of per-module states. No hardware
 * access, no other module included — modules report via set(), consumers read
 * via get().
 */

static const char *TAG = "sys_status";

static SemaphoreHandle_t s_lock;
static system_status_state_t s_states[SYS_MODULE_COUNT];

esp_err_t system_status_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "create mutex failed");
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < SYS_MODULE_COUNT; i++) {
        s_states[i] = SYS_STATUS_UNKNOWN;
    }
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t system_status_set(system_module_t module, system_status_state_t state)
{
    ESP_RETURN_ON_FALSE(module < SYS_MODULE_COUNT, ESP_ERR_INVALID_ARG, TAG, "bad module");
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "not initialized");

    xSemaphoreTake(s_lock, portMAX_DELAY);
#if CONFIG_APP_STATUS_DEBUG
    system_status_state_t old = s_states[module];
#endif
    s_states[module] = state;
    xSemaphoreGive(s_lock);

#if CONFIG_APP_STATUS_DEBUG
    /* Log only on a real transition (old != new), never on repeated same-state sets. */
    if (old != state) {
        ESP_LOGI(TAG, "STATUS: %s %s -> %s",
                 system_status_module_name(module),
                 system_status_state_name(old),
                 system_status_state_name(state));
    }
#endif
    return ESP_OK;
}

system_status_state_t system_status_get(system_module_t module)
{
    if (module >= SYS_MODULE_COUNT || s_lock == NULL) {
        return SYS_STATUS_UNKNOWN;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    system_status_state_t state = s_states[module];
    xSemaphoreGive(s_lock);
    return state;
}

const char *system_status_module_name(system_module_t module)
{
    switch (module) {
    case SYS_MODULE_ATM90: return "ATM90";
    case SYS_MODULE_RS485_MASTER: return "RS485 Master";
    case SYS_MODULE_RS485_SLAVE: return "RS485 Slave";
    case SYS_MODULE_ETHERNET: return "Ethernet";
    case SYS_MODULE_WIFI: return "WiFi";
    case SYS_MODULE_MQTT: return "MQTT";
    case SYS_MODULE_SD_CARD: return "SD Card";
    case SYS_MODULE_DIGITAL_INPUT: return "Digital Input";
    case SYS_MODULE_DIGITAL_OUTPUT: return "Digital Output";
    case SYS_MODULE_RTC: return "RTC";
    default: return "unknown";
    }
}

const char *system_status_state_name(system_status_state_t state)
{
    switch (state) {
    case SYS_STATUS_UNKNOWN: return "UNKNOWN";
    case SYS_STATUS_INIT: return "INIT";
    case SYS_STATUS_READY: return "READY";
    case SYS_STATUS_WARNING: return "WARNING";
    case SYS_STATUS_ERROR: return "ERROR";
    case SYS_STATUS_OFFLINE: return "OFFLINE";
    default: return "UNKNOWN";
    }
}

#if CONFIG_APP_STATUS_DEBUG
void system_status_dump(void)
{
    ESP_LOGI(TAG, "==== system status ====");
    for (int i = 0; i < SYS_MODULE_COUNT; i++) {
        ESP_LOGI(TAG, "%-16s %s",
                 system_status_module_name((system_module_t)i),
                 system_status_state_name(system_status_get((system_module_t)i)));
    }
}
#endif

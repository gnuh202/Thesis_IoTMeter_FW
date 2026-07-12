#include "app_tasks.h"

#include "console_task.h"
#include "energy_meter_task.h"
#include "hmi_test_task.h"
#include "io_expander.h"
#include "modbus_master_task.h"
#include "modbus_slave_task.h"
#include "mqtt_manager.h"
#include "network_comm_task.h"
#include "network_manager.h"
#include "sd_card.h"
#include "wifi_manager.h"
#include "esp_check.h"

static const char *TAG = "app_tasks";

esp_err_t app_tasks_start(void)
{
    /* Own the shared network infrastructure (NVS / netif / event loop) here,
     * before any driver or NVS consumer starts. ethernet_driver and
     * wifi_manager rely on this having run and no longer init it themselves. */
    ESP_RETURN_ON_ERROR(network_manager_infra_init(), TAG, "init network infra failed");

    ESP_RETURN_ON_ERROR(io_expander_start(), TAG, "start IO expander failed");
    ESP_RETURN_ON_ERROR(sd_card_manager_start(), TAG, "start SD card manager failed");
    ESP_RETURN_ON_ERROR(hmi_test_task_start(), TAG, "start HMI test task failed");
    ESP_RETURN_ON_ERROR(energy_meter_task_start(), TAG, "start energy meter task failed");
    ESP_RETURN_ON_ERROR(modbus_slave_task_start(), TAG, "start Modbus slave failed");
    ESP_RETURN_ON_ERROR(modbus_master_task_start(), TAG, "start Modbus master failed");
#if CONFIG_APP_CONSOLE_ENABLE
    ESP_RETURN_ON_ERROR(console_task_start(), TAG, "start console failed");
#endif
    ESP_RETURN_ON_ERROR(wifi_manager_start(), TAG, "start WiFi manager failed");
    ESP_RETURN_ON_ERROR(network_comm_task_start(), TAG, "start network comm task failed");

    /* Start the orchestrator last: it primes STA credentials into the already
     * running WiFi driver and observes Ethernet state. In 5b-2a it only tracks
     * and logs; failover (interface switching) arrives in 5b-2b. */
    ESP_RETURN_ON_ERROR(network_manager_start(), TAG, "start network manager failed");

#if CONFIG_APP_MQTT_ENABLE
    /* MQTT last: it waits for the orchestrator to report an IP before connecting,
     * so the metering core and network stack are already up. A missing/disabled
     * broker profile just leaves the MQTT task idle — it never blocks startup. */
    ESP_RETURN_ON_ERROR(mqtt_manager_start(), TAG, "start MQTT manager failed");
#endif

    return ESP_OK;
}

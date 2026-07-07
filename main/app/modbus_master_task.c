#include "modbus_master_task.h"

#include "esp_log.h"
#include "sdkconfig.h"

/*
 * Modbus RTU master stub.
 *
 * The hardware (UART on RX=GPIO11, TX=GPIO12) is reserved for a future
 * Modbus master role. The concrete polling logic is not defined yet, so this
 * module currently only logs its reserved configuration and returns.
 *
 * When implemented, this will likely:
 *   - init a second esp-modbus controller as MB_PORT_SERIAL_MASTER
 *   - define a device/parameter table for downstream slaves
 *   - poll periodically and expose results (e.g. into the meter/Modbus map)
 */

static const char *TAG = "modbus_master";

esp_err_t modbus_master_task_start(void)
{
    ESP_LOGI(TAG, "Modbus master reserved on RX=%d TX=%d (not implemented yet)",
             CONFIG_APP_MB_MASTER_RXD_GPIO, CONFIG_APP_MB_MASTER_TXD_GPIO);
    return ESP_OK;
}

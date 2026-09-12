#include "network_comm_task.h"

#include "ethernet_driver.h"

/*
 * Legacy network bootstrap entry point retained so boot ordering and callers do
 * not change. Reachability pinging was redundant: link/IP usability is already
 * tracked by ethernet_driver/network_manager, so no background task is needed.
 */

esp_err_t network_comm_task_start(void)
{
    return ethernet_driver_init();
}

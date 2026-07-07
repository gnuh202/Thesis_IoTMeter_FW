#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * W5500 Ethernet driver.
 *
 * Owns the W5500 SPI bring-up, netif, and the ETH/IP event handlers. It
 * publishes link/IP state through an event group so consumers (the ping task
 * now, network_manager later) can wait on it without re-registering handlers.
 *
 * Phase-A note: extracted verbatim from network_comm_task.c. Behavior is
 * unchanged; network_comm_task still owns the ping task and calls this driver.
 * Infrastructure init (NVS / netif / event loop) is intentionally left here for
 * now and moves to network_manager in a later step.
 */

#define ETHERNET_DRIVER_GOT_IP_BIT BIT0

/* Bring up the W5500 and start Ethernet. Registers ETH/IP event handlers and
 * begins driving the event group. Safe to call once. */
esp_err_t ethernet_driver_init(void);

/* Event group carrying ETHERNET_DRIVER_GOT_IP_BIT (set on got-IP, cleared on
 * link down). NULL until ethernet_driver_init() has run. */
EventGroupHandle_t ethernet_driver_event_group(void);

/* True if Ethernet currently holds a valid IP. */
bool ethernet_driver_has_ip(void);

/* True if the Ethernet PHY link is currently up (cable connected + negotiated).
 * Tracks ETHERNET_EVENT_CONNECTED/DISCONNECTED. This is link state only; an IP
 * may not be assigned yet. Used by network_manager for failover decisions. */
bool ethernet_driver_link_is_up(void);

/* The Ethernet netif handle, or NULL before ethernet_driver_init() has run.
 * Used by network_manager to set the default netif on interface switches. */
esp_netif_t *ethernet_driver_netif(void);

#ifdef __cplusplus
}
#endif

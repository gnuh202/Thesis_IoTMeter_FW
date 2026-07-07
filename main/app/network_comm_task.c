#include "network_comm_task.h"

#include <inttypes.h>
#include <string.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "ethernet_driver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "sdkconfig.h"

/*
 * Network connectivity task: brings up the W5500 Ethernet driver and runs a
 * periodic reachability ping once an IP is obtained.
 *
 * Phase-A note: the W5500 bring-up and ETH/IP event handling were moved into
 * ethernet_driver. This task now just starts that driver and waits on its
 * got-IP event group before pinging. Behavior is unchanged.
 */

#define NETWORK_COMM_PING_TARGET "8.8.8.8"

static const char *TAG = "network_comm";

static void ping_success_callback(esp_ping_handle_t handle, void *args)
{
    uint8_t ttl = 0;
    uint16_t seqno = 0;
    uint32_t elapsed_time = 0;
    uint32_t recv_len = 0;
    ip_addr_t target_addr;

    esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(handle, ESP_PING_PROF_TTL, &ttl, sizeof(ttl));
    esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed_time, sizeof(elapsed_time));
    esp_ping_get_profile(handle, ESP_PING_PROF_SIZE, &recv_len, sizeof(recv_len));
    esp_ping_get_profile(handle, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));

    ESP_LOGI(TAG, "ping %s: seq=%u ttl=%u time=%" PRIu32 " ms size=%" PRIu32,
             ipaddr_ntoa(&target_addr), seqno, ttl, elapsed_time, recv_len);
}

static void ping_timeout_callback(esp_ping_handle_t handle, void *args)
{
    uint16_t seqno = 0;
    ip_addr_t target_addr;

    esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &seqno, sizeof(seqno));
    esp_ping_get_profile(handle, ESP_PING_PROF_IPADDR, &target_addr, sizeof(target_addr));

    ESP_LOGW(TAG, "ping %s timeout: seq=%u", ipaddr_ntoa(&target_addr), seqno);
}

static void ping_end_callback(esp_ping_handle_t handle, void *args)
{
    uint32_t transmitted = 0;
    uint32_t received = 0;
    uint32_t total_time_ms = 0;

    esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST, &transmitted, sizeof(transmitted));
    esp_ping_get_profile(handle, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_get_profile(handle, ESP_PING_PROF_DURATION, &total_time_ms, sizeof(total_time_ms));

    uint32_t loss = transmitted == 0 ? 0 : ((transmitted - received) * 100) / transmitted;
    ESP_LOGI(TAG, "ping finished: transmitted=%" PRIu32 " received=%" PRIu32 " loss=%" PRIu32 "%% duration=%" PRIu32 " ms",
             transmitted, received, loss, total_time_ms);

    esp_ping_delete_session(handle);
}

static esp_err_t network_ping_once(void)
{
    struct sockaddr_in sock_addr4;
    ip_addr_t target_addr;

    memset(&target_addr, 0, sizeof(target_addr));
    memset(&sock_addr4, 0, sizeof(sock_addr4));

    if (inet_pton(AF_INET, NETWORK_COMM_PING_TARGET, &sock_addr4.sin_addr) == 1) {
        inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &sock_addr4.sin_addr);
    } else {
        struct addrinfo hint;
        struct addrinfo *res = NULL;
        memset(&hint, 0, sizeof(hint));

        if (getaddrinfo(NETWORK_COMM_PING_TARGET, NULL, &hint, &res) != 0) {
            ESP_LOGE(TAG, "resolve ping target failed: %s", NETWORK_COMM_PING_TARGET);
            return ESP_FAIL;
        }

        if (res->ai_family == AF_INET) {
            struct in_addr addr4 = ((struct sockaddr_in *)(res->ai_addr))->sin_addr;
            inet_addr_to_ip4addr(ip_2_ip4(&target_addr), &addr4);
        }
        freeaddrinfo(res);
    }

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = 4;
    ping_config.interval_ms = 1000;
    ping_config.timeout_ms = 1000;

    esp_ping_callbacks_t callbacks = {
        .cb_args = NULL,
        .on_ping_success = ping_success_callback,
        .on_ping_timeout = ping_timeout_callback,
        .on_ping_end = ping_end_callback,
    };

    esp_ping_handle_t ping_handle = NULL;
    ESP_RETURN_ON_ERROR(esp_ping_new_session(&ping_config, &callbacks, &ping_handle), TAG, "create ping session failed");
    return esp_ping_start(ping_handle);
}

static void network_comm_task(void *arg)
{
    /*
     * Ping whichever interface network_manager currently reports as active
     * (Ethernet, or WiFi STA after a failover). We poll the manager's status
     * instead of waiting on the Ethernet-only event group, so reachability
     * checks follow the data path across interface switches.
     */
    while (1) {
        network_status_t status;
        bool has_ip = (network_manager_get_status(&status) == ESP_OK) && status.has_ip;

        if (has_ip) {
            esp_err_t ret = network_ping_once();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "start ping failed: %s", esp_err_to_name(ret));
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_APP_NETWORK_PING_PERIOD_MS));
        } else {
            /* No active data path yet; poll until one comes up. */
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

esp_err_t network_comm_task_start(void)
{
    ESP_RETURN_ON_ERROR(ethernet_driver_init(), TAG, "init W5500 Ethernet failed");

    BaseType_t ret = xTaskCreate(network_comm_task,
                                 "network_comm_task",
                                 CONFIG_APP_NETWORK_COMM_TASK_STACK_SIZE,
                                 NULL,
                                 CONFIG_APP_NETWORK_COMM_TASK_PRIORITY,
                                 NULL);
    ESP_RETURN_ON_FALSE(ret == pdPASS, ESP_FAIL, TAG, "create network comm task failed");

    return ESP_OK;
}

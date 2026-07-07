#include "ethernet_driver.h"

#include <inttypes.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_netif_glue.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "io_expander.h"
#include "network_manager.h"
#include "sdkconfig.h"

/*
 * W5500 Ethernet driver. Extracted verbatim from network_comm_task.c during
 * phase-A network refactor; behavior is unchanged. Shared infrastructure init
 * (NVS / netif / event loop) is now owned by network_manager_infra_init(),
 * which must run before this driver.
 */

static const char *TAG = "ethernet_driver";

static EventGroupHandle_t s_event_group;
static esp_eth_handle_t s_eth_handle;
static esp_eth_netif_glue_handle_t s_eth_netif_glue;
static esp_netif_t *s_eth_netif;
static volatile bool s_link_up;

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        s_link_up = true;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down");
        s_link_up = false;
        if (s_event_group != NULL) {
            xEventGroupClearBits(s_event_group, ETHERNET_DRIVER_GOT_IP_BIT);
        }
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet got IP");
    ESP_LOGI(TAG, "IP: " IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "NETMASK: " IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "GATEWAY: " IPSTR, IP2STR(&ip_info->gw));

    xEventGroupSetBits(s_event_group, ETHERNET_DRIVER_GOT_IP_BIT);
}

esp_err_t ethernet_driver_init(void)
{
    if (s_event_group == NULL) {
        s_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create event group failed");
    }

    ESP_RETURN_ON_ERROR(network_manager_infra_init(), TAG, "init network infra failed");

    ESP_RETURN_ON_ERROR(io_expander_w5500_reset_pulse(), TAG, "reset W5500 failed");

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(s_eth_netif != NULL, ESP_ERR_NO_MEM, TAG, "create Ethernet netif failed");

    spi_bus_config_t bus_config = {
        .mosi_io_num = CONFIG_APP_W5500_SPI_MOSI_GPIO,
        .miso_io_num = CONFIG_APP_W5500_SPI_MISO_GPIO,
        .sclk_io_num = CONFIG_APP_W5500_SPI_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };

    spi_host_device_t spi_host = (CONFIG_APP_W5500_SPI_HOST == 1) ? SPI2_HOST : SPI3_HOST;

    esp_err_t ret = spi_bus_initialize(spi_host, &bus_config, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "init W5500 SPI bus failed");
    }

    spi_device_interface_config_t dev_config = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = CONFIG_APP_W5500_SPI_CLOCK_MHZ * 1000 * 1000,
        .spics_io_num = CONFIG_APP_W5500_SPI_CS_GPIO,
        .queue_size = 20,
    };

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(spi_host, &dev_config);
    w5500_config.int_gpio_num = CONFIG_APP_W5500_INT_GPIO;

    esp_err_t gpio_isr_ret = gpio_install_isr_service(0);
    if (gpio_isr_ret != ESP_OK && gpio_isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(gpio_isr_ret, TAG, "install GPIO ISR service failed");
    }

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac != NULL, ESP_ERR_NO_MEM, TAG, "create W5500 MAC failed");

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    ESP_RETURN_ON_FALSE(phy != NULL, ESP_ERR_NO_MEM, TAG, "create W5500 PHY failed");

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth_handle), TAG, "install Ethernet driver failed");

    s_eth_netif_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_FALSE(s_eth_netif_glue != NULL, ESP_ERR_NO_MEM, TAG, "create netif glue failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, s_eth_netif_glue), TAG, "attach Ethernet netif failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL), TAG, "register ETH event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler, NULL), TAG, "register IP event failed");

    return esp_eth_start(s_eth_handle);
}

EventGroupHandle_t ethernet_driver_event_group(void)
{
    return s_event_group;
}

bool ethernet_driver_has_ip(void)
{
    if (s_event_group == NULL) {
        return false;
    }
    return (xEventGroupGetBits(s_event_group) & ETHERNET_DRIVER_GOT_IP_BIT) != 0;
}

esp_netif_t *ethernet_driver_netif(void)
{
    return s_eth_netif;
}

bool ethernet_driver_link_is_up(void)
{
    return s_link_up;
}

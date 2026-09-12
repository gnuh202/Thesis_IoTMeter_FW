#include "ethernet_driver.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include "config_manager.h"
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
#include "esp_mac.h"
#include "esp_timer.h"
#include "gpio_isr_service.h"
#include "io_expander.h"
#include "network_manager.h"
#include "sdkconfig.h"
#include "system_status.h"

/*
 * W5500 Ethernet driver. Extracted verbatim from network_comm_task.c during
 * phase-A network refactor; behavior is unchanged. Shared infrastructure init
 * (NVS / netif / event loop) is now owned by network_manager_infra_init(),
 * which must run before this driver.
 */

static const char *TAG = "ethernet_driver";

#ifndef CONFIG_APP_NET_ETH_DHCP_WARN_S
#define CONFIG_APP_NET_ETH_DHCP_WARN_S 15
#endif

static EventGroupHandle_t s_event_group;
static esp_eth_handle_t s_eth_handle;
static esp_eth_netif_glue_handle_t s_eth_netif_glue;
static esp_netif_t *s_eth_netif;
static volatile bool s_link_up;
static esp_timer_handle_t s_dhcp_timer;

/*
 * Log what the link actually negotiated plus the DHCP client state, at the
 * moment the link comes up.
 *
 * Without this the log jumps straight from "link up" to either a got-IP line or
 * nothing at all, which cannot distinguish "DISCOVER was never sent" (netif
 * down, or dhcpc still parked in INIT) from "DISCOVER was sent and the peer
 * never answered". The first is a firmware bug, the second is the network.
 */
static void log_link_details(void)
{
    eth_speed_t speed = ETH_SPEED_10M;
    eth_duplex_t duplex = ETH_DUPLEX_HALF;
    uint8_t mac[6] = {0};

    if (s_eth_handle != NULL) {
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
        esp_eth_ioctl(s_eth_handle, ETH_CMD_G_MAC_ADDR, mac);
    }

    ESP_LOGI(TAG, "link: %s / %s mac=%02x:%02x:%02x:%02x:%02x:%02x",
             (speed == ETH_SPEED_100M) ? "100Mbps" : "10Mbps",
             (duplex == ETH_DUPLEX_FULL) ? "full-duplex" : "half-duplex",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    if (s_eth_netif == NULL) {
        return;
    }

    /* netif_up must be true before lwIP will send a DISCOVER at all, and
     * dhcpc_status must be STARTED. INIT here means esp_netif_action_connected
     * has not run yet (it is queued behind us on the same event loop). */
    esp_netif_dhcp_status_t dhcpc = ESP_NETIF_DHCP_INIT;
    esp_netif_dhcpc_get_status(s_eth_netif, &dhcpc);
    ESP_LOGI(TAG, "netif: up=%d dhcpc=%s",
             (int)esp_netif_is_netif_up(s_eth_netif),
             (dhcpc == ESP_NETIF_DHCP_STARTED) ? "STARTED" :
             (dhcpc == ESP_NETIF_DHCP_STOPPED) ? "STOPPED" : "INIT");
}

/*
 * One-shot timer armed on link up and cancelled by got-IP. If it fires, DHCP has
 * been running for the whole window without a reply, so the DHCP client state at
 * that point tells us whether the board is still soliciting (peer silent) or
 * never started (firmware). This is the case the router log was missing.
 */
static void dhcp_timeout_cb(void *arg)
{
    if (s_eth_netif == NULL || ethernet_driver_has_ip()) {
        return;
    }

    esp_netif_dhcp_status_t dhcpc = ESP_NETIF_DHCP_INIT;
    esp_netif_dhcpc_get_status(s_eth_netif, &dhcpc);
    ESP_LOGW(TAG, "no IP %d s after link up: netif_up=%d dhcpc=%s",
             CONFIG_APP_NET_ETH_DHCP_WARN_S,
             (int)esp_netif_is_netif_up(s_eth_netif),
             (dhcpc == ESP_NETIF_DHCP_STARTED) ? "STARTED (DISCOVER sent, no reply from DHCP server)" :
             (dhcpc == ESP_NETIF_DHCP_STOPPED) ? "STOPPED (static IP mode)" :
             "INIT (client never started - firmware side)");
}

static void arm_dhcp_watchdog(void)
{
    if (s_dhcp_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = dhcp_timeout_cb,
            .name = "eth_dhcp_wd",
        };
        if (esp_timer_create(&args, &s_dhcp_timer) != ESP_OK) {
            return;
        }
    }
    esp_timer_stop(s_dhcp_timer);   /* ESP_ERR_INVALID_STATE if idle: harmless */
    esp_timer_start_once(s_dhcp_timer, (uint64_t)CONFIG_APP_NET_ETH_DHCP_WARN_S * 1000000ULL);
}

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        s_link_up = true;
        system_status_set(SYS_MODULE_ETHERNET, SYS_STATUS_READY);
        log_link_details();
        arm_dhcp_watchdog();
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Ethernet link down");
        s_link_up = false;
        system_status_set(SYS_MODULE_ETHERNET, SYS_STATUS_OFFLINE);
        /* Cable pulled before DHCP finished is not a DHCP problem. */
        if (s_dhcp_timer != NULL) {
            esp_timer_stop(s_dhcp_timer);
        }
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

    /* DHCP (or the static config) succeeded — the no-IP warning is not wanted. */
    if (s_dhcp_timer != NULL) {
        esp_timer_stop(s_dhcp_timer);
    }

    xEventGroupSetBits(s_event_group, ETHERNET_DRIVER_GOT_IP_BIT);
}

/*
 * Apply the stored IP configuration to the Ethernet netif.
 *
 * Until now this was a log-only stub in config_apply, so the DHCP/static fields
 * in NVS had no effect and the interface always ran the netif default (DHCP).
 * Anything that cannot be parsed leaves DHCP in place rather than bringing the
 * interface up with a half-applied static address, which would be worse than
 * the documented default.
 */
esp_err_t ethernet_driver_apply_ip(void)
{
    ESP_RETURN_ON_FALSE(s_eth_netif != NULL, ESP_ERR_INVALID_STATE, TAG, "Ethernet netif not created yet");

    /* config_manager_t is ~2.2 KB — allocate on heap to avoid stack overflow. */
    config_manager_t *cfg = malloc(sizeof(config_manager_t));
    if (cfg == NULL) {
        ESP_LOGE(TAG, "out of memory reading network config; leaving DHCP enabled");
        return ESP_OK;
    }
    esp_err_t ret = config_manager_get(cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "config_manager not ready (%s); leaving DHCP enabled", esp_err_to_name(ret));
        free(cfg);
        return ESP_OK;
    }

    if (cfg->dhcp_enable) {
        ret = esp_netif_dhcpc_start(s_eth_netif);
        if (ret == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ret = ESP_OK;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "start DHCP client failed: %s", esp_err_to_name(ret));
        } else {
            ESP_LOGI(TAG, "IP config: DHCP");
        }
        free(cfg);
        return ret;
    }

    esp_netif_ip_info_t ip_info = {0};
    if (esp_netif_str_to_ip4(cfg->static_ip, &ip_info.ip) != ESP_OK ||
        esp_netif_str_to_ip4(cfg->netmask,   &ip_info.netmask) != ESP_OK ||
        esp_netif_str_to_ip4(cfg->gateway,   &ip_info.gw) != ESP_OK) {
        ESP_LOGE(TAG, "static IP config incomplete (ip=\"%s\" netmask=\"%s\" gw=\"%s\"); staying on DHCP",
                 cfg->static_ip, cfg->netmask, cfg->gateway);
        free(cfg);
        return ESP_OK;
    }

    /* DHCP must be stopped before a manual address is accepted. */
    ret = esp_netif_dhcpc_stop(s_eth_netif);
    if (ret != ESP_OK && ret != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "stop DHCP client failed: %s", esp_err_to_name(ret));
        free(cfg);
        return ret;
    }
    ret = esp_netif_set_ip_info(s_eth_netif, &ip_info);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set static IP failed: %s", esp_err_to_name(ret));
        free(cfg);
        return ret;
    }

    if (strlen(cfg->dns) > 0) {
        esp_netif_dns_info_t dns = {0};
        if (esp_netif_str_to_ip4(cfg->dns, &dns.ip.u_addr.ip4) == ESP_OK) {
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            esp_err_t dns_ret = esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns);
            if (dns_ret != ESP_OK) {
                ESP_LOGW(TAG, "set DNS failed: %s", esp_err_to_name(dns_ret));
            }
        } else {
            ESP_LOGW(TAG, "DNS \"%s\" is not a valid IPv4 address; ignored", cfg->dns);
        }
    }

    ESP_LOGI(TAG, "IP config: static ip=%s netmask=%s gw=%s dns=%s",
             cfg->static_ip, cfg->netmask, cfg->gateway, cfg->dns);
    free(cfg);
    return ESP_OK;
}

esp_err_t ethernet_driver_init(void)
{
    if (s_event_group == NULL) {
        s_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_event_group != NULL, ESP_ERR_NO_MEM, TAG, "create event group failed");
    }

    ESP_RETURN_ON_ERROR(network_manager_infra_init(), TAG, "init network infra failed");

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

    /* The W5500 SPI MAC installs the global GPIO ISR service itself for its INT
     * pin. If another driver (io_expander) already installed it, ESP-IDF logs a
     * noisy "GPIO isr service already installed" error even though it is benign.
     * Pre-install it through our idempotent helper and silence the gpio tag for
     * the window where esp_eth may re-install, then restore the level. */
    esp_err_t gpio_isr_ret = gpio_isr_service_ensure_installed(0);
    ESP_RETURN_ON_ERROR(gpio_isr_ret, TAG, "install GPIO ISR service failed");

    esp_log_level_t prev_gpio_log = esp_log_level_get("gpio");
    esp_log_level_set("gpio", ESP_LOG_NONE);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    /* The esp_eth W5500 MAC polls both the MR reset bit and VERSIONR for
     * sw_reset_timeout_ms, retrying every 10ms. Its own source notes that some
     * W5500 units return version 0x00 when read right after reset, and the
     * 100ms default allows only 10 attempts before failing with
     * ESP_ERR_INVALID_VERSION - which is exactly the intermittent boot failure
     * seen on this board. A longer window costs nothing on a healthy boot
     * because the loop returns on the first good read. */
    mac_config.sw_reset_timeout_ms = CONFIG_APP_W5500_SW_RESET_TIMEOUT_MS;
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (mac == NULL) {
        esp_log_level_set("gpio", prev_gpio_log);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "create W5500 MAC failed");
    }

    /* Pulse RESETn only now, not earlier. The W5500 latches its SPI mode as it
     * comes out of reset and requires SCSn high across that window, but CS is
     * only driven once spi_bus_add_device() runs - which happens inside
     * esp_eth_mac_new_w5500() above. Resetting before that leaves CS a floating
     * input for the whole assert + settle window, so whether the chip latches a
     * valid SPI state depends on residual charge and leakage. That is the
     * intermittent "VERSIONR reads 0x00" failure: the reset pulse itself is
     * clean on a logic analyzer, but the chip was released into an undefined bus
     * state. A longer sw_reset_timeout_ms cannot recover from that - retries
     * only help a chip that needs more time, not one that latched wrong. */
    esp_err_t reset_ret = io_expander_w5500_reset_pulse();
    if (reset_ret != ESP_OK) {
        esp_log_level_set("gpio", prev_gpio_log);
        mac->del(mac);
        ESP_RETURN_ON_ERROR(reset_ret, TAG, "reset W5500 failed");
    }

    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (phy == NULL) {
        esp_log_level_set("gpio", prev_gpio_log);
        ESP_RETURN_ON_FALSE(false, ESP_ERR_NO_MEM, TAG, "create W5500 PHY failed");
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t install_ret = esp_eth_driver_install(&eth_config, &s_eth_handle);
    esp_log_level_set("gpio", prev_gpio_log);
    ESP_RETURN_ON_ERROR(install_ret, TAG, "install Ethernet driver failed");

    /*
     * Give the interface its MAC address.
     *
     * Unlike the ESP32 internal EMAC, the W5500 has no MAC of its own and
     * esp_eth_driver_install() does not seed one: emac_w5500_get_addr() just
     * returns the cached emac->addr, which stays all-zero until someone calls
     * set_addr. Running with 00:00:00:00:00:00 is not merely cosmetic — that
     * address ends up in the W5500 MAC filter register and, via
     * esp_eth_post_attach(), in the netif, so every frame we send has an invalid
     * source MAC. A Windows ICS software bridge tolerates it and answers DHCP
     * anyway; a real router drops such frames, so DISCOVER never gets a reply.
     * That was exactly the "works on laptop share, no IP from router" split.
     *
     * This must run before esp_netif_attach() below, because the glue snapshots
     * the MAC into the netif at attach time and never re-reads it.
     */
    uint8_t mac_addr[6] = {0};
    esp_err_t mac_ret = esp_read_mac(mac_addr, ESP_MAC_ETH);
    if (mac_ret == ESP_OK) {
        ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr), TAG,
                            "set Ethernet MAC address failed");
        ESP_LOGI(TAG, "MAC address %02x:%02x:%02x:%02x:%02x:%02x (from efuse)",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    } else {
        ESP_LOGE(TAG, "read efuse Ethernet MAC failed: %s; interface would run with an "
                 "all-zero MAC and be dropped by most switches/routers", esp_err_to_name(mac_ret));
        return mac_ret;
    }

    s_eth_netif_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_FALSE(s_eth_netif_glue != NULL, ESP_ERR_NO_MEM, TAG, "create netif glue failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, s_eth_netif_glue), TAG, "attach Ethernet netif failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event_handler, NULL), TAG, "register ETH event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, got_ip_event_handler, NULL), TAG, "register IP event failed");

    /* Apply DHCP/static before starting, so the interface never briefly runs
     * DHCP when the user configured a fixed address. */
    esp_err_t ip_ret = ethernet_driver_apply_ip();
    if (ip_ret != ESP_OK) {
        ESP_LOGW(TAG, "apply IP config failed: %s; continuing with netif default",
                 esp_err_to_name(ip_ret));
    }

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

#include "config_apply.h"

#include <stdlib.h>

#include "config_manager.h"
#include "esp_log.h"
#include "ethernet_driver.h"
#include "home_screen.h"
#include "modbus_master_task.h"
#include "modbus_slave_task.h"
#include "mqtt_manager.h"
#include "wifi_manager.h"

/*
 * Configuration Apply Engine (Feature 11 framework, Feature 14 MQTT Apply).
 *
 * Every domain is a real handler now: each pushes the RAM snapshot into the
 * running drivers. Nothing here writes or reloads NVS — Apply and Save stay
 * separate.
 */

static const char *TAG = "config_apply";

static esp_err_t apply_ethernet(const config_manager_t *cfg)
{
    ESP_LOGI(TAG, "Apply Ethernet: dhcp=%d ip=%s gateway=%s netmask=%s dns=%s",
             (int)cfg->dhcp_enable, cfg->static_ip, cfg->gateway, cfg->netmask, cfg->dns);

    /* ethernet_driver reads the RAM snapshot from config_manager itself, so this
     * only has to trigger the re-apply. Before the driver has created its netif
     * there is nothing to apply — init() will pick the config up, so that is not
     * an error the caller should see. */
    esp_err_t ret = ethernet_driver_apply_ip();
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "Ethernet not up yet; IP config applies at driver init");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "apply Ethernet IP failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t apply_wifi(const config_manager_t *cfg)
{
    ESP_LOGI(TAG, "Apply WiFi: ssid=%s", cfg->wifi_ssid);

    /* Push the credentials into the WiFi driver. This does not associate — the
     * STA is brought up only by network_manager's ETH>STA failover, so applying
     * here must not steal that decision. */
    esp_err_t ret = wifi_manager_sta_set_credentials(cfg->wifi_ssid, cfg->wifi_pass);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "WiFi not started yet; credentials apply at manager start");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "apply WiFi credentials failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Only reconnect if the STA was already the active data path: a live
     * reassociation with the new SSID. If the STA is down, network_manager owns
     * when it comes up and will use these credentials then. */
    if (wifi_manager_sta_is_enabled()) {
        ESP_LOGI(TAG, "STA active; reassociating with the new credentials");
        /* Drop first: esp_wifi_connect() on an already-associated STA returns
         * ESP_ERR_WIFI_CONN and keeps the old AP, so the new SSID would never
         * take effect without the disconnect. */
        wifi_manager_sta_disconnect();
        esp_err_t rc_ret = wifi_manager_sta_connect();
        if (rc_ret != ESP_OK) {
            ESP_LOGW(TAG, "STA reassociate request failed: %s", esp_err_to_name(rc_ret));
            return rc_ret;
        }
    }
    return ESP_OK;
}

static esp_err_t apply_mqtt(void)
{
    ESP_LOGI(TAG, "Apply MQTT: recreating client from the active RAM profile");
    esp_err_t ret = mqtt_manager_apply();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Apply MQTT failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* Asynchronous: mqtt_manager_apply only queues the request; the actual
     * teardown/rebuild runs in the mqtt task (esp_mqtt_client_stop can take
     * seconds without a network and must never block the caller's UI). */
    ESP_LOGI(TAG, "Apply MQTT queued; client rebuild runs in the mqtt task");
    return ESP_OK;
}

static esp_err_t apply_modbus_master(const config_manager_t *cfg)
{
    unsigned used = 0;
    for (unsigned i = 0; i < CONFIG_MANAGER_MB_SLOT_COUNT; i++) {
        if (cfg->mb_slots[i].used) {
            used++;
        }
    }
    ESP_LOGI(TAG, "Apply Modbus Master: bus_en=%d baud=%u parity=%u period=%ums slots=%u",
             (int)cfg->mb_enabled, cfg->mb_baud_code, cfg->mb_parity_code,
             (unsigned)cfg->mb_poll_period_ms, used);

    /* Master task re-reads the RAM snapshot and restarts its RTU stack. */
    esp_err_t ret = modbus_master_reconfigure();
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "Modbus master not started yet; config applies at task start");
        ret = ESP_OK;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Apply Modbus master failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t apply_modbus_slave(const config_manager_t *cfg)
{
    ESP_LOGI(TAG, "Apply Modbus Slave: slave_id=0x%02X slave_baud_code=%u",
             (unsigned)cfg->mb_slave_id, (unsigned)cfg->mb_slave_baud_code);

    ESP_LOGI(TAG, "Modbus Slave configuration is LCD/NVS-owned; live link unchanged");
    return ESP_OK;
}

static esp_err_t apply_lcd(const config_manager_t *cfg)
{
    ESP_LOGI(TAG, "Apply LCD: backlight=%d sleep_timeout_s=%u",
             (int)cfg->lcd_backlight, (unsigned)cfg->lcd_sleep_timeout_s);

    /* The Home Screen task owns the LCD, so it re-reads the RAM snapshot and
     * drives the panel itself; this only raises the request. In engineering mode
     * that task does not exist (hmi_test_task holds the LCD instead) — the
     * settings are picked up when the Home Screen next starts, so that is not an
     * error the caller should see. */
    esp_err_t ret = home_screen_apply_config();
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "Home screen not running; LCD config applies at its start");
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Apply LCD failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static esp_err_t apply_measurement(const config_manager_t *cfg)
{
    (void)cfg;
    /* PGA and line frequency are owned only by console (meter-cal set) and
     * Kconfig defaults (plus NVS restore of a console-saved calib blob).
     * CONFIG_APPLY_MEASUREMENT no longer pushes line_freq onto the chip. */
    ESP_LOGI(TAG, "Apply Measurement: no-op (PGA/line_freq = console+Kconfig only)");
    return ESP_OK;
}

const char *config_apply_flag_name(config_apply_flags_t flag)
{
    switch (flag) {
    case CONFIG_APPLY_ETHERNET:        return "ETHERNET";
    case CONFIG_APPLY_WIFI:            return "WIFI";
    case CONFIG_APPLY_MQTT:            return "MQTT";
    case CONFIG_APPLY_MODBUS_SLAVE:    return "MODBUS_SLAVE";
    case CONFIG_APPLY_MODBUS_MASTER:   return "MODBUS_MASTER";
    case CONFIG_APPLY_MODBUS:          return "MODBUS";
    case CONFIG_APPLY_LCD:             return "LCD";
    case CONFIG_APPLY_MEASUREMENT:     return "MEASUREMENT";
    case CONFIG_APPLY_ALARM:           return "ALARM";
    case CONFIG_APPLY_NONE:            return "NONE";
    case CONFIG_APPLY_ALL:             return "ALL";
    default:                           return "unknown";
    }
}

esp_err_t config_apply(config_apply_flags_t flags)
{
    if (flags == CONFIG_APPLY_NONE || (flags & ~CONFIG_APPLY_ALL) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Only the handlers that need the large snapshot malloc it. MQTT and
     * Measurement reload their own state directly, so they run even if the shared
     * snapshot cannot be obtained. */
    config_manager_t *cfg = NULL;
    esp_err_t first_err = ESP_OK;
    if ((flags & (CONFIG_APPLY_ETHERNET | CONFIG_APPLY_WIFI |
                  CONFIG_APPLY_MODBUS_SLAVE | CONFIG_APPLY_MODBUS_MASTER |
                  CONFIG_APPLY_LCD | CONFIG_APPLY_ALARM)) != 0) {
        cfg = malloc(sizeof(*cfg));
        if (cfg == NULL) {
            first_err = ESP_ERR_NO_MEM;
            ESP_LOGE(TAG, "no memory for non-MQTT Apply snapshot");
        } else {
            esp_err_t err = config_manager_get(cfg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "config_manager_get failed (%s); non-MQTT domains not applied",
                         esp_err_to_name(err));
                first_err = err;
                free(cfg);
                cfg = NULL;
            }
        }
    }

    /* Fixed order so the result never depends on how the caller combined the
     * bits. Continue after a handler error and return the first one. */
    if ((flags & CONFIG_APPLY_ETHERNET) && cfg != NULL) {
        esp_err_t apply_err = apply_ethernet(cfg);
        if (first_err == ESP_OK && apply_err != ESP_OK) {
            first_err = apply_err;
        }
    }

    if ((flags & CONFIG_APPLY_WIFI) && cfg != NULL) {
        esp_err_t apply_err = apply_wifi(cfg);
        if (first_err == ESP_OK && apply_err != ESP_OK) {
            first_err = apply_err;
        }
    }

    if (flags & CONFIG_APPLY_MQTT) {
        esp_err_t apply_err = apply_mqtt();
        if (first_err == ESP_OK && apply_err != ESP_OK) {
            first_err = apply_err;
        }
    }

    /* Modbus is split: the master and slave stacks are independent, so the
     * caller picks which one to rebuild. CONFIG_APPLY_MODBUS expands to both
     * for the legacy `apply modbus` console command. Master is rebuilt first
     * so it sees the new bus config before the slave answers on it. */
    if ((flags & CONFIG_APPLY_MODBUS_MASTER) && cfg != NULL) {
        esp_err_t apply_err = apply_modbus_master(cfg);
        if (first_err == ESP_OK && apply_err != ESP_OK) {
            first_err = apply_err;
        }
    }

    if ((flags & CONFIG_APPLY_MODBUS_SLAVE) && cfg != NULL) {
        esp_err_t apply_err = apply_modbus_slave(cfg);
        if (first_err == ESP_OK && apply_err != ESP_OK) {
            first_err = apply_err;
        }
    }

    if ((flags & CONFIG_APPLY_LCD) && cfg != NULL) {
        esp_err_t apply_err = apply_lcd(cfg);
        if (first_err == ESP_OK && apply_err != ESP_OK) {
            first_err = apply_err;
        }
    }

    if (flags & CONFIG_APPLY_MEASUREMENT) {
        /* Measurement apply needs the snapshot for line_freq + wiring_mode only.
         * Reuse the shared malloc if it exists, or allocate temporarily. */
        config_manager_t *meas_cfg = cfg;
        bool free_meas = false;
        if (meas_cfg == NULL) {
            meas_cfg = malloc(sizeof(*meas_cfg));
            if (meas_cfg == NULL) {
                ESP_LOGE(TAG, "no memory for Measurement Apply");
                if (first_err == ESP_OK) {
                    first_err = ESP_ERR_NO_MEM;
                }
            } else {
                esp_err_t err = config_manager_get(meas_cfg);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "config_manager_get failed (%s); Measurement not applied",
                             esp_err_to_name(err));
                    if (first_err == ESP_OK) {
                        first_err = err;
                    }
                    free(meas_cfg);
                    meas_cfg = NULL;
                } else {
                    free_meas = true;
                }
            }
        }
        if (meas_cfg != NULL) {
            esp_err_t apply_err = apply_measurement(meas_cfg);
            if (first_err == ESP_OK && apply_err != ESP_OK) {
                first_err = apply_err;
            }
            if (free_meas) {
                free(meas_cfg);
            }
        }
    }

    if ((flags & CONFIG_APPLY_ALARM) && cfg != NULL) {
        ESP_LOGI(TAG,
                 "Apply Alarm Settings: nominal=%uHz trigger=%us clear=%us (detection deferred)",
                 (unsigned)cfg->alarm_nominal_frequency_hz,
                 (unsigned)cfg->alarm_trigger_delay_s,
                 (unsigned)cfg->alarm_clear_delay_s);
    }

    if (cfg != NULL) {
        free(cfg);
    }
    return first_err;
}

esp_err_t config_apply_all(void)
{
    return config_apply(CONFIG_APPLY_ALL);
}

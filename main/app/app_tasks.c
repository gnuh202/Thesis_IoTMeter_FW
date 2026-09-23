#include "app_tasks.h"

#include "boot_manager.h"
#include "cert_store.h"
#include "config_manager.h"
#include "console_task.h"
#include "energy_meter_task.h"
#include "home_screen.h"
#include "hmi_test_task.h"
#include "io_expander.h"
#include "modbus_master_task.h"
#include "modbus_slave_task.h"
#include "modbus_tcp_task.h"
#include "mqtt_manager.h"
#include "network_comm_task.h"
#include "network_manager.h"
#include "ota_manager.h"
#include "sd_card.h"
#include "system_status.h"
#include "time_source.h"
#include "wifi_manager.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "app_tasks";

/* Map a boot step result to READY/ERROR for the status registry. */
static system_status_state_t boot_state(esp_err_t ret)
{
    return ret == ESP_OK ? SYS_STATUS_READY : SYS_STATUS_ERROR;
}

esp_err_t app_tasks_start(void)
{
    /* Central status registry: all modules start UNKNOWN until they report. */
    system_status_init();

    /* Core system infrastructure (NVS / netif / event loop). This is the only
     * class of failure allowed to abort boot: without NVS the config store and
     * every consumer that reads it cannot work. */
    ESP_RETURN_ON_ERROR(network_manager_infra_init(), TAG, "init network infra failed");

    /* Central configuration snapshot: load from NVS (or defaults) into RAM now
     * that NVS is up, before consumers read. Load-only: does not apply settings
     * or notify modules, so boot behavior is unchanged. */
    ESP_RETURN_ON_ERROR(config_manager_init(), TAG, "init configuration manager failed");

    /* OTA bookkeeping. Runs this early for one reason: if the bootloader
     * started us as PENDING_VERIFY, the self-test clock should begin at the
     * top of boot, not after the slowest peripheral. It starts a timer and
     * reads one partition entry -- no network, no task. */
    esp_err_t ota_ret = ota_manager_init();
    if (ota_ret != ESP_OK) {
        ESP_LOGW(TAG, "OTA manager init failed: %s", esp_err_to_name(ota_ret));
    }

    /* Wall-clock abstraction: timezone, boot counter, and the DS1307 on the
     * shared I2C bus. Runs here, before boot_manager_begin(), because the
     * energy task must find the clock already established when it writes its
     * first CSV row. A missing or untrusted RTC is non-fatal — stamps fall
     * back to the NVS floor ('E') or 1970 ('U'), never to a fabricated date.
     *
     * Reported through system_status only: boot_manager_step() draws to an LCD
     * that does not exist yet at this point in boot. */
    esp_err_t time_ret = time_source_init();
    if (time_ret != ESP_OK) {
        ESP_LOGW(TAG, "time source init failed: %s", esp_err_to_name(time_ret));
    }
    system_status_set(SYS_MODULE_RTC,
                      time_source_rtc_present() ? SYS_STATUS_READY : SYS_STATUS_ERROR);

    /* Boot UI: bring up the LCD + PCF8575 (idempotent hmi_bsp_init), show the
     * splash, then the "Initializing..." progress screen. LCD failure is
     * non-fatal — boot continues headless. */
    boot_manager_begin();

    /* Read the engineering-mode key combo now that buttons are up. */
    bool engineering_mode = boot_manager_engineering_mode();
    if (engineering_mode) {
        ESP_LOGW(TAG, "============================================");
        ESP_LOGW(TAG, "ENGINEERING MODE ACTIVE");
        ESP_LOGW(TAG, "Only calibration-related tasks enabled:");
        ESP_LOGW(TAG, "  - energy_meter (calibration needs it)");
        ESP_LOGW(TAG, "  - sd_card (export/load)");
        ESP_LOGW(TAG, "  - console (debug)");
        ESP_LOGW(TAG, "ALL other tasks disabled:");
        ESP_LOGW(TAG, "  - LED/IO expander");
        ESP_LOGW(TAG, "  - WiFi/Ethernet");
        ESP_LOGW(TAG, "  - MQTT/Modbus");
        ESP_LOGW(TAG, "============================================");
    }

    /* Peripheral / extension modules: init failure is logged, marked ERROR, and
     * boot continues. Each module keeps its own init ownership; we only feed the
     * result to boot_manager for logging + on-screen status, and to the status
     * registry so consumers see a READY/ERROR baseline after boot. Each start is
     * called exactly once; its result is reused for both.
     *
     * In engineering mode: ONLY energy_meter + sd_card + console run.
     * All other tasks (LED, WiFi, Ethernet, Modbus, MQTT) are skipped. */
    esp_err_t r;

    /* IO Expander (LED blink) — skip in engineering mode */
    if (!engineering_mode) {
        r = io_expander_start();
        boot_manager_step("Digital IO", r);
        system_status_set(SYS_MODULE_DIGITAL_INPUT, boot_state(r));
        system_status_set(SYS_MODULE_DIGITAL_OUTPUT, boot_state(r));
    } else {
        ESP_LOGI(TAG, "IO Expander skipped (engineering mode)");
        system_status_set(SYS_MODULE_DIGITAL_INPUT, SYS_STATUS_OFFLINE);
        system_status_set(SYS_MODULE_DIGITAL_OUTPUT, SYS_STATUS_OFFLINE);
    }

    r = sd_card_manager_start();
    boot_manager_step("SD Card", r);
    system_status_set(SYS_MODULE_SD_CARD, boot_state(r));

    /* Certificate store on internal flash (/flash). Must be mounted before the
     * MQTT manager starts, otherwise a TLS profile whose ca_path points there
     * cannot load its PEM. Non-fatal: a mount failure only costs TLS. */
    boot_manager_step("Cert Store", cert_store_init());

    /* Energy meter — ALWAYS run (calibration needs it) */
    r = energy_meter_task_start();
    boot_manager_step("ATM90E32", r);
    system_status_set(SYS_MODULE_ATM90,
                      r == ESP_OK ? SYS_STATUS_INIT : SYS_STATUS_ERROR);

    /* Modbus slave — skip in engineering mode */
    if (!engineering_mode) {
        r = modbus_slave_task_start();
        boot_manager_step("RS485 Slave", r);
        system_status_set(SYS_MODULE_RS485_SLAVE, boot_state(r));
    } else {
        ESP_LOGI(TAG, "Modbus Slave skipped (engineering mode)");
        system_status_set(SYS_MODULE_RS485_SLAVE, SYS_STATUS_OFFLINE);
    }

    /* Modbus master — skip in engineering mode */
    if (!engineering_mode) {
        r = modbus_master_task_start();
        boot_manager_step("RS485 Master", r);
        if (r != ESP_OK) {
            system_status_set(SYS_MODULE_RS485_MASTER, SYS_STATUS_ERROR);
        }
    } else {
        ESP_LOGI(TAG, "Modbus Master skipped (engineering mode)");
        system_status_set(SYS_MODULE_RS485_MASTER, SYS_STATUS_OFFLINE);
    }

#if CONFIG_APP_MB_TCP_ENABLE
    /* Modbus TCP (EVN profile) — skip in engineering mode. Binds INADDR_ANY, so
     * it survives the ETH/STA failover without knowing which netif is live. */
    if (!engineering_mode) {
        boot_manager_step("Modbus TCP", modbus_tcp_task_start());
    } else {
        ESP_LOGI(TAG, "Modbus TCP skipped (engineering mode)");
    }
#endif

#if CONFIG_APP_CONSOLE_ENABLE
    boot_manager_step("Console", console_task_start());
#endif

    /* WiFi — skip in engineering mode */
    if (!engineering_mode) {
        r = wifi_manager_start();
        boot_manager_step("WiFi", r);
        system_status_set(SYS_MODULE_WIFI,
                          r == ESP_OK ? SYS_STATUS_INIT : SYS_STATUS_ERROR);
    } else {
        ESP_LOGI(TAG, "WiFi skipped (engineering mode)");
        system_status_set(SYS_MODULE_WIFI, SYS_STATUS_OFFLINE);
    }

    /* Ethernet — skip in engineering mode */
    if (!engineering_mode) {
        r = network_comm_task_start();
        boot_manager_step("Ethernet", r);
        system_status_set(SYS_MODULE_ETHERNET,
                          r == ESP_OK ? SYS_STATUS_INIT : SYS_STATUS_ERROR);
    } else {
        ESP_LOGI(TAG, "Ethernet skipped (engineering mode)");
        system_status_set(SYS_MODULE_ETHERNET, SYS_STATUS_OFFLINE);
    }

    /* Network manager — skip in engineering mode */
    if (!engineering_mode) {
        boot_manager_step("Network", network_manager_start());
    } else {
        ESP_LOGI(TAG, "Network Manager skipped (engineering mode)");
    }

#if CONFIG_APP_MQTT_ENABLE
    /* MQTT — skip in engineering mode */
    if (!engineering_mode) {
        r = mqtt_manager_start();
        boot_manager_step("MQTT", r);
        system_status_set(SYS_MODULE_MQTT,
                          r == ESP_OK ? SYS_STATUS_INIT : SYS_STATUS_ERROR);
    } else {
        ESP_LOGI(TAG, "MQTT skipped (engineering mode)");
        system_status_set(SYS_MODULE_MQTT, SYS_STATUS_OFFLINE);
    }
#else
    system_status_set(SYS_MODULE_MQTT, SYS_STATUS_OFFLINE);
#endif

    boot_manager_end();

#if CONFIG_APP_STATUS_DEBUG
    /* DEBUG ONLY: dump the full status table once after boot. Removed with the
     * rest of the Feature 04 debug scaffolding once PASS is confirmed. */
    system_status_dump();
#endif

    /* Exactly one HMI screen owns the LCD after boot: the Home Screen on a
     * normal boot, or the engineering test menu when the key combo is held.
     * Screen-task creation failure is a task-create fault (core), so it aborts. */
    if (engineering_mode) {
        ESP_LOGW(TAG, "Engineering mode: starting HMI test menu");
        ESP_RETURN_ON_ERROR(hmi_test_task_start(), TAG, "start HMI test task failed");
    } else {
        ESP_RETURN_ON_ERROR(home_screen_start(), TAG, "start home screen failed");
    }

    return ESP_OK;
}

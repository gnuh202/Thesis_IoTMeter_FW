#include "web_portal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_store.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "web_portal";

static httpd_handle_t s_httpd;
static bool s_dns_running;
static TaskHandle_t s_dns_task;
static int s_dns_sock = -1;
static char s_session_token[17];

#define DNS_PORT 53
#define DNS_MAX_PACKET 512
#define DNS_QR_RESPONSE 0x8000
#define DNS_A_RECORD 1
#define DNS_IN_CLASS 1

static void captive_dns_task(void *arg)
{
    (void)arg;
    uint8_t rx[DNS_MAX_PACKET];
    uint8_t tx[DNS_MAX_PACKET];
    const uint32_t ap_ip = inet_addr("192.168.4.1");

    while (1) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(s_dns_sock, rx, sizeof(rx), 0, (struct sockaddr *)&from, &from_len);
        if (len < 12) {
            continue;
        }

        int q_end = 12;
        while (q_end < len && rx[q_end] != 0) {
            q_end += rx[q_end] + 1;
        }
        if (q_end + 5 > len) {
            continue;
        }

        memcpy(tx, rx, q_end + 5);
        tx[2] = 0x81; /* response + recursion desired */
        tx[3] = 0x80; /* recursion available, no error */
        tx[6] = 0x00;
        tx[7] = 0x01; /* one answer */
        tx[8] = 0x00;
        tx[9] = 0x00;
        tx[10] = 0x00;
        tx[11] = 0x00;

        int pos = q_end + 5;
        if (pos + 16 > (int)sizeof(tx)) {
            continue;
        }
        tx[pos++] = 0xC0;
        tx[pos++] = 0x0C; /* pointer to queried name */
        tx[pos++] = 0x00;
        tx[pos++] = DNS_A_RECORD;
        tx[pos++] = 0x00;
        tx[pos++] = DNS_IN_CLASS;
        tx[pos++] = 0x00;
        tx[pos++] = 0x00;
        tx[pos++] = 0x00;
        tx[pos++] = 0x3C; /* TTL 60s */
        tx[pos++] = 0x00;
        tx[pos++] = 0x04;
        memcpy(&tx[pos], &ap_ip, 4);
        pos += 4;

        sendto(s_dns_sock, tx, pos, 0, (struct sockaddr *)&from, from_len);
    }
}

static esp_err_t captive_dns_start(void)
{
    if (s_dns_running) {
        return ESP_OK;
    }

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        return ESP_FAIL;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_dns_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_FAIL;
    }

    if (xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 5, &s_dns_task) != pdPASS) {
        close(s_dns_sock);
        s_dns_sock = -1;
        return ESP_ERR_NO_MEM;
    }

    s_dns_running = true;
    return ESP_OK;
}

static void captive_dns_stop(void)
{
    if (!s_dns_running) {
        return;
    }
    s_dns_running = false;
    if (s_dns_sock >= 0) {
        shutdown(s_dns_sock, SHUT_RDWR);
        close(s_dns_sock);
        s_dns_sock = -1;
    }
    if (s_dns_task != NULL) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }
}

#ifndef CONFIG_APP_CONSOLE_AUTH_ENABLE
#define CONFIG_APP_CONSOLE_AUTH_ENABLE 0
#endif
#ifndef CONFIG_APP_CONSOLE_AUTH_USERNAME
#define CONFIG_APP_CONSOLE_AUTH_USERNAME ""
#endif
#ifndef CONFIG_APP_CONSOLE_AUTH_PASSWORD
#define CONFIG_APP_CONSOLE_AUTH_PASSWORD ""
#endif

static const char *HTML_STYLE =
    ":root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#172033;background:#eef3f8}"
    "body{margin:0;min-height:100vh;display:grid;place-items:center;padding:24px}"
    ".card{width:min(820px,100%);background:#fff;border-radius:18px;box-shadow:0 18px 50px #13233a22;padding:28px}"
    ".badge{display:inline-block;background:#e7f7ef;color:#087443;border-radius:999px;padding:6px 12px;font-weight:700;font-size:13px}"
    "h1{margin:16px 0 8px;font-size:30px}p{line-height:1.55;color:#4d5b70}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin-top:22px}"
    ".box{border:1px solid #dce5ef;border-radius:14px;padding:14px;background:#f8fbfe}.box b{display:block;margin-bottom:6px}"
    "code{background:#eef3f8;border-radius:8px;padding:2px 6px}"
    ".nav{display:flex;gap:8px;flex-wrap:wrap;margin:18px 0}.nav a{color:#2563eb;text-decoration:none;background:#eff6ff;border-radius:999px;padding:8px 12px;font-weight:700}"
    ".section{margin-top:22px;padding-top:18px;border-top:1px solid #e5edf5}.section h2{margin:0 0 10px}.muted{color:#667085;font-size:14px}.pill{display:inline-block;border-radius:999px;background:#f1f5f9;padding:4px 9px;font-size:13px;margin-left:6px}"
    "textarea.input{min-height:120px;resize:vertical}.row{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.profile{border:1px solid #dce5ef;border-radius:14px;padding:14px;margin:12px 0;background:#fbfdff}"
    "label{display:block;margin:14px 0 6px;font-weight:700}.input{width:100%;box-sizing:border-box;border:1px solid #cfd9e5;border-radius:12px;padding:12px;font:inherit}"
    ".btn{border:0;border-radius:12px;background:#2563eb;color:white;font-weight:800;padding:12px 18px;margin-top:18px;cursor:pointer}.btn:hover{background:#1d4ed8}"
    ".err{background:#fff1f2;color:#be123c;border:1px solid #fecdd3;border-radius:12px;padding:10px 12px;margin:12px 0}";

static void send_redirect(httpd_req_t *req, const char *location)
{
    static const char login_page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"0;url=/login\"></head>"
        "<body><a href=\"/login\">Open login</a></body></html>";
    static const char root_page[] =
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"refresh\" content=\"0;url=/\"></head>"
        "<body><a href=\"/\">Open config portal</a></body></html>";

    httpd_resp_set_type(req, "text/html");
    if (location != NULL && strcmp(location, "/login") == 0) {
        httpd_resp_send(req, login_page, sizeof(login_page) - 1);
    } else {
        httpd_resp_send(req, root_page, sizeof(root_page) - 1);
    }
}

static bool request_has_valid_session(httpd_req_t *req)
{
#if CONFIG_APP_CONSOLE_AUTH_ENABLE
    if (s_session_token[0] == '\0') {
        return false;
    }

    char cookie[160];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, sizeof(cookie)) != ESP_OK) {
        return false;
    }

    const char *needle = "wp_session=";
    const char *p = strstr(cookie, needle);
    if (p == NULL) {
        return false;
    }
    p += strlen(needle);
    return strncmp(p, s_session_token, strlen(s_session_token)) == 0;
#else
    return true;
#endif
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(char *s)
{
    char *w = s;
    for (char *r = s; *r != '\0'; r++) {
        if (*r == '+') {
            *w++ = ' ';
        } else if (*r == '%' && hexval(r[1]) >= 0 && hexval(r[2]) >= 0) {
            *w++ = (char)((hexval(r[1]) << 4) | hexval(r[2]));
            r += 2;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static bool form_get_value(char *body, const char *key, char *out, size_t out_len)
{
    size_t key_len = strlen(key);
    char *p = body;
    while (p != NULL && *p != '\0') {
        char *next = strchr(p, '&');
        if (next != NULL) {
            *next = '\0';
        }
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            strlcpy(out, p + key_len + 1, out_len);
            url_decode(out);
            if (next != NULL) {
                *next = '&';
            }
            return true;
        }
        if (next == NULL) {
            break;
        }
        *next = '&';
        p = next + 1;
    }
    return false;
}

static char *read_form_body(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 8192) {
        return NULL;
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        return NULL;
    }

    int total = 0;
    while (total < req->content_len) {
        int got = httpd_req_recv(req, body + total, req->content_len - total);
        if (got <= 0) {
            free(body);
            return NULL;
        }
        total += got;
    }
    body[total] = '\0';
    return body;
}

static bool form_get_u32(char *body, const char *key, uint32_t *out)
{
    char tmp[32];
    if (!form_get_value(body, key, tmp, sizeof(tmp)) || tmp[0] == '\0') {
        return false;
    }
    *out = (uint32_t)strtoul(tmp, NULL, 10);
    return true;
}

static esp_err_t send_page_start(httpd_req_t *req, const char *title)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, "<!doctype html><html lang=\"vi\"><head><meta charset=\"utf-8\">"
                                  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
    httpd_resp_sendstr_chunk(req, "<title>");
    httpd_resp_sendstr_chunk(req, title);
    httpd_resp_sendstr_chunk(req, "</title><style>");
    httpd_resp_sendstr_chunk(req, HTML_STYLE);
    return httpd_resp_sendstr_chunk(req, "</style></head><body><main class=\"card\">");
}

static esp_err_t send_page_end(httpd_req_t *req)
{
    httpd_resp_sendstr_chunk(req, "</main></body></html>");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t send_saved_page(httpd_req_t *req, const char *title, const char *message)
{
    send_page_start(req, title);
    httpd_resp_sendstr_chunk(req, "<span class=\"badge\">Saved</span><h1>");
    httpd_resp_sendstr_chunk(req, title);
    httpd_resp_sendstr_chunk(req, "</h1><p>");
    httpd_resp_sendstr_chunk(req, message);
    httpd_resp_sendstr_chunk(req, "</p><p class=\"muted\">Cấu hình đã lưu vào NVS. Reboot để áp dụng.</p><p><a class=\"btn\" href=\"/\">Back to config</a></p>");
    return send_page_end(req);
}

static esp_err_t require_auth_or_redirect(httpd_req_t *req)
{
    if (!request_has_valid_session(req)) {
        send_redirect(req, "/login");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static const char *network_mode_str(config_network_mode_t mode)
{
    switch (mode) {
    case CONFIG_NETWORK_MODE_AUTO: return "AUTO (ETH -> WiFi -> AP recovery)";
    case CONFIG_NETWORK_MODE_ETH_ONLY: return "ETH_ONLY";
    case CONFIG_NETWORK_MODE_WIFI_ONLY: return "WIFI_ONLY";
    default: return "UNKNOWN";
    }
}

static const char *yes_no(bool v)
{
    return v ? "yes" : "no";
}

static esp_err_t send_escaped(httpd_req_t *req, const char *s)
{
    for (const char *p = s; p != NULL && *p != '\0'; p++) {
        switch (*p) {
        case '&': httpd_resp_sendstr_chunk(req, "&amp;"); break;
        case '<': httpd_resp_sendstr_chunk(req, "&lt;"); break;
        case '>': httpd_resp_sendstr_chunk(req, "&gt;"); break;
        case '"': httpd_resp_sendstr_chunk(req, "&quot;"); break;
        default: {
            char c[2] = { *p, '\0' };
            httpd_resp_sendstr_chunk(req, c);
            break;
        }
        }
    }
    return ESP_OK;
}

static esp_err_t send_input(httpd_req_t *req, const char *label, const char *name, const char *value)
{
    httpd_resp_sendstr_chunk(req, "<label>");
    httpd_resp_sendstr_chunk(req, label);
    httpd_resp_sendstr_chunk(req, "</label><input class=\"input\" name=\"");
    httpd_resp_sendstr_chunk(req, name);
    httpd_resp_sendstr_chunk(req, "\" value=\"");
    send_escaped(req, value != NULL ? value : "");
    return httpd_resp_sendstr_chunk(req, "\">");
}

static esp_err_t save_network_post_handler(httpd_req_t *req)
{
    if (require_auth_or_redirect(req) != ESP_OK) return ESP_OK;
    char *body = read_form_body(req);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }

    config_network_t net;
    config_store_get_network(&net);
    form_get_value(body, "wifi_ssid", net.wifi_ssid, sizeof(net.wifi_ssid));
    char pass[CONFIG_STORE_PASS_LEN] = "";
    if (form_get_value(body, "wifi_pass", pass, sizeof(pass)) && pass[0] != '\0') {
        strlcpy(net.wifi_pass, pass, sizeof(net.wifi_pass));
    }

    free(body);
    esp_err_t ret = config_store_set_network(&net);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save network failed");
        return ESP_OK;
    }
    return send_saved_page(req, "Network saved", "WiFi STA settings saved.");
}

static esp_err_t save_system_post_handler(httpd_req_t *req)
{
    if (require_auth_or_redirect(req) != ESP_OK) return ESP_OK;
    char *body = read_form_body(req);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }

    config_system_t sys;
    config_store_get_system(&sys);
    form_get_value(body, "device_name", sys.device_name, sizeof(sys.device_name));
    form_get_value(body, "hostname", sys.hostname, sizeof(sys.hostname));
    free(body);

    esp_err_t ret = config_store_set_system(&sys);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save system failed");
        return ESP_OK;
    }
    return send_saved_page(req, "System saved", "System identity settings saved.");
}

static esp_err_t save_mqtt_post_handler(httpd_req_t *req)
{
    if (require_auth_or_redirect(req) != ESP_OK) return ESP_OK;
    char *body = read_form_body(req);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }

    config_mqtt_t mqtt;
    config_store_get_mqtt(&mqtt);

    uint32_t v = 0;
    if (form_get_u32(body, "active", &v) && v < CONFIG_STORE_MQTT_PROFILE_COUNT) {
        mqtt.active = (uint8_t)v;
    }
    if (form_get_u32(body, "keepalive_s", &v) && v > 0 && v <= UINT16_MAX) {
        mqtt.keepalive_s = (uint16_t)v;
    }
    if (form_get_u32(body, "publish_period_ms", &v) && v >= 1000) {
        mqtt.publish_period_ms = v;
    }

    for (int i = 0; i < CONFIG_STORE_MQTT_PROFILE_COUNT; i++) {
        mqtt_profile_t *p = &mqtt.profiles[i];
        char key[24];
        char tmp[CONFIG_STORE_MQTT_CA_LEN];

        snprintf(key, sizeof(key), "p%d_name", i);
        form_get_value(body, key, p->name, sizeof(p->name));
        snprintf(key, sizeof(key), "p%d_uri", i);
        form_get_value(body, key, p->uri, sizeof(p->uri));
        snprintf(key, sizeof(key), "p%d_port", i);
        if (form_get_u32(body, key, &v) && v > 0 && v <= UINT16_MAX) {
            p->port = (uint16_t)v;
        }
        snprintf(key, sizeof(key), "p%d_user", i);
        form_get_value(body, key, p->username, sizeof(p->username));
        snprintf(key, sizeof(key), "p%d_pass", i);
        if (form_get_value(body, key, tmp, sizeof(p->password)) && tmp[0] != '\0') {
            strlcpy(p->password, tmp, sizeof(p->password));
        }
        snprintf(key, sizeof(key), "p%d_ca", i);
        if (form_get_value(body, key, tmp, sizeof(tmp)) && tmp[0] != '\0') {
            strlcpy(p->ca_cert, tmp, sizeof(p->ca_cert));
            p->use_custom_ca = true;
            p->tls_enable = true;
        }
    }

    free(body);
    esp_err_t ret = config_store_set_mqtt(&mqtt);
    if (ret != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save mqtt failed");
        return ESP_OK;
    }
    return send_saved_page(req, "MQTT saved", "MQTT profiles saved.");
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(900));
    ESP_LOGW(TAG, "rebooting to apply web portal config");
    esp_restart();
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (require_auth_or_redirect(req) != ESP_OK) return ESP_OK;

    send_page_start(req, "Rebooting");
    httpd_resp_sendstr_chunk(req,
        "<span class=\"badge\">Apply</span>"
        "<h1>Rebooting...</h1>"
        "<p>Thiết bị sẽ khởi động lại để áp dụng cấu hình mới. Sau vài giây, kết nối lại AP hoặc mạng cấu hình mới.</p>");
    send_page_end(req);

    BaseType_t ok = xTaskCreate(reboot_task, "web_reboot", 2048, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create reboot task failed");
    }
    return ESP_OK;
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    send_redirect(req, "/");
    return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!request_has_valid_session(req)) {
        send_redirect(req, "/login");
        return ESP_OK;
    }

    config_network_t net;
    config_mqtt_t mqtt;
    config_system_t sys;
    config_store_get_network(&net);
    config_store_get_mqtt(&mqtt);
    config_store_get_system(&sys);

    char tmp[96];
    send_page_start(req, "IoT Meter Config");
    httpd_resp_sendstr_chunk(req,
        "<span class=\"badge\">Config Portal Online</span>"
        "<h1>ESP32-S3 IoT Meter</h1>"
        "<p>Chỉnh cấu hình qua SoftAP. Secret đã lưu chỉ hiển thị trạng thái set/empty; nhập giá trị mới khi muốn đổi.</p>"
        "<div class=\"nav\"><a href=\"#network\">Network</a><a href=\"#mqtt\">MQTT</a><a href=\"#system\">System</a><a href=\"#apply\">Apply</a></div>");

    httpd_resp_sendstr_chunk(req, "<section class=\"section\" id=\"network\"><h2>Network</h2><form method=\"post\" action=\"/save/network\">");
    httpd_resp_sendstr_chunk(req, "<p class=\"muted\">Mode hiện tại: <b>");
    httpd_resp_sendstr_chunk(req, network_mode_str(net.mode));
    httpd_resp_sendstr_chunk(req, "</b>. Static IP sửa sau; giai đoạn này ưu tiên WiFi STA + AP portal.</p><div class=\"row\">");
    send_input(req, "WiFi STA SSID", "wifi_ssid", net.wifi_ssid);
    snprintf(tmp, sizeof(tmp), "%s", strlen(net.wifi_pass) ? "(đã đặt - nhập mới để đổi)" : "(empty)");
    send_input(req, "WiFi STA password", "wifi_pass", "");
    httpd_resp_sendstr_chunk(req, "<div><label>Password hiện tại</label><div class=\"box\">");
    send_escaped(req, tmp);
    httpd_resp_sendstr_chunk(req, "</div></div>");
    httpd_resp_sendstr_chunk(req, "</div><button class=\"btn\" type=\"submit\">Save Network</button></form></section>");

    httpd_resp_sendstr_chunk(req, "<section class=\"section\" id=\"mqtt\"><h2>MQTT</h2><form method=\"post\" action=\"/save/mqtt\">");
    snprintf(tmp, sizeof(tmp), "%u", (unsigned)mqtt.active);
    send_input(req, "Active profile (0..2)", "active", tmp);
    snprintf(tmp, sizeof(tmp), "%u", (unsigned)mqtt.keepalive_s);
    send_input(req, "Keepalive (s)", "keepalive_s", tmp);
    snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)mqtt.publish_period_ms);
    send_input(req, "Publish period (ms)", "publish_period_ms", tmp);
    httpd_resp_sendstr_chunk(req, "<p class=\"muted\">MQTT enabled: <b>");
    httpd_resp_sendstr_chunk(req, yes_no(mqtt.enabled));
    httpd_resp_sendstr_chunk(req, "</b></p>");

    for (int i = 0; i < CONFIG_STORE_MQTT_PROFILE_COUNT; i++) {
        const mqtt_profile_t *p = &mqtt.profiles[i];
        snprintf(tmp, sizeof(tmp), "<div class=\"profile\"><h3>Profile %d%s</h3><div class=\"row\">", i, i == mqtt.active ? " <span class=\"pill\">active</span>" : "");
        httpd_resp_sendstr_chunk(req, tmp);
        char name_key[24], uri_key[24], port_key[24], user_key[24], pass_key[24];
        snprintf(name_key, sizeof(name_key), "p%d_name", i);
        snprintf(uri_key, sizeof(uri_key), "p%d_uri", i);
        snprintf(port_key, sizeof(port_key), "p%d_port", i);
        snprintf(user_key, sizeof(user_key), "p%d_user", i);
        snprintf(pass_key, sizeof(pass_key), "p%d_pass", i);
        send_input(req, "Name", name_key, p->name);
        send_input(req, "URI/host", uri_key, p->uri);
        snprintf(tmp, sizeof(tmp), "%u", (unsigned)p->port);
        send_input(req, "Port", port_key, tmp);
        send_input(req, "Username", user_key, p->username);
        send_input(req, "Password mới (để trống = giữ nguyên)", pass_key, "");
        httpd_resp_sendstr_chunk(req, "</div><p class=\"muted\">TLS: <b>");
        httpd_resp_sendstr_chunk(req, yes_no(p->tls_enable));
        httpd_resp_sendstr_chunk(req, "</b> · Custom CA: <b>");
        httpd_resp_sendstr_chunk(req, yes_no(p->use_custom_ca));
        httpd_resp_sendstr_chunk(req, "</b> · CA cert: <b>");
        httpd_resp_sendstr_chunk(req, strlen(p->ca_cert) ? "set" : "empty");
        httpd_resp_sendstr_chunk(req, "</b></p><label>CA cert mới (textarea, để trống = giữ nguyên)</label><textarea class=\"input\" name=\"");
        char ca_key[24];
        snprintf(ca_key, sizeof(ca_key), "p%d_ca", i);
        httpd_resp_sendstr_chunk(req, ca_key);
        httpd_resp_sendstr_chunk(req, "\"></textarea></div>");
    }
    httpd_resp_sendstr_chunk(req, "<button class=\"btn\" type=\"submit\">Save MQTT</button></form></section>");

    httpd_resp_sendstr_chunk(req, "<section class=\"section\" id=\"system\"><h2>System</h2><form method=\"post\" action=\"/save/system\"><div class=\"row\">");
    send_input(req, "Device name", "device_name", sys.device_name);
    send_input(req, "Hostname", "hostname", sys.hostname);
    httpd_resp_sendstr_chunk(req, "</div><button class=\"btn\" type=\"submit\">Save System</button></form></section>");

    httpd_resp_sendstr_chunk(req, "<section class=\"section\" id=\"apply\"><h2>Apply</h2><p class=\"muted\">Sau khi save, reboot để cấu hình mới có hiệu lực.</p><form method=\"post\" action=\"/reboot\"><button class=\"btn\" type=\"submit\">Reboot to apply</button></form></section>");
    return send_page_end(req);
}

static esp_err_t login_get_handler(httpd_req_t *req)
{
    send_page_start(req, "Login - IoT Meter");
    httpd_resp_sendstr_chunk(req,
        "<span class=\"badge\">Config Portal</span>"
        "<h1>Đăng nhập</h1>"
        "<p>Dùng chung tài khoản console để vào Web Config Portal.</p>"
        "<form method=\"post\" action=\"/login\">"
        "<label>Username</label><input class=\"input\" name=\"user\" autocomplete=\"username\">"
        "<label>Password</label><input class=\"input\" name=\"pass\" type=\"password\" autocomplete=\"current-password\">"
        "<button class=\"btn\" type=\"submit\">Login</button>"
        "</form>");
    return send_page_end(req);
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
#if !CONFIG_APP_CONSOLE_AUTH_ENABLE
    ESP_LOGW(TAG, "console auth disabled; web login bypassed");
    send_redirect(req, "/");
    return ESP_OK;
#else
    if (req->content_len <= 0 || req->content_len >= 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        return ESP_OK;
    }

    char body[256];
    int got = httpd_req_recv(req, body, req->content_len);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_OK;
    }
    body[got] = '\0';

    char user[64] = "";
    char pass[96] = "";
    form_get_value(body, "user", user, sizeof(user));
    form_get_value(body, "pass", pass, sizeof(pass));

    if (strcmp(user, CONFIG_APP_CONSOLE_AUTH_USERNAME) == 0 &&
        strcmp(pass, CONFIG_APP_CONSOLE_AUTH_PASSWORD) == 0) {
        snprintf(s_session_token, sizeof(s_session_token), "%08" PRIx32 "%08" PRIx32,
                 esp_random(), esp_random());
        char cookie[96];
        snprintf(cookie, sizeof(cookie), "wp_session=%s; Path=/; HttpOnly; SameSite=Strict", s_session_token);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie);
        ESP_LOGI(TAG, "web login ok");
        send_redirect(req, "/");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "web login failed for user '%s'", user);
    send_page_start(req, "Login failed - IoT Meter");
    httpd_resp_sendstr_chunk(req,
        "<span class=\"badge\">Config Portal</span>"
        "<h1>Đăng nhập</h1>"
        "<div class=\"err\">Sai username hoặc password.</div>"
        "<form method=\"post\" action=\"/login\">"
        "<label>Username</label><input class=\"input\" name=\"user\" autocomplete=\"username\">"
        "<label>Password</label><input class=\"input\" name=\"pass\" type=\"password\" autocomplete=\"current-password\">"
        "<button class=\"btn\" type=\"submit\">Login</button>"
        "</form>");
    return send_page_end(req);
#endif
}

static esp_err_t register_handlers(void)
{
    const httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &root), TAG, "register / failed");

    const char *probe_uris[] = {
        "/generate_204",
        "/gen_204",
        "/hotspot-detect.html",
        "/library/test/success.html",
        "/ncsi.txt",
        "/connecttest.txt",
        "/fwlink",
    };
    for (size_t i = 0; i < sizeof(probe_uris) / sizeof(probe_uris[0]); i++) {
        const httpd_uri_t probe = {
            .uri = probe_uris[i],
            .method = HTTP_GET,
            .handler = captive_redirect_handler,
            .user_ctx = NULL,
        };
        esp_err_t ret = httpd_register_uri_handler(s_httpd, &probe);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "register captive URI %s failed: %s", probe_uris[i], esp_err_to_name(ret));
        }
    }

    const httpd_uri_t login_get = {
        .uri = "/login",
        .method = HTTP_GET,
        .handler = login_get_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &login_get), TAG, "register GET /login failed");

    const httpd_uri_t login_post = {
        .uri = "/login",
        .method = HTTP_POST,
        .handler = login_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &login_post), TAG, "register POST /login failed");

    const httpd_uri_t save_network = {
        .uri = "/save/network",
        .method = HTTP_POST,
        .handler = save_network_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &save_network), TAG, "register POST /save/network failed");

    const httpd_uri_t save_mqtt = {
        .uri = "/save/mqtt",
        .method = HTTP_POST,
        .handler = save_mqtt_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &save_mqtt), TAG, "register POST /save/mqtt failed");

    const httpd_uri_t save_system = {
        .uri = "/save/system",
        .method = HTTP_POST,
        .handler = save_system_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &save_system), TAG, "register POST /save/system failed");

    const httpd_uri_t reboot = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = reboot_post_handler,
        .user_ctx = NULL,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &reboot), TAG, "register POST /reboot failed");

    return ESP_OK;
}

esp_err_t web_portal_start(void)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }

    s_session_token[0] = '\0';

    esp_err_t dns_ret = captive_dns_start();
    if (dns_ret == ESP_OK) {
        ESP_LOGI(TAG, "captive DNS server started");
    } else {
        ESP_LOGW(TAG, "captive DNS server start failed: %s", esp_err_to_name(dns_ret));
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 12288;
    cfg.max_uri_handlers = 16;

    esp_err_t ret = httpd_start(&s_httpd, &cfg);
    if (ret != ESP_OK) {
        if (s_dns_running) {
            captive_dns_stop();
            s_dns_running = false;
        }
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = register_handlers();
    if (ret != ESP_OK) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
        if (s_dns_running) {
            captive_dns_stop();
            s_dns_running = false;
        }
        return ret;
    }

    ESP_LOGI(TAG, "web portal started on SoftAP (http://192.168.4.1/)");
    return ESP_OK;
}

esp_err_t web_portal_stop(void)
{
    if (s_httpd == NULL) {
        return ESP_OK;
    }

    esp_err_t ret = httpd_stop(s_httpd);
    s_httpd = NULL;
    s_session_token[0] = '\0';
    if (s_dns_running) {
        captive_dns_stop();
        ESP_LOGI(TAG, "captive DNS server stopped");
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "web portal stopped");
    }
    return ret;
}

bool web_portal_is_running(void)
{
    return s_httpd != NULL;
}

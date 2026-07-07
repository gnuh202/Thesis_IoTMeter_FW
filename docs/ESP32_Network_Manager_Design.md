# Thiết kế Network Manager + Configuration Portal (ESP32-S3, ESP-IDF)

> Trạng thái: **ĐÃ TRIỂN KHAI** (trừ các mục `[TODO-SAU]`). Đây là tài liệu thiết kế gốc; phần data-path (ETH/WiFi failover, gom init, AP on-demand, auto-AP recovery) đã code xong và chạy trên thiết bị thật. Các mục đánh dấu `[QUYẾT ĐỊNH]` là chốt chọn, `[TODO-SAU]` là **chưa làm**.
>
> **Đã implement:** NetworkManager state machine + failover ETH>STA (option B: STA chỉ bật khi ETH mất), gom init hạ tầng (NVS/netif/event loop) trong `network_manager_infra_init()`, AP on-demand (boot STA-only), auto-AP recovery (mất hết mạng → tự bật AP → grace 60s khi mạng về). Cấu hình qua console `net-cfg` (xem [console_commands.md](console_commands.md)).
>
> **Chưa làm (`[TODO-SAU]`):** Web Configuration Portal (trang web nhập cấu hình khi vào AP), Modbus TCP server, LCD2004 + 5 nút, static IP (hiện chỉ DHCP). Cấu hình mạng hiện nhập qua console, chưa có web form.
>
> Kiến trúc tổng thể & vòng đời khởi động: xem [architecture.md](architecture.md).

## 1. Mục tiêu

Module quản lý mạng cho đồng hồ đo điện năng công nghiệp ESP32-S3, ưu tiên **ổn định** và **không gián đoạn dịch vụ đo**.

Phần cứng/dịch vụ liên quan:
- Ethernet W5500 (SPI) — RJ45
- WiFi STA (nối router)
- WiFi SoftAP (chỉ để cấu hình / fallback)
- MQTT Client (thiết kế trong đợt này, code sau)
- Modbus RTU Server (RS485/UART — **đã có**, giữ nguyên)
- Modbus TCP Server — `[TODO-SAU]`, chỉ define khung
- LCD2004 (I2C) + 5 nút — `[TODO-SAU]`, define khung API
- SD card, NVS

## 2. Nguyên tắc kiến trúc (quan trọng — khác bản nháp cũ)

1. **Một interface data-path active tại một thời điểm**: ưu tiên `Ethernet > WiFi STA`. Nhưng **SoftAP là netif riêng, chạy song song được** với ETH hoặc STA — AP không tính là "interface data-path", nó chỉ phục vụ cấu hình.

2. **Init hạ tầng mạng tập trung một chỗ.** Hiện tại `network_comm_task.c` và `wifi_meter_server.c` **mỗi cái tự gọi** `nvs_flash_init` + `esp_netif_init` + `esp_event_loop_create_default`. Đây là nợ kỹ thuật. NetworkManager sẽ **sở hữu** các init này; module con không tự gọi nữa.

3. **Chuyển interface = mọi TCP socket chết.** Khi đổi ETH↔STA phải: đặt lại default netif (`esp_netif_set_default_netif`), rồi **reconnect MQTT** và **restart Modbus TCP listener**. Không có chuyện socket tự sống qua lần đổi interface.

4. **Dịch vụ đo (ATM90 + Modbus RTU) độc lập hoàn toàn với mạng.** Mạng sập, đang cấu hình, hay đổi interface — task đo và Modbus RTU vẫn chạy. Đây là ràng buộc bắt buộc.

5. **Event-driven, non-blocking.** Không busy-wait. Trạng thái đổi theo event (link up/down, got IP, disconnected) và timeout, không polling link đồng bộ.

## 3. Phân biệt 2 thứ hay bị gộp nhầm

| | Live Dashboard | Configuration Portal |
|---|---|---|
| Mục đích | Xem số đo realtime | Nhập cấu hình mạng/MQTT |
| Endpoint | `/`, `/measurements` (JSON) | `/config`, `/save` (form) |
| Khi nào chạy | Trên interface data-path active (ETH/STA) **và** khi AP bật | Chỉ khi vào Config Portal |
| Kích hoạt | Luôn chạy khi có IP (LAN hoặc AP) | LCD menu, hoặc auto-AP khi mất mạng |

`[QUYẾT ĐỊNH]` **AP on-demand**: bình thường AP **tắt**. HTTP server (dashboard) chạy trên interface data-path đang active (ETH/STA) — truy cập dashboard qua IP LAN. Khi mở Config Portal thì bật thêm AP, dashboard cũng phục vụ trên `192.168.4.1`. Telemetry chính đi qua ETH/STA (MQTT / HTTP upload).

## 4. Chế độ mạng (NetworkMode trong NVS)

```
AUTO       : ưu tiên ETH, rớt ETH thì STA, rớt cả hai thì auto-AP
ETH_ONLY   : chỉ Ethernet
WIFI_ONLY  : chỉ WiFi STA
```

## 5. Boot sequence (event-driven, có timeout)

```
1. NVS load config (NetworkConfig, MQTTConfig, SystemConfig).
2. esp_netif_init + event loop + đăng ký handler (một lần, tập trung).
3. Theo NetworkMode:
   - AUTO / ETH_ONLY:
       esp_eth_start()
       chờ ETHERNET_EVENT_CONNECTED tối đa ETH_LINK_TIMEOUT (vd 5s)
       - có link -> chờ IP_EVENT_ETH_GOT_IP (DHCP timeout vd 10s) -> ETH_ACTIVE
       - hết timeout link -> (AUTO: sang WiFi STA) / (ETH_ONLY: RETRY)
   - AUTO / WIFI_ONLY:
       nếu có SSID -> WIFI_CONNECTING -> chờ IP_EVENT_STA_GOT_IP
       - được IP -> WIFI_ACTIVE
       - fail sau N lần -> (AUTO/WIFI_ONLY: AP_MODE auto)
4. Khi có interface active + IP:
   - set default netif
   - start MQTT (nếu enabled)
   - Modbus TCP listener  [TODO-SAU]
5. Modbus RTU + đo lường: đã chạy từ trước, độc lập.
```

Lưu ý kỹ thuật W5500: **không đọc link đồng bộ lúc boot**. Phải `esp_eth_start()` rồi chờ event — bản nháp cũ ("Check Ethernet link" ở bước 2) là sai mô hình, đã sửa.

## 6. Runtime — chuyển interface

### 6.1. Cắm ETH khi đang WiFi STA (AUTO)
```
ETHERNET_EVENT_CONNECTED -> chờ IP_EVENT_ETH_GOT_IP
  -> MQTT disconnect
  -> esp_netif_set_default_netif(eth)
  -> esp_wifi disconnect STA (giữ driver, không stop hẳn nếu AP đang bật)
  -> MQTT reconnect qua ETH
  -> Modbus TCP rebind  [TODO-SAU]
  -> state = ETH_ACTIVE
```

### 6.2. Rút ETH
```
ETHERNET_EVENT_DISCONNECTED
  -> debounce ETH_DOWN_DEBOUNCE (vd 3s, tránh nhiễu/rung dây)
  -> nếu vẫn down và mode=AUTO:
       WIFI_CONNECTING -> got IP -> set default netif(sta)
       -> MQTT reconnect -> state = WIFI_ACTIVE
  -> nếu STA cũng fail sau N lần -> AP_MODE auto
```

### 6.3. Config Portal khi đang có ETH
`[QUYẾT ĐỊNH]` **Giữ ETH chạy song song, KHÔNG cắt telemetry.** AP bật thêm (SoftAP netif riêng), ETH vẫn active, MQTT/đo vẫn chạy. Người dùng cấu hình xong -> lưu NVS -> **restart NetworkManager** (không reboot cả thiết bị) -> áp cấu hình mới.

Lý do chọn: ESP32 chạy AP + ETH đồng thời được; cắt mạng lúc cấu hình gây gián đoạn đo không cần thiết. An toàn hơn cho thiết bị đang vận hành.

## 7. Config Portal (LCD + Web)

### LCD (chỉ điều hướng, KHÔNG nhập text — 5 nút không nhập được SSID/pass)
```
Settings
 └── Network
      └── Start Config Portal?  [YES/NO]
```
YES -> bật SoftAP + Web Portal. LCD hiện:
```
AP MODE
SSID: PowerMeter_xxxx
PASS: <ap_pass>
http://192.168.4.1
```
(SSID hiện tại đã thống nhất tiền tố **PowerMeter**, không phải EnergyMeter.)

### Web Portal — trang & trường
- **Status**: ETH / WiFi / MQTT status, IP, active interface.
- **Network**: NetworkMode, WiFi SSID/Pass, ETH DHCP|Static, Static IP/Gateway/DNS.
- **MQTT**: Broker, Port, User, Pass, ClientID, KeepAlive, QoS.
- **System**: Device Name, Hostname, Restart, Factory Reset.
- Nút **Save / Cancel**.

Save flow:
```
Validate -> lưu NVS -> tắt AP (nếu là on-demand) -> restart NetworkManager -> áp config mới
```

## 8. Auto-AP
Tự bật AP khi: (AUTO/WIFI_ONLY) WiFi fail sau N retry **và** ETH không có link. Hiện thông tin AP lên LCD. Khi ETH/STA phục hồi và mode cho phép -> tự tắt AP (nếu không có client đang cấu hình).

## 9. State machine (đầy đủ đường thoát)

```
        INIT
          |
      LOAD_CONFIG
          |
       CHECK_ETH ---- link up ----> ETH_GET_IP --got IP--> ETH_ACTIVE
          |                              |timeout             |
     link timeout                        v                    |
          |                          (AUTO->WiFi)             | ETH link down (debounce)
          v                                                   v
   WIFI_CONNECTING --got IP--> WIFI_ACTIVE <------------------+
          |  fail N lần                 |
          v                             | (mode cho phép, ETH về)
       AP_MODE (auto) <-----------------+
          |
   (Save config / ETH hoặc STA phục hồi)
          |
        RESTART_MANAGER --> INIT

CONFIG_PORTAL: nhánh song song, bật AP + web, không phá state data-path
               (đã chọn: coexist với ETH/STA).
ERROR/RETRY  : mọi state lỗi -> đếm retry -> RESTART_MANAGER hoặc AP_MODE.
```

## 10. Module & API (header/source tách bạch)

| Module | Trách nhiệm | Ghi chú |
|---|---|---|
| `network_manager` | State machine, sở hữu init hạ tầng, điều phối | **mới** |
| `ethernet_driver` | W5500 SPI, link/IP event | refactor từ `network_comm_task.c` |
| `wifi_manager` | STA + SoftAP, event | refactor từ `wifi_meter_server.c` |
| `mqtt_manager` | Kết nối, publish/subscribe, reconnect | thiết kế đợt này, code sau |
| `web_portal` | Config pages + Save | tách khỏi dashboard |
| `web_dashboard` | `/measurements` realtime | từ code AP hiện có |
| `config_store` | NVS load/save, magic+version | dùng lại pattern của calib |
| `lcd_menu`, `button_manager` | HMI | `[TODO-SAU]`, define API |
| `modbus_tcp_server` | Modbus TCP | `[TODO-SAU]`, define khung |

Ranh giới: driver phần cứng tách khỏi logic ứng dụng; mỗi module 1 header API rõ ràng, coupling tối thiểu.

## 11. NVS schema (magic + version, giống calib đang dùng)

```
namespace "netcfg", key "net_v1":
  magic, version
  network_mode (AUTO|ETH_ONLY|WIFI_ONLY)
  wifi_ssid, wifi_pass
  eth_dhcp (bool), static_ip, gateway, netmask, dns
  ap_ssid_suffix, ap_pass

namespace "mqttcfg", key "mqtt_v1":
  magic, version
  broker_uri, port, username, password, client_id, keepalive, qos

namespace "syscfg", key "sys_v1":
  magic, version
  device_name, hostname
```
Mỗi blob có magic+version, đọc sai -> dùng default (không fake). Đồng bộ cách làm calib.

## 12. MQTT (thiết kế đợt này, code sau)

**Publish**: số đo công suất, trạng thái ngõ vào/ra (PCF8574), sự kiện cảnh báo, sự kiện hệ thống, dữ liệu Modbus Master, heartbeat.
**Subscribe**: điều khiển ngõ ra, (tùy chọn) lệnh reboot.
**Ràng buộc**: cấu hình **không** được sửa qua MQTT (chỉ qua Web Portal). Reconnect tự động khi đổi interface hoặc mất kết nối.

## 13. Yêu cầu phi chức năng
- Non-blocking, event-driven, FreeRTOS.
- Tách driver / logic; header-source rõ ràng; API public có doc.
- Sẵn sàng mở rộng: OTA, SNTP, HTTPS, cloud.
- Debounce link, timeout, retry-count đều cấu hình được (Kconfig).

## 14. Việc cần làm khi triển khai (thứ tự đề xuất)
1. `config_store` (NVS schema) trước — nền cho mọi module.
2. `network_manager` + refactor `ethernet_driver` / `wifi_manager` (gom init tập trung).
3. Tách `web_dashboard` khỏi `web_portal`; AP thành on-demand.
4. `mqtt_manager`.
5. `[TODO-SAU]`: LCD/nút, Modbus TCP.

## 15. Quyết định đã chốt

1. `[QUYẾT ĐỊNH]` **Auto-AP timeout = 5 phút** không có client cấu hình thì tự tắt AP. Kconfig: `APP_NET_AP_IDLE_TIMEOUT_S` default 300.
2. `[QUYẾT ĐỊNH]` **Timeout/retry để trong Kconfig** (không hard-code):
   - `APP_NET_ETH_LINK_TIMEOUT_MS` default 5000
   - `APP_NET_ETH_DHCP_TIMEOUT_MS` default 10000
   - `APP_NET_STA_MAX_RETRY` default 5
   - `APP_NET_ETH_DOWN_DEBOUNCE_MS` default 3000
3. `[QUYẾT ĐỊNH]` **Giữ web dashboard trên IP LAN** khi chạy ETH/STA (không chỉ MQTT). HTTP server chạy trên interface data-path active; dashboard `/` + `/measurements` truy cập được qua IP LAN. AP chỉ thêm khi vào Config Portal.
4. `[QUYẾT ĐỊNH]` **AP có password, default `12345678`** (`APP_NET_AP_PASSWORD`). Đủ cản người ngoài, không phải bảo mật mạnh.

# Thiết kế MQTT Client (ESP32-S3, ESP-IDF)

> Trạng thái: **ĐANG TRIỂN KHAI**. `[QUYẾT ĐỊNH]` = đã chốt, `[TODO-SAU]` = pha sau.
>
> **Đã implement:** `config_mqtt_t` v2 (3 broker profiles trong NVS, 1 active), mqtt_manager kết nối theo profile active (TLS-capable qua cert bundle IDF), LWT online/offline, client_id tự sinh (device_name + MAC suffix), publish định kỳ `telemetry`/`energy`/`io`/`heartbeat` (cJSON), reconnect chủ động khi đổi interface, subscribe `cmd/out0`/`cmd/out1` điều khiển 2 relay (payload JSON `{"on":bool}`, validate chặt, echo lại `io`). Cấu hình qua console `mqtt-cfg` (xem [console_commands.md](console_commands.md)).
>
> **Chưa làm (`[TODO-SAU]`):** nhập CA cert custom qua console/portal, alarm topic, cmd/reboot, live-apply (đổi cấu hình hiện phải reboot).
>
> Kiến trúc tổng thể & vòng đời khởi động: xem [architecture.md](architecture.md).

## 1. Mục tiêu

MQTT client là kênh telemetry chính (thay cho dashboard đã bỏ) và là kênh điều khiển từ xa cho đồng hồ đo điện năng ESP32-S3.

- **Publish**: số đo, năng lượng, demand, trạng thái I/O, heartbeat.
- **Subscribe**: điều khiển 2 ngõ ra relay (out0/out1).
- Chạy trên interface data-path đang active (ETH hoặc WiFi STA), reconnect khi đổi interface.

## 2. Quyết định đã chốt

- `[QUYẾT ĐỊNH]` **Broker-independent** — không khóa vào một broker. Hỗ trợ tối đa **3 broker profiles**, chỉ **1 profile active** tại một thời điểm. Mặc định phát triển với **Mosquitto**, nhưng code không phụ thuộc broker cụ thể.
- `[QUYẾT ĐỊNH]` **TLS bật/tắt theo từng profile** (`mqtts://` cổng 8883 khi bật, `mqtt://` 1883 khi tắt). Mỗi profile chọn dùng **CA hệ thống** hay **CA tùy chỉnh** (self-signed Mosquitto).
- `[QUYẾT ĐỊNH]` **Chu kỳ publish cấu hình được** qua Kconfig (mặc định 5s), sau này chỉnh runtime.
- `[QUYẾT ĐỊNH]` **Device ID trong topic = Device Name**. MQTT Client ID **tự sinh** = DeviceName + hậu tố MAC (đảm bảo duy nhất trên broker).
- `[QUYẾT ĐỊNH]` **Có subscribe điều khiển 2 relay** out0/out1, payload **JSON**.
- `[QUYẾT ĐỊNH]` **QoS**: telemetry QoS0 (không retain); energy/io/alarm/command QoS1.

## 3. Ràng buộc quan trọng (đọc trước khi code)

1. **Điều khiển relay qua broker cloud = rủi ro an toàn thật.** Bất kỳ ai publish được vào topic control đều bật/tắt được relay vật lý. Bắt buộc:
   - TLS (chống nghe lén/sửa gói trên đường).
   - Username/password MQTT riêng cho thiết bị (đã có trong `config_mqtt_t`).
   - Broker phải cấu hình ACL: chỉ client được phép mới publish vào topic control của thiết bị này.
   - **Không** để topic control ở dạng đoán được + broker mở. Đây là trách nhiệm cấu hình broker, firmware chỉ làm đúng phần nó (TLS + auth + validate payload).

2. **Đo lường độc lập MQTT.** Broker chết / mất mạng / TLS lỗi → task đo + Modbus RTU vẫn chạy. MQTT chỉ là kênh phụ, không được kéo sập lõi đo.

3. **Config KHÔNG sửa qua MQTT.** Chỉ điều khiển relay + đọc telemetry. Đổi cấu hình mạng/MQTT chỉ qua console / (sau này) web portal. Giữ đúng nguyên tắc trong doc network.

4. **TLS tốn RAM/flash.** Mỗi kết nối TLS cần ~20-40KB RAM cho handshake + buffer. N16R2 có PSRAM (chưa bật) — nếu RAM căng, cân nhắc bật PSRAM. Cert bundle nằm trong flash (app partition đã nới 3MB, đủ chỗ).

## 4. Cấu hình — 3 broker profiles trong NVS

`config_mqtt_t` hiện tại ([config_store.h](../main/app/config_store.h)) chỉ đủ cho **1 profile** và **thiếu** field TLS/CA. Phải **mở rộng schema** (bump version, thêm migration về default nếu blob cũ):

```
typedef struct {
    char     name[32];        // nhãn profile ("Mosquitto local", "HiveMQ"...)
    char     uri[128];        // host, KHÔNG kèm scheme (vd "192.168.1.10")
    uint16_t port;            // 1883 / 8883
    char     username[33];
    char     password[65];
    bool     tls_enable;      // true -> mqtts, verify server
    bool     use_custom_ca;   // true -> dùng ca_cert dưới; false -> CA bundle hệ thống
    char     ca_cert[2048];   // PEM CA tùy chỉnh (self-signed Mosquitto), rỗng nếu không dùng
} mqtt_profile_t;

typedef struct {
    uint8_t        active;              // index profile đang dùng (0..2)
    mqtt_profile_t profiles[3];
    uint16_t       keepalive_s;         // dùng chung
    uint32_t       publish_period_ms;   // chu kỳ publish telemetry (NVS = nơi cấu hình chính)
    bool           enabled;             // bật/tắt MQTT client
} config_mqtt_t;                        // version bump: v1 -> v2
```

Ghi chú:
- **Client ID không lưu** — tự sinh runtime = `device_name + "-" + 3 byte cuối MAC` (vd `PowerMeter-3AF2C1`). Lấy device_name từ `config_system_t`.
- `ca_cert` 2KB/profile × 3 = 6KB trong blob NVS — chấp nhận được. Nếu cert lớn hơn, tăng sau.
- Đổi `config_mqtt_t` là **breaking change** với blob v1 đã lưu → magic/version check hiện có sẽ tự rơi về default (đã thiết kế vậy từ đầu), không cần lo mất an toàn.
- **`publish_period_ms` lưu trong NVS** là nơi cấu hình chính, đổi runtime không cần build lại. Kconfig chỉ cấp giá trị default lần đầu (khi blob chưa có / dùng default).

**Kconfig** (chỉ default lần đầu + tham số build, KHÔNG phải nơi cấu hình chính):
- `APP_MQTT_ENABLE` (bool, default y) — bật/tắt build module.
- `APP_MQTT_PUBLISH_PERIOD_MS_DEFAULT` (int, default 5000) — chỉ dùng làm default cho `config_mqtt_t.publish_period_ms` khi NVS chưa có. Runtime đọc từ NVS.
- `APP_MQTT_TASK_STACK_SIZE` (int, default 6144 — TLS cần stack lớn).

## 5. Topic schema

Tiền tố theo thiết bị để nhiều thiết bị không đụng nhau. `[QUYẾT ĐỊNH]` `<id>` = **device_name** (từ `config_system_t`):

```
pm/<device_name>/...
```

- **Client ID MQTT** (khác `<id>` topic): tự sinh = `device_name + "-" + 3 byte cuối MAC` để duy nhất trên broker kể cả khi 2 thiết bị trùng device_name.
- device_name nên sạch (không dấu cách/ký tự đặc biệt) để hợp lệ trong topic. Sẽ sanitize khi build topic.

### Publish (thiết bị → broker)

| Topic | Nội dung | QoS | Retain |
|---|---|---|---|
| `pm/<id>/telemetry` | JSON số đo (V/I/P/Q/S/PF/freq/temp) | 0 | no |
| `pm/<id>/energy` | JSON năng lượng (kWh import/export, demand) | 1 | no |
| `pm/<id>/io` | JSON trạng thái in0/in1/out0/out1 | 1 | yes |
| `pm/<id>/status` | online/offline (LWT), fw version | 1 | yes |
| `pm/<id>/heartbeat` | uptime, RSSI, free heap, iface active, **fw_version, active_broker, ip_address** | 0 | no |

- **LWT (Last Will)**: broker tự publish `status=offline` (retained) khi thiết bị mất kết nối đột ngột. Lúc connect publish `status=online`.
- `io` và `status` **retained** → client mới subscribe thấy ngay trạng thái cuối.

### Subscribe (broker → thiết bị)

| Topic | Payload (JSON) | Hành động |
|---|---|---|
| `pm/<id>/cmd/out0` | `{"on":true}` / `{"on":false}` | `io_expander_set_out0()` |
| `pm/<id>/cmd/out1` | `{"on":true}` / `{"on":false}` | `io_expander_set_out1()` |

- Payload là **JSON** (không phải `"0"/"1"`): `{"on":<bool>}`. Dễ mở rộng sau (thêm field như `pulse_ms`, `source`... mà không phá format).
- Sau khi đặt relay, publish lại `pm/<id>/io` để xác nhận (echo trạng thái thật).
- **Validate payload chặt**: parse JSON, chỉ chấp nhận field `on` kiểu bool; payload sai/thiếu field/không phải JSON → bỏ qua + log cảnh báo. Không thực thi gì khác.
- `[TODO-SAU]` (tùy chọn) `pm/<id>/cmd/reboot` — có trong doc network nhưng để sau, và phải cân nhắc an toàn.

## 6. Kiến trúc module

| Module | Trách nhiệm |
|---|---|
| `mqtt_manager` | Kết nối (TLS), reconnect, publish/subscribe, LWT, điều phối |
| (dùng lại) `config_store` | Đọc `config_mqtt_t` từ NVS |
| (dùng lại) `energy_meter` | Nguồn số đo/energy/demand |
| (dùng lại) `io_expander` | Đọc input, đặt output relay |
| (dùng lại) `network_manager` | Biết interface đang active, có IP chưa |

**Vòng đời**: `network_manager` báo "có data-path + IP" → `mqtt_manager` connect. Task publish định kỳ đọc dữ liệu từ energy_meter/io_expander, đóng gói JSON, publish.

**Reconnect chủ động theo interface (không chỉ dựa esp-mqtt).** Khi `network_manager` đổi default netif (failover ETH↔STA), TCP socket cũ của MQTT chết theo interface cũ. esp-mqtt có tự reconnect, nhưng nó không biết interface đã đổi — có thể mất nhiều giây bám socket cũ trước khi bỏ. Vì vậy `mqtt_manager` **theo dõi active_iface từ `network_manager_get_status()`**:
- Interface đổi (vd ETH→STA) → `mqtt_manager` **chủ động** `esp_mqtt_client_stop()` rồi `start()` lại, buộc bắt tay TCP/TLS mới trên interface mới ngay, không chờ esp-mqtt timeout.
- Mất IP hoàn toàn (không interface nào active) → stop client, chờ có IP lại mới start.
- Cơ chế auto-reconnect của esp-mqtt vẫn giữ (xử lý broker rớt tạm khi interface không đổi). Hai lớp bổ sung nhau: esp-mqtt lo broker/mạng chập chờn, mqtt_manager lo chuyển interface.

## 7. Luồng hoạt động

```
1. app_tasks khởi động mqtt_manager (sau network_manager).
2. mqtt_manager đọc broker config từ NVS:
   - đọc active_profile_index -> lấy profile đang active trong 3 profile.
   - profile disabled / URI rỗng -> không connect, chờ cấu hình.
3. Chờ network có IP (poll network_manager_get_status).
4. Dựng client_id = "<DeviceName>-<MAC 3 byte cuối>".
   Dựng device_id topic = DeviceName (sanitize ký tự topic).
5. esp_mqtt_client_start():
   - nếu profile.tls_enable: TLS. use_custom_ca -> dùng CA của profile;
     ngược lại -> CA bundle mặc định của IDF (broker public).
   - user/pass của profile + LWT status=offline retained.
6. On connected:
   - publish status=online (retained)
   - subscribe pm/<id>/cmd/out0, pm/<id>/cmd/out1
7. Task publish mỗi APP_MQTT_PUBLISH_PERIOD_MS:
   - telemetry, energy, io, heartbeat
8. On message (cmd/outX): parse JSON -> validate -> set relay -> echo pm/<id>/io
9. Broker/mạng chập chờn (interface KHÔNG đổi): esp-mqtt tự reconnect; broker phát LWT offline.
10. Interface đổi (failover ETH↔STA) hoặc mất IP: mqtt_manager theo dõi active_iface,
    chủ động stop + start lại client trên interface mới (không chờ esp-mqtt timeout socket cũ).
11. Đổi profile active (console/portal) -> stop client -> reconnect profile mới.
```

## 8. Kconfig dự kiến

```
APP_MQTT_ENABLE            (bool, default y)
APP_MQTT_PUBLISH_PERIOD_MS (int, default 5000)
APP_MQTT_TASK_STACK_SIZE   (int, default 6144 — TLS cần stack lớn)
APP_MQTT_TASK_PRIORITY     (int, default 5)
```
Broker profiles (URI/port/user/pass/TLS/CA) + `active_profile_index` lấy từ **NVS** (config_store), không hardcode Kconfig — đổi runtime không cần build lại. Kconfig chỉ giữ tham số vận hành (chu kỳ, stack, enable build).

## 9. Nạp CA certificate

Broker-independent, nên **CA theo từng profile, lưu NVS** — KHÔNG nhúng cứng vào firmware:

- `profile.tls_enable = false` → kết nối `mqtt://` thường (Mosquitto dev nội bộ).
- `profile.tls_enable = true`, `use_custom_ca = false` → TLS dùng **CA bundle mặc định của ESP-IDF** (`esp_crt_bundle_attach`), verify được hầu hết broker cloud công khai (HiveMQ/EMQX/AWS...). Không cần dán cert.
- `profile.tls_enable = true`, `use_custom_ca = true` → dùng **PEM trong `profile.ca_cert`** (broker tự dựng / CA riêng).

Cần bật `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE` để dùng CA bundle. Cert riêng nhập qua console/portal, lưu NVS.
- `[TODO-SAU]`: nhập CA dài qua portal (console giới hạn độ dài dòng).

## 10. Việc triển khai (thứ tự, mỗi bước build/test riêng)

1. **config_store: MQTT profiles** — mở rộng schema 3 profiles + active index + getter/setter. Bump version blob. Test: build, đọc/ghi profile.
2. **Kconfig + CMake + bật cert bundle** — tham số vận hành, chưa connect.
3. **mqtt_manager khung** — connect (TLS theo profile) + LWT + status online/offline, client_id tự sinh. Test: broker thấy thiết bị online.
4. **Publish telemetry/energy/io/heartbeat** — task định kỳ. Test: broker nhận JSON.
5. **Subscribe cmd/outX (JSON)** — parse + validate + set relay + echo. Test: publish lệnh → relay đổi.
6. **Lệnh console `mqtt-cfg`** — CRUD 3 profile + chọn active (giống `net-cfg`).
7. `[TODO-SAU]`: CA qua portal, cmd/reboot, publish theo sự kiện, alarm topic.

## 11. Điểm đã chốt

1. **Broker-independent, 3 profile** trong NVS, 1 active. Dev mặc định Mosquitto. ✓
2. **Device ID topic = DeviceName**; **client_id = DeviceName + MAC suffix** (tự sinh, duy nhất). ✓
3. **QoS**: telemetry QoS0 (no retain); energy/io/alarm/cmd QoS1. ✓
4. **Payload lệnh relay = JSON** `{"on":true}` / `{"on":false}` — một format duy nhất. ✓

## 12. Chốt thêm

5. **CA cert = `ca_cert[2048]`** cho giai đoạn đầu. Mở rộng sau nếu gặp chain dài / nhiều cert. ✓
6. **Alarm topic = `[TODO-SAU]`**. Hoàn thành telemetry/energy/io/relay/heartbeat trước. Khi chốt nguồn cảnh báo (lỗi ATM90, quá áp/dòng/công suất, mất pha...) mới thêm `pm/<id>/alarm` QoS1. ✓
7. **DeviceName** lấy từ `config_system_t.device_name`; rỗng → fallback `"PowerMeter"`. ✓

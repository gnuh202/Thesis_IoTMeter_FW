# Hướng dẫn MQTT: cấu hình, vận hành và test

Tài liệu này mô tả cách cấu hình MQTT cho ESP32-S3 Power Meter, cách chạy broker/test tool trên PC, các lệnh cần nhập trên console ESP, và bộ testcase kiểm chứng publish/subscribe.

> Topic và payload chi tiết xem [mqtt_payloads.md](mqtt_payloads.md). Kiến trúc tổng thể xem [architecture.md](architecture.md). Danh sách lệnh console đầy đủ xem [console_commands.md](console_commands.md).

---

## 1. Trạng thái hiện tại

Đã implement:

- MQTT client dùng `esp-mqtt`.
- 3 broker profiles trong NVS, 1 profile active.
- Cấu hình qua console `mqtt-cfg`.
- Client ID tự sinh: `<device_name>-<MAC suffix>`.
- Topic prefix: `pm/<device_name>/...`.
- Publish định kỳ: `telemetry`, `energy`, `io`, `heartbeat`.
- Publish `status=online` khi connect, LWT `offline` khi mất kết nối đột ngột.
- Subscribe lệnh relay: `pm/<id>/cmd/out0`, `pm/<id>/cmd/out1`.
- Payload command dạng JSON: `{"state":"on"}` hoặc `{"state":"off"}`.

Chưa làm / để sau:

- Nhập custom CA certificate qua console/portal.
- Command reboot qua MQTT.
- Alarm topic.
- Live-apply MQTT config không cần reboot. Hiện tại đổi `mqtt-cfg` xong cần reboot.

---

## 2. Mô hình test khuyến nghị

Trong giai đoạn phát triển, dùng broker Mosquitto trên PC/LAN:

```text
ESP32-S3 Power Meter  <--WiFi/ETH-->  PC chạy Mosquitto broker
                                      + mosquitto_sub
                                      + mosquitto_pub
```

Ví dụ:

| Thành phần | Ví dụ |
|---|---|
| IP PC/broker | `192.168.1.10` |
| MQTT port thường | `1883` |
| Device name | `Power_Meter` |
| Topic prefix | `pm/Power_Meter` |

Trong lệnh bên dưới, thay:

```text
<BROKER_IP> = IP máy PC chạy Mosquitto
<DEVICE_ID> = device_name đã sanitize, ví dụ Power_Meter
<PORT> = cổng serial ESP, ví dụ COM7 hoặc /dev/ttyACM0
```

---

## 3. Chuẩn bị PC

### 3.1. Cài Mosquitto

Windows: cài từ:

```text
https://mosquitto.org/download/
```

Sau khi cài, kiểm tra trong CMD/PowerShell:

```cmd
mosquitto -h
mosquitto_sub -h
mosquitto_pub -h
```

Nếu không tìm thấy lệnh, thêm thư mục cài Mosquitto vào `PATH`, ví dụ:

```text
C:\Program Files\mosquitto
```

Ubuntu/Debian:

```bash
sudo apt update
sudo apt install mosquitto mosquitto-clients
```

macOS:

```bash
brew install mosquitto
```

### 3.2. Chạy broker local không auth

PC terminal 1:

```bash
mosquitto -p 1883 -v
```

Cửa sổ này giữ mở để xem log broker. Khi ESP connect thành công, log sẽ có client ID của thiết bị.

> Broker không auth chỉ dùng trong lab nội bộ. Không dùng cho môi trường thật vì MQTT command có thể điều khiển relay vật lý.

### 3.3. Subscribe tất cả topic của thiết bị

PC terminal 2:

Windows CMD:

```cmd
mosquitto_sub -h <BROKER_IP> -p 1883 -t "pm/<DEVICE_ID>/#" -v
```

Linux/macOS/Git Bash:

```bash
mosquitto_sub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/#' -v
```

Ví dụ:

```bash
mosquitto_sub -h 192.168.1.10 -p 1883 -t 'pm/Power_Meter/#' -v
```

Lệnh subscribe nhanh để dò toàn bộ topic `pm` trên broker PC hiện tại:

```cmd
mosquitto_sub -h 192.168.137.1 -t "pm/#" -v
```

---

## 4. Cấu hình trên console ESP

### 4.1. Mở console

```bash
idf.py -p <PORT> monitor
```

Sau khi login thành công, prompt:

```text
meter>
```

Gõ `help` để xem lệnh.

### 4.2. Kiểm tra network

MQTT chỉ connect khi ESP có IP.

```text
net status
```

Nếu dùng WiFi STA, cấu hình WiFi trước theo [console_commands.md](console_commands.md), ví dụ:

```text
wifi-cfg set --ssid "TenWiFi" --pass "MatKhauWiFi"
wifi-cfg enable
```

### 4.3. Cấu hình MQTT profile local

```text
mqtt-cfg show
mqtt-cfg set --idx 0 --name "Local" --uri <BROKER_IP> --port 1883
mqtt-cfg active --idx 0
mqtt-cfg enable
mqtt-cfg period --period 5000
mqtt-cfg show
```

Ví dụ:

```text
mqtt-cfg set --idx 0 --name "Local" --uri 192.168.1.10 --port 1883
mqtt-cfg active --idx 0
mqtt-cfg enable
mqtt-cfg period --period 5000
mqtt-cfg show
```

Quan trọng: hiện tại thay đổi MQTT config cần reboot để task MQTT đọc lại NVS:

```text
reboot
```

Nếu không có lệnh `reboot`, nhấn reset board hoặc power-cycle.

### 4.4. Bật log MQTT khi debug

```text
log mqtt_mgr debug
log network_mgr info
```

Giảm log:

```text
log mqtt_mgr warn
```

---

## 5. Topic/payload cần thấy trên PC

Sau khi ESP reboot và network có IP, terminal `mosquitto_sub` phải thấy:

```text
pm/Power_Meter/status online
pm/Power_Meter/telemetry {...}
pm/Power_Meter/energy {...}
pm/Power_Meter/io {...}
pm/Power_Meter/heartbeat {...}
```

Tóm tắt topic:

| Topic | Hướng | QoS | Retain | Ghi chú |
|---|---|---:|---|---|
| `pm/<id>/telemetry` | ESP → broker | 0 | no | số đo tức thời |
| `pm/<id>/energy` | ESP → broker | 1 | no | năng lượng/demand |
| `pm/<id>/io` | ESP → broker | 1 | yes | input/output, echo sau command |
| `pm/<id>/heartbeat` | ESP → broker | 0 | no | uptime/heap/fw/ip |
| `pm/<id>/status` | ESP → broker | 1 | yes | `online`/`offline` |
| `pm/<id>/cmd/out0` | broker → ESP | 1 | — | điều khiển relay 0 |
| `pm/<id>/cmd/out1` | broker → ESP | 1 | — | điều khiển relay 1 |

Payload chi tiết xem [mqtt_payloads.md](mqtt_payloads.md).

---

## 6. Lệnh điều khiển relay từ PC

> Cảnh báo: command MQTT điều khiển relay vật lý. Chỉ test khi tải an toàn hoặc đã cô lập ngõ ra.

### Windows CMD

```cmd
mosquitto_pub -h <BROKER_IP> -p 1883 -t "pm/<DEVICE_ID>/cmd/out0" -q 1 -m "{\"state\":\"on\"}"
mosquitto_pub -h <BROKER_IP> -p 1883 -t "pm/<DEVICE_ID>/cmd/out0" -q 1 -m "{\"state\":\"off\"}"
mosquitto_pub -h <BROKER_IP> -p 1883 -t "pm/<DEVICE_ID>/cmd/out1" -q 1 -m "{\"state\":\"on\"}"
mosquitto_pub -h <BROKER_IP> -p 1883 -t "pm/<DEVICE_ID>/cmd/out1" -q 1 -m "{\"state\":\"off\"}"
```

### PowerShell

```powershell
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{"state":"on"}'
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{"state":"off"}'
```

### Linux/macOS/Git Bash

```bash
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{"state":"on"}'
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{"state":"off"}'
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out1' -q 1 -m '{"state":"on"}'
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out1' -q 1 -m '{"state":"off"}'
```

Sau mỗi lệnh hợp lệ, ESP publish lại `pm/<id>/io` để xác nhận trạng thái thật.

---

## 7. Test cases

### TC-MQTT-001 — Broker local chạy được

PC terminal 1:

```bash
mosquitto -p 1883 -v
```

PC terminal 2:

```bash
mosquitto_sub -h 127.0.0.1 -p 1883 -t 'test/#' -v
```

PC terminal 3:

```bash
mosquitto_pub -h 127.0.0.1 -p 1883 -t 'test/hello' -m 'ok'
```

Kỳ vọng terminal 2 thấy:

```text
test/hello ok
```

### TC-MQTT-002 — ESP connect broker và publish status online

ESP console:

```text
mqtt-cfg set --idx 0 --name "Local" --uri <BROKER_IP> --port 1883
mqtt-cfg active --idx 0
mqtt-cfg enable
mqtt-cfg period --period 5000
mqtt-cfg show
reboot
```

PC:

```bash
mosquitto_sub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/#' -v
```

Kỳ vọng nhận được:

```text
pm/<DEVICE_ID>/status online
```

### TC-MQTT-003 — Publish telemetry/energy/io/heartbeat định kỳ

PC subscribe:

```bash
mosquitto_sub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/#' -v
```

Kỳ vọng mỗi chu kỳ thấy:

```text
pm/<DEVICE_ID>/telemetry {...}
pm/<DEVICE_ID>/energy {...}
pm/<DEVICE_ID>/io {...}
pm/<DEVICE_ID>/heartbeat {...}
```

### TC-MQTT-004 — Đổi chu kỳ publish

ESP console:

```text
mqtt-cfg period --period 2000
mqtt-cfg show
reboot
```

Kỳ vọng telemetry/heartbeat xuất hiện khoảng mỗi 2 giây.

Khôi phục:

```text
mqtt-cfg period --period 5000
reboot
```

### TC-MQTT-005 — Điều khiển relay out0 hợp lệ

PC subscribe:

```bash
mosquitto_sub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/io' -v
```

PC publish ON/OFF:

```bash
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{"state":"on"}'
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{"state":"off"}'
```

Kỳ vọng relay out0 đổi trạng thái và `io` echo đúng `out0:true/false`.

### TC-MQTT-006 — Điều khiển relay out1 hợp lệ

Dùng topic:

```text
pm/<DEVICE_ID>/cmd/out1
```

Kỳ vọng relay out1 đổi trạng thái và `io` echo đúng `out1:true/false`.

### TC-MQTT-007 — Payload sai bị bỏ qua

PC publish payload sai:

```bash
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m 'on'
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{}'
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{"state":"toggle"}'
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{"state":1}'
```

Kỳ vọng:

- Relay không đổi.
- ESP log cảnh báo payload invalid.
- `io` vẫn giữ trạng thái cũ.

### TC-MQTT-008 — Retained status/io

Mở subscriber mới sau khi ESP đã online:

```bash
mosquitto_sub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/status' -v
mosquitto_sub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/io' -v
```

Kỳ vọng nhận ngay retained `status` và trạng thái `io` cuối cùng.

### TC-MQTT-009 — LWT offline

PC subscribe status:

```bash
mosquitto_sub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/status' -v
```

Rút nguồn ESP hoặc ngắt mạng đột ngột.

Kỳ vọng sau keepalive/timeout:

```text
pm/<DEVICE_ID>/status offline
```

Lưu ý: reset mềm/disconnect sạch có thể không kích hoạt LWT.

### TC-MQTT-010 — Disable MQTT

ESP console:

```text
mqtt-cfg disable
mqtt-cfg show
reboot
```

Kỳ vọng ESP không connect/publish MQTT.

Bật lại:

```text
mqtt-cfg enable
reboot
```

---

## 8. Broker có username/password

Tạo password file:

```bash
mosquitto_passwd -c passwd meter
```

Tạo file `mosquitto_auth.conf`:

```conf
listener 1883
allow_anonymous false
password_file passwd
```

Chạy broker:

```bash
mosquitto -c mosquitto_auth.conf -v
```

Cấu hình ESP:

```text
mqtt-cfg set --idx 0 --name "LocalAuth" --uri <BROKER_IP> --port 1883 --user meter --pass meter123
mqtt-cfg active --idx 0
mqtt-cfg enable
reboot
```

Test từ PC:

```bash
mosquitto_sub -h <BROKER_IP> -p 1883 -u meter -P meter123 -t 'pm/<DEVICE_ID>/#' -v
mosquitto_pub -h <BROKER_IP> -p 1883 -u meter -P meter123 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{"state":"on"}'
```

---

## 9. Ghi chú TLS

Firmware hỗ trợ hướng TLS ở tầng thiết kế/profile, nhưng custom CA qua console/portal hiện để sau. Trong lab nên bắt đầu với `mqtt://` port 1883 để xác minh chức năng trước.

Khi triển khai thật:

- Dùng `mqtts://`/port 8883.
- Bật username/password riêng cho từng thiết bị.
- Broker cấu hình ACL để chỉ đúng client được publish vào `pm/<device>/cmd/#`.
- Không dùng broker public mở cho relay command.

---

## 10. Checklist debug nhanh

### Không thấy ESP connect broker

ESP console:

```text
net status
mqtt-cfg show
log mqtt_mgr debug
```

Kiểm tra:

- ESP có IP chưa?
- PC và ESP cùng LAN không?
- `<BROKER_IP>` đúng IP máy chạy Mosquitto không?
- Firewall Windows có chặn port 1883 không?
- Broker có đang chạy `mosquitto -p 1883 -v` không?
- `mqtt-cfg enable` chưa?
- Đã reboot sau khi đổi `mqtt-cfg` chưa?

Test port từ PC:

PowerShell:

```powershell
Test-NetConnection <BROKER_IP> -Port 1883
```

Linux/macOS:

```bash
nc -vz <BROKER_IP> 1883
```

### ESP connect nhưng không thấy topic

Subscribe rộng để dò đúng device id:

```bash
mosquitto_sub -h <BROKER_IP> -p 1883 -t 'pm/#' -v
```

Kiểm tra device name có bị sanitize không. Ví dụ `Power Meter` thành `Power_Meter`.

### Command relay không chạy

Kiểm tra:

- Topic đúng `pm/<DEVICE_ID>/cmd/out0` hoặc `cmd/out1` chưa?
- Payload đúng JSON chưa?
- Escape quote đúng shell chưa?
- ESP log có báo payload invalid không?
- Relay/IO phần cứng đã test local chưa?

### Status offline không xuất hiện

- LWT chỉ phát khi kết nối mất đột ngột.
- Đợi quá keepalive timeout.
- Subscribe retained status đúng topic.

---

## 11. Quy tắc an toàn khi nghiệm thu

1. Không nối tải nguy hiểm khi test relay qua MQTT lần đầu.
2. Broker test không auth chỉ dùng trong lab kín.
3. Khi ra môi trường thật phải có TLS/mạng riêng, username/password và ACL theo từng thiết bị.
4. MQTT lỗi không được ảnh hưởng đo lường, Modbus và console.

---

## 12. Tóm tắt lệnh nhanh

PC broker:

```bash
mosquitto -p 1883 -v
```

PC subscribe:

```bash
mosquitto_sub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/#' -v
```

ESP console:

```text
mqtt-cfg set --idx 0 --name "Local" --uri <BROKER_IP> --port 1883
mqtt-cfg active --idx 0
mqtt-cfg enable
mqtt-cfg period --period 5000
mqtt-cfg show
reboot
```

PC command relay:

```bash
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{"state":"on"}'
mosquitto_pub -h <BROKER_IP> -p 1883 -t 'pm/<DEVICE_ID>/cmd/out0' -q 1 -m '{"state":"off"}'
```

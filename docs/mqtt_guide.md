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
- TLS: certificate store trên partition FAT `storage` (mount `/flash`), **mỗi profile một bộ
  CA/cert/key riêng**, upload bằng file picker ngay trong khối máy chủ của Web Config Portal
  (mục **MQTT servers**) hoặc bằng curl.
  Xem [mục 9](#9-tls).

Chưa làm / để sau:

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

## 9. TLS

Trong lab nên bắt đầu với `mqtt://` port 1883 để xác minh chức năng trước, rồi mới bật TLS.

### 9.1. Certificate store trên flash nội

Certificate nằm trên partition FAT `storage` (1 MB, khai báo trong `partitions.csv`), mount tại
`/flash` khi boot. Không dùng SD card. Không lưu PEM vào NVS — NVS chỉ giữ *đường dẫn* trong
snapshot của Configuration Manager.

**Mỗi MQTT profile có bộ ca/cert/key riêng.** 3 profile là 3 broker độc lập, thường không dùng
chung CA; nếu dùng chung một `ca.pem` thì đổi profile active sẽ âm thầm verify bằng CA của
broker trước. File đặt tên theo index profile:

| Slot | Profile 0 | Profile 1 | Profile 2 | Dùng cho |
|---|---|---|---|---|
| `ca` | `/flash/ca0.pem` | `/flash/ca1.pem` | `/flash/ca2.pem` | CA của broker (CA_ONLY, MUTUAL) |
| `cert` | `/flash/cert0.pem` | `/flash/cert1.pem` | `/flash/cert2.pem` | client certificate (MUTUAL) |
| `key` | `/flash/key0.pem` | `/flash/key1.pem` | `/flash/key2.pem` | client private key (MUTUAL) |

Partition được format tự động ở lần boot đầu. Tên file phải nằm trong 8.3 vì project đặt
`CONFIG_FATFS_LFN_NONE=y` — tên dài nhất là `cert0` (5 ký tự), vẫn thoải mái.

> Nếu board đã từng upload theo cách cũ (một bộ `ca.pem`/`cert.pem`/`key.pem` dùng chung) thì
> các file đó thành mồ côi: firmware không đọc chúng nữa. Upload lại cho từng profile rồi chạy
> `mqtt-cfg set --idx N --tls ca` để `ca_path` trỏ đúng `ca<N>.pem`.

### 9.2. Upload certificate

Cả hai cách đều dùng cùng một endpoint `POST /api/cert?slot=ca|cert|key&profile=0|1|2` và đều
cần session của Web Config Portal (cùng tài khoản console). Thiếu `profile` thì mặc định là 0;
`profile` ngoài `0..2` bị từ chối (không clamp) để một cái typo không ghi đè file của profile
khác.

**Cách 1 — chọn file trong portal (thường dùng).** Mở `http://192.168.4.1/`, login, vào mục
**MQTT servers**. Giao diện portal là tiếng Anh, nên tên nút/mục dưới đây trích đúng chuỗi đang
hiển thị. Mỗi profile là một khối gập/mở "Server 1..3" (máy chủ đang dùng mở sẵn) chứa
*toàn bộ* thông tin của máy chủ đó: địa chỉ, cổng, tài khoản, mức bảo mật, và 3 file chứng chỉ
ca/cert/key. Không còn mục "Certs" riêng — nhờ vậy không thể upload nhầm sang máy chủ khác. Mỗi
slot có một nút chọn file: chọn file từ máy rồi bấm **Upload file**, có hiệu lực ngay không cần
bấm Save. Slot nào đã có file thì hiện `Loaded` + size + 16 hex đầu của SHA-256 kèm nút
**Delete file**; slot rỗng hiện `Not loaded`.

Upload và delete **không reload trang**: dòng trạng thái của riêng slot đó tự cập nhật,
mọi ô text đang nhập dở vẫn còn nguyên.

Các ô text (địa chỉ, cổng, tài khoản, mức bảo mật, chu kỳ gửi, tên thiết bị, WiFi) dùng chung
**một** nút duy nhất ở cuối trang: `Save and restart`. Chỉ một nút vì hầu như không field nào
áp được khi đang chạy — lưu mà không reboot chỉ trông như đã có hiệu lực. Chọn mức bảo mật ở
dropdown **Connection security** thì firmware tự trỏ đường dẫn certificate theo đúng profile,
không cần gõ `mqtt-cfg set --tls` nữa. Dropdown chỉ có 3 mức an toàn (`No encryption (port 1883)`,
`TLS (port 8883) — typical`, `TLS with device certificate`); `insecure` là chế độ debug nên
không xuất hiện trên web.

Nhận file đuôi `.pem`, `.crt`, `.cer`, `.key`. Đuôi file **không** được kiểm tra — firmware chỉ
xét nội dung, nên file nào là PEM text (mở bằng notepad thấy `-----BEGIN ...`) là được. File
DER nhị phân bị từ chối; đổi sang PEM trước:

```bash
openssl x509 -inform der -in ca.der -out ca.pem
```

**Cách 2 — curl.** Body là PEM thô:

```bash
# login lấy cookie session (portal dùng cookie wp_session, không phải HTTP Basic)
curl -c cookies.txt -d "user=<user>&pass=<pass>" http://192.168.4.1/login

# upload cho profile 0
curl -b cookies.txt --data-binary @ca.pem   "http://192.168.4.1/api/cert?slot=ca&profile=0"
curl -b cookies.txt --data-binary @cert.pem "http://192.168.4.1/api/cert?slot=cert&profile=0"
curl -b cookies.txt --data-binary @key.pem  "http://192.168.4.1/api/cert?slot=key&profile=0"

# broker khác thì đổi profile
curl -b cookies.txt --data-binary @ca2.pem  "http://192.168.4.1/api/cert?slot=ca&profile=1"

# trạng thái tất cả profile: chỉ có/không + size + 16 hex đầu của SHA-256
curl -b cookies.txt "http://192.168.4.1/api/cert"

# xoá theo profile + slot
curl -b cookies.txt -X POST "http://192.168.4.1/api/cert/delete?slot=key&profile=0"
```

Firmware từ chối body rỗng, PEM > 8192 byte, và body không có envelope
`-----BEGIN`/`-----END`. Khoảng trắng đầu/cuối và BOM bị cắt trước khi ghi, nên file kéo từ
editor Windows vẫn dùng được. Upload ghi ra file tạm rồi rename, nên một upload bị ngắt giữa
đường không để lại PEM cụt ở đường dẫn mà MQTT runtime sẽ đọc.

**Không có API nào đọc lại nội dung certificate hay private key.** `GET /api/cert` và
`mqtt-cfg show` chỉ báo tồn tại, size và fingerprint ngắn.

### 9.3. Chọn TLS mode

```
mqtt-cfg set --idx 0 --uri broker.example.com --port 8883 --tls ca
cfg-save
cfg-apply mqtt        # hoặc reboot
```

`--tls` nhận `off|ca|mutual|insecure` và tự điền path theo **profile `--idx`** (profile 0 lấy
`ca0.pem`, profile 1 lấy `ca1.pem`...):

- `ca` — nếu chưa upload `ca<N>.pem` thì `ca_path` để trống và firmware verify bằng
  **certificate bundle** dựng sẵn trong image (đủ cho broker dùng CA public như HiveMQ Cloud,
  EMQX Cloud). Nếu đã upload `ca<N>.pem` thì dùng file đó. Cả hai đều là verify đầy đủ.
- `mutual` — yêu cầu đủ cả `ca<N>.pem` + `cert<N>.pem` + `key<N>.pem`; thiếu một file là từ chối
  connect, không tự tụt xuống verify một chiều.
- `insecure` — chỉ để debug và build hiện tại **không** bật
  `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`, nên chọn mode này sẽ bị từ chối connect kèm log
  giải thích. Không bao giờ ship với mode này bật.

Khi `tls_mode != off`, firmware tự dùng scheme `mqtts://`.

### 9.4. Cảnh báo bảo mật

Private key hiện lưu **plaintext** trên flash và có thể lấy ra bằng `esptool read_flash`.
Chấp nhận cho giai đoạn hiện tại (lab/đồ án). Trước khi ra production bằng mutual TLS thì
**bắt buộc bật flash encryption** — đây là điều kiện tiên quyết, không phải việc làm sau.

### 9.5. Khi triển khai thật

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

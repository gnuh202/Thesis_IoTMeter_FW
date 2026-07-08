# Console Commands & Log Tags

Firmware có một **developer console** (esp_console qua USB serial/JTAG). Sau khi bật nguồn, console yêu cầu đăng nhập (nếu `CONFIG_APP_CONSOLE_AUTH_ENABLE`), rồi cho phép gõ các lệnh dưới đây.

> Tất cả cấu hình do các lệnh này lưu đều nằm trong **NVS** và **sống qua reboot**. Nhiều thay đổi network/MQTT cần **reboot mới áp dụng** (ghi rõ ở từng lệnh).

---

## 1. `meter-latest`

In số đo mới nhất của ATM90E32AS (V/I/P/Q/S/PF/freq/temp).

```
meter-latest
```

Không có tham số.

---

## 2. `meter-reg` — đọc/ghi thanh ghi ATM90E32AS

```
meter-reg read <addr>
meter-reg write <addr> <value>
```

| Vị trí | Bắt buộc | Ý nghĩa |
|---|---|---|
| `read\|write` | có | thao tác |
| `<addr>` | có | địa chỉ thanh ghi, hex, vd `0x61` |
| `[value]` | chỉ khi `write` | giá trị ghi, hex, vd `0x1C89` |

Ví dụ:
```
meter-reg read 0x61
meter-reg write 0x61 0x1C89
```

---

## 3. `meter-cal` — hiệu chỉnh ATM90E32AS

```
meter-cal <show|default|apply|save|load|set|guide> [tùy chọn]
```

| Subcommand | Việc |
|---|---|
| `show` | in cấu hình calib hiện tại |
| `default` | nạp giá trị calib mặc định (chưa ghi NVS) |
| `apply` | áp calib hiện tại xuống chip |
| `save` | lưu calib vào NVS |
| `load` | đọc calib từ NVS |
| `set` | đặt một trường calib (xem bên dưới) |
| `guide` | in hướng dẫn quy trình calib |

Tham số cho `set`:

| Cờ | Ý nghĩa |
|---|---|
| `--field <f>` | `gain\|offset\|power-offset\|phase\|ref\|pga\|wiring\|freq` |
| `--phase <a\|b\|c>` | pha đích (với trường theo pha) |
| `--u <n>` | giá trị liên quan điện áp |
| `--i <n>` | giá trị liên quan dòng |
| `--p <n>` | offset công suất tác dụng |
| `--q <n>` | offset công suất phản kháng |
| `--phi <n>` | bù pha |
| `--value <v>` | giá trị chip-wide: `pga 1\|2\|4`, `wiring 3p4w\|3p3w`, `freq 50\|60` |
| `--apply` | áp ngay xuống chip |

Chi tiết quy trình calib: xem [atm90e32as_console_calib.md](atm90e32as_console_calib.md). Bắt đầu nhanh: `meter-cal guide`.

---

## 4. `net-cfg` — cấu hình mạng

```
net-cfg show
net-cfg sta --ssid <ssid> [--pass <pass>]
net-cfg ap on|off
```

| Subcommand | Việc | Áp dụng |
|---|---|---|
| `show` | in mode / WiFi SSID / trạng thái pass / eth_dhcp | ngay |
| `sta --ssid <s> [--pass <p>]` | lưu credential WiFi STA vào NVS + đẩy vào driver đang chạy | dùng ở lần failover kế tiếp, không cần reboot |
| `ap on` | bật SoftAP (config portal) on-demand | ngay |
| `ap off` | tắt SoftAP | ngay |

Ví dụ:
```
net-cfg sta --ssid "MyWiFi" --pass "secret123"
net-cfg ap on
```

SSID có dấu cách phải bọc trong dấu nháy kép.

---

## 5. `mqtt-cfg` — cấu hình MQTT (3 broker profile)

```
mqtt-cfg show
mqtt-cfg set --idx <0..2> [--name <n>] [--uri <host>] [--port <n>] [--user <u>] [--pass <p>]
mqtt-cfg active --idx <0..2>
mqtt-cfg enable | disable
mqtt-cfg period --period <ms>
```

| Subcommand | Việc |
|---|---|
| `show` | in enabled/active/keepalive/period + cả 3 profile (profile active đánh dấu `*`) |
| `set --idx <n> ...` | đặt các trường của profile `n` (chỉ trường có truyền vào) |
| `active --idx <n>` | chọn profile active |
| `enable` / `disable` | bật/tắt MQTT client |
| `period --period <ms>` | chu kỳ publish telemetry |

Cờ cho `set`:

| Cờ | Ý nghĩa |
|---|---|
| `--idx <0..2>` | chỉ số profile (bắt buộc với `set`/`active`) |
| `--name <n>` | nhãn profile |
| `--uri <host>` | host broker, **không kèm scheme** (vd `192.168.1.10`, không phải `mqtt://...`) |
| `--port <n>` | cổng, vd `1883` (thường) / `8883` (TLS) |
| `--user <u>` | username broker |
| `--pass <p>` | password broker |

**Mọi thay đổi `mqtt-cfg` cần reboot mới áp dụng** (task MQTT đọc config một lần lúc khởi động).

Ví dụ:
```
mqtt-cfg set --idx 0 --name "Local" --uri 192.168.1.10 --port 1883
mqtt-cfg active --idx 0
mqtt-cfg enable
# reboot
```

> TLS/custom-CA chưa cấu hình được qua console (CA PEM quá dài cho một dòng lệnh) — xem [mqtt_guide.md](mqtt_guide.md) mục ghi chú TLS/TODO.

---

## 6. `log` — chỉnh mức log runtime theo TAG

Bật/tắt hoặc đổi độ chi tiết log của từng task **mà không cần build lại**. Dùng để tắt task log quá nhiều, giữ lại task cần theo dõi.

```
log <tag|*> <none|error|warn|info|debug|verbose>
```

| Tham số | Ý nghĩa |
|---|---|
| `<tag>` | TAG của task (bảng dưới), hoặc `*` cho tất cả |
| `<level>` | `none` (tắt hẳn) → `verbose` (chi tiết nhất) |

Ví dụ:
```
log network_comm none      # tắt hẳn log ping (task hay spam)
log mqtt_mgr warn          # MQTT chỉ còn cảnh báo/lỗi
log * warn                 # ẩn bớt tất cả, chỉ còn warn+error
log energy_meter info      # bật lại log mặc định cho một task
```

> Mức log tối đa biên dịch sẵn là **DEBUG** (`CONFIG_LOG_MAXIMUM_LEVEL`), nên `verbose` có thể không hiện thêm gì so với `debug`.

### Bảng TAG log

**App (main/app/):**

| TAG | Module | Ghi chú log |
|---|---|---|
| `app_main` | điểm vào | ít |
| `app_tasks` | khởi động task | ít |
| `energy_meter` | task đo ATM90E32AS | vừa |
| `console_task` | console | ít |
| `modbus_slave` | Modbus RTU slave | vừa |
| `modbus_master` | Modbus RTU master | vừa |
| `net_mgr` | network manager (state machine, failover) | vừa |
| `network_comm` | task ping | **nhiều** (mỗi ping) |
| `ethernet_driver` | driver W5500 | vừa (link/IP event) |
| `wifi_manager` | WiFi STA/AP | vừa |
| `mqtt_mgr` | MQTT client | **nhiều** (mỗi publish/connect) |

**Component (components/):**

| TAG | Module |
|---|---|
| `config_store` | lưu cấu hình NVS |
| `io_expander` | BSP PCF8574 (relay + input) |
| `atm90e32as` | driver IC đo |
| `pcf8574` | driver GPIO expander |
| `sd_card` | thẻ SD |
| `spi_bus_shared` | SPI bus dùng chung |

---

## Ghi chú áp dụng cấu hình

| Lệnh | Áp dụng khi |
|---|---|
| `net-cfg sta` | ngay (đẩy vào driver) — dùng ở failover kế tiếp |
| `net-cfg ap` | ngay |
| `mqtt-cfg *` | **cần reboot** |
| `meter-cal save` | ghi NVS ngay; `apply` áp xuống chip ngay |
| `log` | ngay (chỉ hiệu lực tới khi reboot — không lưu NVS) |

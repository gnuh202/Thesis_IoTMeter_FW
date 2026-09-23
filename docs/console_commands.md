# Console Commands & Log Tags

Firmware có một **developer console** (esp_console qua USB serial/JTAG). Sau khi bật nguồn, console yêu cầu đăng nhập (nếu `CONFIG_APP_CONSOLE_AUTH_ENABLE`), rồi cho phép gõ các lệnh dưới đây.

> Cấu hình lưu qua NVS thì **sống qua reboot**. Ngoại lệ: `mqtt-cfg` chỉ sửa RAM — cần
> `cfg-save` để xuống NVS (ghi rõ ở từng lệnh). Đường áp dụng runtime (`cfg-apply`) được ghi
> chú ở cuối tài liệu.

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
meter-cal <show|default|apply|save|load|auto|auto-pq-gain|auto-phi|phi-err|get-p|auto-power-offset|set|guide> [tùy chọn]
```

| Subcommand | Việc |
|---|---|
| `show` | in cấu hình calib hiện tại |
| `default` | nạp giá trị calib mặc định (không `--field`: toàn image, cần `--apply`; `--field <f>`: reset chọn lọc, áp ngay) |
| `apply` | áp calib hiện tại xuống chip |
| `save` | lưu calib vào NVS |
| `load` | đọc calib từ NVS |
| `set` | đặt một trường calib (xem bên dưới) |
| `guide` | in hướng dẫn quy trình calib |
| `auto` | auto-cal voltage/current gain hoặc offset (một pha hoặc cả 3 pha) |
| `auto-pq-gain` | auto-cal active power gain (PQGain) |
| `auto-phi` | auto-cal phase compensation (Phi) với P_ref trực tiếp |
| `phi-err` | auto-cal phase compensation với known power error (không cần reference meter trực tiếp) |
| `get-p` | đọc công suất trung bình từ chip (dùng cho workflow calib thủ công) |
| `auto-power-offset` | auto-cal active/reactive power offset |

Tham số cho `set` / `auto`:

| Cờ | Ý nghĩa |
|---|---|
| `--field <f>` | `set`: `uigain\|uioffset\|gain\|offset\|power-offset\|phase\|pq-gain\|fundamental-power-gain\|pga\|wiring\|freq`; `auto`: `u\|i`; `default --field`: `phi\|pqgain\|uigain\|uioffset\|power-offset\|fundamental\|all` |
| `--phase <a\|b\|c\|all>` | pha đích. `auto`: bỏ trống hoặc `all` = calib cả 3 pha cùng một reference chung. `set`/`default`: chỉ `a\|b\|c` (bỏ trống = cả 3 với `default --field`) |
| `--u <n>` | giá trị liên quan điện áp |
| `--i <n>` | giá trị liên quan dòng |
| `--p <n>` | offset công suất tác dụng |
| `--q <n>` | offset công suất phản kháng |
| `--phi <n>` | bù pha |
| `--value <v>` | `auto`: reference (`<số>\|external\|offset`); `set`: chip-wide `pga 1\|2\|4`, `wiring 3p4w\|3p3w`, `freq 50\|60`; `get-p`: số mẫu (1-50, default 3) |
| `--interval <ms>` | khoảng thời gian giữa các mẫu cho `get-p` (1-1000ms, default 100ms) |
| `--error <percent>` | sai số công suất đã biết từ PF=1 baseline (dùng cho `phi-err`) |
| `--apply` | **Bắt buộc** với chip-wide `pga\|wiring\|freq` và `default` không `--field`. Với calib per-phase (`auto`, `set <field per-phase>`, `default --field`) giá trị **tự áp xuống chip ngay**, `--apply` là no-op |

### Các subcommands mới cho phase calibration

**`phi-err` - Calibrate phase với known power error:**

Dùng khi không thể cấp nguồn đồng thời cho DUT và reference meter. Workflow:
1. Đo sai số công suất tại PF=1 (dùng `ref compare`)
2. Dùng `phi-err` với error đó để calibrate phase tại PF=0.5

```bash
# Sau khi có error từ ref compare
meter-cal phi-err --phase a --error 1.166 [--tolerance 2.0]
```

Command sẽ:
- Đo P_chip hiện tại
- Tính ngược P_ref = P_chip / (1 + error/100)
- Chạy thuật toán phase calibration như `auto-phi`
- Verify residual error trong tolerance

**`get-p` - Đọc công suất trung bình từ chip:**

Đo công suất từ DUT chip (không phải reference meter), hỗ trợ nhiều mẫu:

```bash
meter-cal get-p --phase a --value 10 --interval 200
# Output: Average active power: 1234.678 W (12346780 mW)
```

Dùng cho:
- Workflow calibration thủ công
- Verify công suất sau khi calib
- So sánh với reference meter (dùng `ref compare` để tự động)

Chi tiết quy trình calib: xem [atm90e32as_console_calib.md](atm90e32as_console_calib.md). Bắt đầu nhanh: `meter-cal guide`.

Ví dụ auto-cal U/I gain cả 3 pha cùng một reference (một nguồn AC + trung tính cho 3 kênh áp; hoặc 3 CT trên cùng một tải):

```text
meter-cal auto --field u --value 220            # U gain cả 3 pha
meter-cal auto --field i --value 5             # I gain cả 3 pha
meter-cal auto --field i --phase a --value 5   # chỉ pha A
meter-cal auto --field u --value offset        # U offset cả 3 pha (không tải)
```

All-or-nothing: nếu một pha không đo được (CT hở, giá trị ≤ 0), cả lệnh fail và **không ghi gì**; chip giữ nguyên calib cũ. Chỉ calib ở **3P4W** (3P3W trả lỗi vì cần trung tính; gain dùng chung giữa 2 mode).

Ví dụ auto-cal PQGain (phải đã calib U/I, tải PF≈1, dòng Ib):

```text
meter-cal auto-pq-gain --phase a --value 123.4
```

Lệnh lấy mẫu nhanh (3 mẫu × 100 ms ≈ 300 ms) để giảm sai lệch do tải thay đổi. Có thể chạy lại nhiều lần để tinh chỉnh; firmware tự động hiệu chỉnh tăng dần từ PQGain hiện tại. Giữ tải ổn định trong ~1 giây từ lúc nhập lệnh.

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

## 5. `mqtt-cfg` — cấu hình MQTT (một broker duy nhất)

```
mqtt-cfg show
mqtt-cfg set [--name <n>] [--uri <host>] [--port <n>] [--user <u>] [--pass <p>] [--tls <mode>]
mqtt-cfg enable | disable
mqtt-cfg period --period <s>
```

| Subcommand | Việc |
|---|---|
| `show` | in broker duy nhất (enable/name/host/port/keepalive/user/pass/tls_mode), `publish_period=<s>`, và trạng thái certificate store |
| `set ...` | đặt từng trường của broker (chỉ trường có truyền vào) |
| `enable` / `disable` | bật/tắt MQTT client — đường sản phẩm là **LCD** (Settings > MQTT > Status); lệnh này dành cho dev |
| `period --period <s>` | chu kỳ publish telemetry, tính theo **giây**, chặn ngoài khoảng **1..60** |

Cờ cho `set`:

| Cờ | Ý nghĩa |
|---|---|
| `--name <n>` | nhãn broker (lên heartbeat `active_broker`) |
| `--uri <host>` | host broker, **không kèm scheme** (vd `192.168.1.10`, không phải `mqtt://...`) |
| `--port <n>` | cổng, vd `1883` (thường) / `8883` (TLS) |
| `--user <u>` | username broker |
| `--pass <p>` | password broker |
| `--tls <mode>` | `off`\|`ca`\|`mutual`\|`insecure`; tự điền đường dẫn certificate theo slot `/flash` của broker (`ca0/cert0/key0.pem`) |

Mật khẩu không bao giờ in ra `show` — chỉ `(set)` hoặc `(empty)`.

**`mqtt-cfg` chỉ sửa RAM** (thông báo `RAM only, not persisted`). Để thay đổi có hiệu lực thật:

- `cfg-save` → xuống NVS (sống qua reboot), và
- `cfg-apply mqtt` → MQTT client đọc lại config và rebuild ngay, **không cần reboot**
  (hoặc reboot cũng được).

Ví dụ đầy đủ:
```
mqtt-cfg set --name "Local" --uri 192.168.1.10 --port 1883
mqtt-cfg enable
mqtt-cfg period --period 5
cfg-save
cfg-apply mqtt
```

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
| `network_comm` | bootstrap mạng (gọi `ethernet_driver_init`) | ít |
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

## 7. `ping` — kiểm tra kết nối ICMP từ chính thiết bị

```
ping <ip> [--count <1..100>] [--interval <ms>] [--timeout <ms>]
```

| Tham số | Mặc định | Ý nghĩa |
|---|---|---|
| `<ip>` | — | **địa chỉ IP số** (IPv4/IPv6). Không có phân giải tên miền — muốn test DNS thì trỏ tới thẳng IP |
| `--count` | 4 | số gói gửi, 1..100 |
| `--interval` | 1000 | nhịp gửi tính bằng ms (tối thiểu 100) |
| `--timeout` | 2000 | thời gian chờ trả lời cho mỗi gói, ms (tối thiểu 100) |

Lệnh chạy **block console** tới khi hết vòng ping rồi in tổng kết; trong lúc đó lệnh khác không gõ được.

```
ping 192.168.1.1
PING 192.168.1.1 64 data bytes, interval=1000ms timeout=2000ms
64 bytes from 192.168.1.1: icmp_seq=1 ttl=64 time=2 ms
64 bytes from 192.168.1.1: icmp_seq=2 ttl=64 time=1 ms
--- 192.168.1.1 ping statistics ---
2 packets transmitted, 2 received, 0% packet loss
rtt min/avg/max = 1/1/2 ms
```

| Hiện tượng | Nguyên nhân |
|---|---|
| `is not a numeric IP address` | gõ hostname — `ping` không resolve DNS |
| mọi gói `timed out`, `100% packet loss` | ETH/WiFi chưa lên hoặc sai mạng; kiểm tra `net-cfg show` + log `net_mgr` |
| `did not end in time; stopping` | phiên ping bất thường (lệnh đã chờ quá `count*(interval+timeout) + 3s`); vẫn in tổng kết tới thời điểm đó |

Mã thoát: `0` nếu nhận được ít nhất một gói trả lời, `1` nếu mất toàn bộ.

---

## 8. `ref` — đọc từ reference meter và so sánh với DUT

```
ref list
ref read --id <slave_id> [--samples <N>] [--interval <ms>]
ref compare --id <slave_id> --phase <a|b|c> [--samples <N>] [--interval <ms>]
```

| Subcommand | Việc |
|---|---|
| `list` | liệt kê tất cả các slot Modbus đã cấu hình (slave ID, device type, name, device state) |
| `read --id <N>` | đọc công suất từ reference meter (hỗ trợ nhiều mẫu để tính trung bình) |
| `compare --id <N> --phase <a\|b\|c>` | **so sánh đồng thời** P_ref và P_meter, tự động tính error % |

**Tham số:**

| Cờ | Mặc định | Giới hạn | Ý nghĩa |
|---|---|---|---|
| `--samples <N>` | `read`: 1, `compare`: 5 | 1-50 | số lần lấy mẫu để tính trung bình |
| `--interval <ms>` | `read`: 100, `compare`: 200 | 1-1000 | khoảng thời gian giữa các mẫu (ms) |
| `--phase <a\|b\|c>` | — | — | pha DUT (chỉ dùng cho `compare`) |

**Device types được hỗ trợ:**
- **PM710** (Schneider): đọc active power từ register 1006 (float32, kW)
- **EM07K** (TENSE): đọc active power từ registers 4042/4043/4044 (U16 per phase, cần CTR×VTR scale)
- **UNKNOWN**: hiển thị nếu device type không khớp PM710 hoặc EM07K

### Ví dụ sử dụng

**1. Liệt kê reference meters:**
```bash
ref list
# Output:
# Slot   Slave ID   Type       Name                 Status
# 0      1          PM710      Main-Meter           ON
# 1      2          EM07K      Phase-Meter          OFF
```

Trạng thái device (tri-state):
- **ON** — thiết bị trả lời lần poll gần nhất
- **OFF** — master đang poll nhưng thiết bị mất kết nối (5 lần poll liên tiếp không ack)
- **INACTIVE** — master không poll (bus/slot bị tắt hoặc portal config đang bật)

**2. Đọc công suất từ reference meter (1 mẫu):**
```bash
ref read --id 1
# Output:
# Slave ID 1 - Active Power: 1234.567 W
#   Voltage: L1=230.0V L2=229.5V L3=230.2V
#   Current: L1=5.36A L2=5.41A L3=5.38A
#   ...
```

**3. Đọc công suất trung bình (nhiều mẫu):**
```bash
ref read --id 1 --samples 10 --interval 200
# Output:
# Measuring slave ID 1 active power (10 samples, 200 ms interval)...
# Slave ID 1 - Active Power: 1235.123 W (average of 10 samples)
#   Voltage: L1=230.0V ...
```

**4. So sánh đồng thời P_ref và P_meter (workflow calibration):**
```bash
ref compare --id 1 --phase a --samples 10 --interval 200
# Output:
# Comparing phase A: DUT vs Reference meter (slave ID 1)
# Sampling: 10 samples, 200 ms interval
# --------------------------------------------------
# Sample  1: P_ref=1220.000 W, P_meter=1234.567 W
# Sample  2: P_ref=1221.100 W, P_meter=1235.234 W
# ...
# Sample 10: P_ref=1219.800 W, P_meter=1233.890 W
# --------------------------------------------------
# Results:
#   P_ref average:   1220.450 W
#   P_meter average: 1234.678 W
#   Power error:     1.166%
#
# Use this error value for phase calibration:
#   meter-cal phi-err --phase a --error 1.166
```

### Workflow calibration phase 2-step

Khi **không thể cấp nguồn đồng thời** cho DUT và reference meter, dùng workflow 2-step:

#### **STEP 1: Đo sai số công suất tại PF=1 (baseline)**

Cấp nguồn PF=1 (tải thuần trở) vào cả DUT và reference meter:

```bash
# Cách 1: Đọc thủ công và tính error bằng tay
meter-cal get-p --phase a --samples 10 --interval 200    # P_DUT = 1234.678 W
ref read --id 1 --samples 10 --interval 200               # P_ref = 1220.450 W
# Tính: error = (1234.678 - 1220.450) / 1220.450 * 100 = 1.166%

# Cách 2: Dùng ref compare (TỰ ĐỘNG tính error)
ref compare --id 1 --phase a --samples 10 --interval 200
# Output trực tiếp: Power error: 1.166%
```

**Lưu lại giá trị error này** (ví dụ: 1.166%) để dùng cho Step 2.

#### **STEP 2: Calibrate phase tại PF=0.5L**

Bây giờ **chỉ cấp nguồn PF=0.5L** (xung lệch pha 60°) vào DUT (không cần reference meter):

```bash
# 1. Reset Phi về 0 (baseline requirement)
meter-cal default --field phi --phase a --apply

# 2. Auto calibrate phase với known error từ Step 1
meter-cal phi-err --phase a --error 1.166

# 3. Verify và lưu
meter-cal save
```

Command `phi-err` sẽ:
- Đo P_chip hiện tại từ DUT
- Tính ngược P_ref = P_chip / (1 + error/100)
- Chạy thuật toán phase calibration như `auto-phi`
- Ghi Phi vào chip và verify residual error

---

## 9. `ota` — cập nhật firmware từ GitHub Releases

```
ota <status|check|update|confirm|rollback> [--url <URL>]
```

| Subcommand | Việc |
|---|---|
| `status` | version đang chạy, trạng thái worker, %, lỗi, `probation`, release mới nhất từ lần check gần nhất |
| `check` | tải `manifest.json` và so version — không chặn console |
| `update` | cài bản từ lần `check` gần nhất; `--url` để chỉ thẳng một `.bin` |
| `confirm` | commit ảnh đang chạy ngay, bỏ qua cửa sổ self-test |
| `rollback` | quay về slot cũ và reboot |

`check` và `update` trả về ngay khi worker task đã được tạo; theo dõi tiến trình bằng
`ota status` hoặc xem log. Cài xong máy **tự reboot**.

```
ota check
ota status
ota update
ota update --url https://github.com/gnuh202/Thesis_IoTMeter_FW/releases/download/v1.0.1/luanvan_firmware.bin
```

Dòng `probation` trong `ota status` cho biết ảnh đang chạy đã commit hay chưa:
`yes (not committed)` nghĩa là lần reset kế tiếp sẽ quay về ảnh cũ nếu self-test không
đạt. Chi tiết điều kiện self-test và quy trình phát hành: [ota_release.md](ota_release.md).

Trả về `OTA is disabled in this build` nếu `CONFIG_APP_OTA_ENABLE=n`.

---

## Ghi chú áp dụng cấu hình

| Lệnh | Áp dụng khi |
|---|---|
| `net-cfg sta` | ngay (đẩy vào driver) — dùng ở failover kế tiếp |
| `net-cfg ap` | ngay |
| `mqtt-cfg *` | RAM only; `cfg-save` để xuống NVS, `cfg-apply mqtt` để chạy ngay không reboot |
| `cfg-save` | ghi NVS toàn bộ snapshot RAM (không tự áp dụng gì) |
| `cfg-apply [domain]` | áp domain vào runtime; `mqtt` rebuild client bất đồng bộ |
| `meter-cal save` | ghi NVS ngay; `apply` áp xuống chip ngay |
| `log` | ngay (chỉ hiệu lực tới khi reboot — không lưu NVS) |

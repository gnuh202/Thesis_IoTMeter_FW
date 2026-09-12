# Reference Manual
## IoT 3-Phase Power Meter (ESP32-S3 + ATM90E32AS)

**Phiên bản firmware**: v1.00 (build mới nhất trong repo)
**Giao thức**: Modbus RTU Slave, RS485 half-duplex
**Tài liệu này**: tham chiếu giao tiếp Modbus + vận hành thiết bị qua LCD. Đã đối chiếu với code `modbus_slave_task.c`, `home_screen.c` và `config_manager_t`. Chi tiết bảng thanh ghi được giữ đồng bộ với `docs/modbus_slave_register_map.md`.

---

## Mục lục

- [Phần 1 — Giới thiệu](#phần-1--giới-thiệu)
- [Phần 2 — An toàn & lưu ý vận hành](#phần-2--an-toàn--lưu-ý-vận-hành)
- [Phần 3 — Vận hành cơ bản (LCD)](#phần-3--vận-hành-cơ-bản-lcd)
- [Phần 4 — Thông số dùng chung](#phần-4--thông-số-dùng-chung)
- [Phần 5 — Thông số đặc biệt](#phần-5--thông-số-đặc-biệt)
- [Phụ lục A — Đặc tính kỹ thuật](#phụ-lục-a--đặc-tính-kỹ-thuật)
- [Phụ lục B — Bảng thanh ghi Modbus](#phụ-lục-b--bảng-thanh-ghi-modbus)
- [Phụ lục C — Xử lý sự cố](#phụ-lục-c--xử-lý-sự-cố)
- [Phụ lục D — Ví dụ frame](#phụ-lục-d--ví-dụ-frame)
- [Phụ lục E — Tham chiếu mã nguồn](#phụ-lục-e--tham-chiếu-mã-nguồn)

---

## Phần 1 — Giới thiệu

Thiết bị là **đồng hồ đo điện 3 pha** dựa trên ESP32-S3 + chip ATM90E32AS, giao tiếp upstream qua Modbus RTU trên cổng RS485.

**Vai trò Modbus**: thiết bị là **slave** — luôn lắng nghe bus và trả lời master. Slave luôn **on**; không có lệnh bật/tắt slave qua Modbus.

**Hình thức vật lý**:

| Thành phần | Mô tả |
|------------|-------|
| RS485 COM (UART1) | RXD = GPIO13, TXD = GPIO14, **auto-direction transceiver** (không có chân DE/RE trên ESP32) |
| Đầu vào đo lường | 3 kênh điện áp L1/L2/L3, 3 kênh dòng L1/L2/L3 + dây N, qua biến áp/biến dòng ngoài |
| Nguồn cấp | ESP32-S3 DevKit-C (USB-C 5V) |
| Ngõ ra số | 2 kênh relay qua PCF8574 (P7, P6) |
| Ngõ vào số | 2 kênh qua PCF8574 (P5, P2) |
| Hiển thị | LCD 20×4 (điều khiển qua HMI BSP) |
| Lưu trữ | NVS (cấu hình), thẻ SD (log/calib) |

---

## Phần 2 — An toàn & lưu ý vận hành

### 2.1. Lưu ý phần cứng

- **KHÔNG** cấp điện áp lưới khi chưa đấu nối đầy đủ CT/PT. CT hở mạch thứ cấp sẽ phát áp cao nguy hiểm.
- **KHÔNG** dùng Diode/Megger test trên thiết bị khi đang cấp nguồn — sẽ chết ESP32 + ATM90.
- Chỉ nhân viên điện có chuyên môn được can thiệp phần cứng.

### 2.2. Lưu ý phần mềm

- Slave address 1..247. Đặt ngoài khoảng này slave sẽ **không trả lời** master.
- Khi đổi baud rate, **master phải đổi baud trước khi gửi frame kế tiếp**. Khoảng "mất kết nối tạm thời" ~50–200 ms là bình thường.
- Khi ghi `RebootDevice` (magic `0x5AA5`), thiết bị reboot sau 200 ms. Không gửi frame trong khoảng này.
- Sau khi cập nhật firmware có đổi **cấu trúc NVS**, xóa flash bằng `idf.py -p COMx erase_flash` rồi flash lại.

### 2.3. Quy tắc giao thức

- **Framing**: 8N1 cố định. Firmware **không hỗ trợ** parity/stop-bit khác trên slave hiện tại.
- **CRC**: CRC-16/Modbus (poly 0xA001, init 0xFFFF) — đúng theo Modbus RTU spec. Không dùng CRC-16/CCITT-FALSE.
- **Byte order**: big-endian (MSB trước) cho mọi giá trị nhiều byte. Float 32-bit = 2 register, high-word first.

---

## Phần 3 — Vận hành cơ bản (LCD)

### 3.1. Nút bấm

LCD 20×4, 4 nút chức năng:

| Vị trí | Tên nút | Vai trò trong menu |
|--------|---------|---------------------|
| Trên cùng | TOP | Chuyển menu lên / Tăng giá trị đang chỉnh (giữ = lặp nhanh) |
| Dưới cùng | BOTTOM | Chuyển menu xuống / Giảm giá trị đang chỉnh (giữ = lặp nhanh) |
| Trái | LEFT | Back về menu trước (cũng đóng mọi Info screen) |
| Phải / Giữa | CENTER | Vào menu con / Xác nhận giá trị đã chỉnh / OK trên Info |

### 3.2. Cấu trúc menu chính

```
MAIN
├── Metering      (đo lường realtime, không thay đổi được)
├── Energy        (năng lượng tích lũy, có thể reset)
├── Demand        (công suất nhu cầu)
├── Settings
│   ├── Network          (Ethernet + WiFi)
│   ├── MQTT             (broker, credentials)
│   ├── Modbus
│   │   ├── RTU Master   (polling downstream meters)
│   │   └── RTU Slave    ← thiết bị này
│   ├── Display          (LCD backlight, sleep)
│   └── Calibration      (console only)
├── Diagnostics   (trạng thái từng module)
└── Reboot
```

### 3.3. Đổi slave address / baud rate

Đường dẫn: `MAIN → Settings → Modbus → RTU Slave`.

```
RTU SLAVE
├── Info       (xem thông tin: ID, baud, "Always on")
├── ID         (chỉnh 1..247, TOP/BOTTOM tăng/giảm, CENTER lưu, LEFT back)
├── Baud       (chỉnh 9600/19200/38400/57600/115200, TOP/BOTTOM tăng/giảm, CENTER lưu)
└── Back
```

**Quy trình khuyến nghị khi đổi baud**:

1. Đảm bảo master đã biết baud mới.
2. Trên LCD, vào `Baud`, chọn baud mới, CENTER để lưu.
3. Thiết bị rebuild RTU stack ở baud mới (~50–200 ms).
4. Master dùng baud mới để tiếp tục giao tiếp.

**Quy trình khuyến nghị khi đổi slave address**:

1. Master cập nhật địa chỉ mới trước.
2. Trên LCD, vào `ID`, chọn ID mới, CENTER để lưu.
3. Thiết bị rebuild RTU stack với ID mới.

### 3.4. Reset dữ liệu

| Mục đích | Cách |
|----------|------|
| Reset energy | Ghi `HR_RESET_ENERGY = 1` qua Modbus, hoặc qua console `energy_reset` |
| Reset demand | Ghi `HR_RESET_DEMAND = 1` qua Modbus, hoặc qua console `demand_reset` |
| Reboot | Ghi `HR_REBOOT = 0x5AA5` qua Modbus, hoặc LCD `Reboot` |

### 3.5. Xem thông tin thiết bị

LCD `MAIN → Diagnostics` cho thấy trạng thái từng module (ATM90, RS485 Master, RS485 Slave, Ethernet, WiFi, MQTT, SD, DI, DO) — enum `UNKNOWN/INIT/READY/WARNING/ERROR/OFFLINE`.

---

## Phần 4 — Thông số dùng chung

Các thông số mọi Modbus master cần biết để giao tiếp thành công.

### 4.1. Communication settings (RS485)

| Thông số       | Giá trị                              | Cấu hình qua              |
|----------------|--------------------------------------|---------------------------|
| Slave address  | 1..247 (mặc định **10**)            | LCD / Config Manager      |
| Baud rate      | 9600 / 19200 / 38400 / 57600 / 115200 (mặc định **9600**) | LCD / Config Manager |
| Data bits      | 8 (cố định)                          | -                         |
| Parity         | None (cố định)                       | -                         |
| Stop bits      | 1 (cố định)                          | -                         |
| Framing        | 8N1                                  | -                         |
| CRC            | CRC-16/Modbus (poly 0xA001)          | -                         |
| Byte order     | Big-endian                           | -                         |
| Inter-frame    | 3.5 char theo baud hiện tại          | tự động                   |
| UART port      | UART1                                | Kconfig (không đổi runtime) |
| RXD / TXD      | GPIO13 / GPIO14                       | Kconfig (không đổi runtime) |

> **Lưu ý**: Slave address, baud rate và parity chỉ đổi được qua LCD/Config Manager. Sau khi đổi phải reboot để áp dụng. Modbus không có thanh ghi cấu hình truyền thông.

### 4.2. Đơn vị đo lường

| Đại lượng        | Đơn vị   | Format trong register          |
|------------------|----------|--------------------------------|
| Điện áp          | V        | float32, 2 register high-first |
| Dòng điện        | A        | float32, 2 register high-first |
| Công suất tác dụng | W      | float32, 2 register high-first |
| Công suất phản kháng | var  | float32, 2 register high-first |
| Công suất biểu kiến | VA    | float32, 2 register high-first |
| Hệ số công suất  | -1..+1   | float32, 2 register high-first |
| Tần số           | Hz       | float32, 2 register high-first |
| Nhiệt độ         | °C       | float32, 2 register high-first |
| Góc pha          | độ       | float32, 2 register high-first |
| Năng lượng tác dụng | kWh   | float32, 2 register high-first |
| Năng lượng phản kháng | kvarh | float32, 2 register high-first |
| Demand           | W        | float32, 2 register high-first |

### 4.3. Quy ước địa chỉ

Tài liệu dùng **protocol address (0-based)**. Tool master nào dùng dạng 3xxxx/4xxxx thì **cộng 30000/40000** vào.

| Vùng            | Protocol (0-based) | Reference (1-based) | Số thanh ghi dùng |
|-----------------|---------------------|---------------------|--------------------|
| Coil            | 0..1                | 00001..00002        | 2 (bit-packed)     |
| Discrete Input  | 0..1                | 10001..10002        | 2 (bit-packed)     |
| Input Register  | 0..109              | 30001..30110        | 110                |
| Holding Register | 0..7               | 40001..40008        | 8                  |

### 4.4. Hàm Modbus hỗ trợ

| Function Code | Tên              | Dùng cho                  |
|---------------|------------------|---------------------------|
| FC 01         | Read Coils       | Đọc relay ngõ ra          |
| FC 02         | Read Discrete Inputs | Đọc ngõ vào số       |
| FC 03         | Read Holding Registers | Đọc cấu hình       |
| FC 04         | Read Input Registers  | Đọc đo lường         |
| FC 05         | Write Single Coil | Bật/tắt 1 relay          |
| FC 06         | Write Single Register | Đổi 1 thanh ghi HR    |
| FC 15         | Write Multiple Coils | Bật/tắt nhiều relay    |
| FC 16         | Write Multiple Registers | Đổi nhiều HR       |

---

## Phần 5 — Thông số đặc biệt

Những thanh ghi / chức năng không nằm trong "communication cơ bản", cần đọc kỹ trước khi dùng.

### 5.1. Đổi cấu hình truyền thông

Slave address, baud rate và parity **không thể đổi qua Modbus**. Chúng chỉ được cấu hình qua LCD menu hoặc Config Manager, lưu xuống NVS, và có hiệu lực sau lần reboot tiếp theo.

Quy trình vận hành:

1. Vào LCD menu **Communication Settings**.
2. Chọn giá trị Slave Address (1..247), Baud Rate (9600/19200/38400/57600/115200) hoặc Parity.
3. Xác nhận lưu → firmware reboot.
4. Sau reboot, master dùng thông số mới để giao tiếp.

> **Lưu ý**: Nếu quên địa chỉ hoặc baud, có thể dùng console UART0 để kiểm tra lại.

### 5.2. Reset / Reboot qua Modbus

| Tác vụ              | Holding reg | Giá trị ghi         | Hành vi |
|---------------------|-------------|---------------------|---------|
| Reset energy        | `HR_RESET_ENERGY`   (HR 3) | `1`         | Reset 4 energy accumulators về 0, tự xóa về 0 |
| Reset demand        | `HR_RESET_DEMAND`   (HR 4) | `1`         | Reset `ActivePowerDemand` + `_Max` về 0, tự xóa về 0 |
| Reboot              | `HR_REBOOT`         (HR 7) | **`0x5AA5`** | Reboot sau 200 ms (chỉ chấp nhận magic này) |

### 5.3. Wiring mode

`HR_WIRING_MODE` (HR 0): `0` = 3P4W (mặc định), `1` = 3P3W.

Ghi qua Modbus **áp dụng ngay** vào chip ATM90. Giá trị khác 0/1 bị từ chối.

### 5.4. Line frequency

`HR_LINE_FREQ_SEL` (HR 1): `0` = 50 Hz, `1` = 60 Hz.

Ghi qua Modbus **áp dụng ngay** vào chip ATM90. Giá trị khác 0/1 bị từ chối.

### 5.5. Demand window

`HR_DEMAND_WINDOW` (HR 2): cửa sổ trung bình trượt demand, đơn vị phút, hợp lệ **1..60**, mặc định **15**.

Ghi giá trị mới qua FC 06 — áp dụng ngay, không cần `ApplyConfig`. Giá trị `0` bị từ chối.

### 5.6. Identification (FC 04, RO)

| Addr (protocol) | Tên               | Kiểu   | Giá trị hiện tại |
|------------------|-------------------|--------|------------------|
| 100              | `DeviceID`         | uint16 | `0x9032`         |
| 101              | `FirmwareVersion`  | uint16 | `0x0100` (v1.00) |
| 102              | `HardwareVersion`  | uint16 | `0x0100`         |
| 103              | `MeasureValid`     | uint16 | `0` = chưa có số đo, `1` = đã có ít nhất 1 lần cập nhật |
| 104..105         | `Uptime`           | uint32 | `(reg[104] << 16) \| reg[105]` = giây kể từ boot |

### 5.7. Lỗi Modbus

| Mã FC  | Tên                  | Mã lỗi | Ý nghĩa |
|--------|----------------------|--------|----------|
| 01     | ILLEGAL_FUNCTION     | 0x01   | FC không hỗ trợ |
| 02     | ILLEGAL_DATA_ADDRESS | 0x02   | Địa chỉ thanh ghi ngoài phạm vi |
| 03     | ILLEGAL_DATA_VALUE   | 0x03   | Giá trị ghi không hợp lệ |
| 04     | SLAVE_DEVICE_FAILURE | 0x04   | Lỗi nội bộ (rất hiếm) |

**Cảnh báo đặc biệt**:

- Ghi giá trị ngoài range cho `HR_WIRING_MODE` (khác 0/1), `HR_LINE_FREQ_SEL` (khác 0/1), hoặc `HR_DEMAND_WINDOW` (bằng 0) → bị từ chối (`ILLEGAL_DATA_VALUE`).
- Ghi `HR_REBOOT` với giá trị ≠ 0x5AA5 → bị từ chối, thanh ghi xóa về 0, `LastResult` báo failed.
- Đọc register ngoài phạm vi (IR >= 110, HR >= 8, coil >= 2, DI >= 2) → `ILLEGAL_DATA_ADDRESS`.

### 5.8. Trạng thái module (system status enum)

Các thanh ghi 56..59 (SysStatus/MeterStatus của ATM90) và trạng thái các module hiển thị qua LCD dùng chung enum:

| Giá trị | Tên         | Ý nghĩa                       |
|---------|-------------|-------------------------------|
| 0       | UNKNOWN     | Chưa khởi tạo                 |
| 1       | INIT        | Đang khởi tạo                 |
| 2       | READY       | Hoạt động bình thường         |
| 3       | WARNING     | Hoạt động nhưng có cảnh báo   |
| 4       | ERROR       | Lỗi nghiêm trọng             |
| 5       | OFFLINE     | Tính năng đang tắt            |

---

## Phụ lục A — Đặc tính kỹ thuật

### A.1. Đặc tính đo (Metering characteristics)

| Đại lượng | Khoảng đo / Đặc tính |
|-----------|----------------------|
| Điện áp L-N / L-L | Tùy thuộc biến áp ngoài (PT ratio) |
| Dòng điện | Tùy thuộc biến dòng ngoài (CT ratio) |
| Tần số | 45–65 Hz (auto-detect) |
| True RMS | Có, lấy mẫu 32/cycle |
| Công suất tác dụng / phản kháng / biểu kiến | Tính theo từng pha + tổng |
| Hệ số công suất | -1..+1 (dấu = hướng dòng năng lượng) |
| Góc pha | 0..360° |
| Năng lượng | Tích lũy trong firmware (chip registers read-to-clear) |
| Demand | Trung bình trượt, cửa sổ 1..60 phút, mặc định 15 |
| Current peak | Đọc từ `IPeakA/B/C`, đơn vị A |
| Nhiệt độ | Cảm biến ATM90, °C |

### A.2. Truyền thông

| Mục | Giá trị |
|-----|---------|
| Chuẩn vật lý | RS485 half-duplex, auto-direction transceiver |
| UART | UART1 (GPIO13/14) |
| Protocol | Modbus RTU Slave |
| Baud | 9600 / 19200 / 38400 / 57600 / 115200 |
| Framing | 8N1 |
| CRC | CRC-16/Modbus |
| Số slave tối đa trên bus | 247 (địa chỉ 1..247) |

### A.3. Phần cứng

| Mục | Giá trị |
|-----|---------|
| MCU | ESP32-S3 |
| Metering chip | ATM90E32AS |
| I/O expander | PCF8574 (relay + DI) |
| Hiển thị | LCD 20×4 |
| Nguồn | 5V qua USB-C |
| Lưu trữ | NVS + thẻ SD (optional) |

---

## Phụ lục B — Bảng thanh ghi Modbus

Tất cả địa chỉ dưới đây là **protocol (0-based)**.

**Chú thích cột chung**:

- **Addr**: protocol address (0-based). Reference = Addr + 30000 (IR) hoặc + 40000 (HR) hoặc + 1 cho coil/DI.
- **Size**: số thanh ghi 16-bit.
- **Type**: `uint16`, `int16`, `float32` (2 reg), `bool` (1 bit trong packed byte).
- **R/W**: `R` = Read Only, `RW` = Read/Write.
- **NV**: `Y` = lưu NVS, `N` = chỉ RAM, `—` = không áp dụng.
- **Scale**: hệ số nhân — `1` nghĩa là thanh ghi = giá trị thực, `0.1` nghĩa là thanh ghi × 0.1 = giá trị thực, v.v. Float: xem IEEE-754.
- **Units**: đơn vị vật lý.
- **Range**: khoảng giá trị hợp lệ.

### B.1. Discrete Inputs (FC 02) — Ngõ vào số

| Addr | Name    | Size | Type  | R/W | Scale | Units | Range | Description |
|------|---------|------|-------|-----|-------|-------|-------|-------------|
| 0    | Input0  | 1 bit| bool  | R   | 1     | -     | 0..1  | Ngõ vào số kênh 0 (PCF8574 P5) |
| 1    | Input1  | 1 bit| bool  | R   | 1     | -     | 0..1  | Ngõ vào số kênh 1 (PCF8574 P2) |
| 2..7 | (rsvd)  | -    | -     | -   | -     | -     | -     | Luôn đọc 0 |

### B.2. Coils (FC 01 / 05 / 15) — Ngõ ra Relay

| Addr | Name    | Size | Type  | R/W | Scale | Units | Range | Description |
|------|---------|------|-------|-----|-------|-------|-------|-------------|
| 0    | Output0 | 1 bit| bool  | RW  | 1     | -     | 0..1  | Relay kênh 0 (PCF8574 P7). Đọc lại = cache lần ghi gần nhất |
| 1    | Output1 | 1 bit| bool  | RW  | 1     | -     | 0..1  | Relay kênh 1 (PCF8574 P6). Đọc lại = cache lần ghi gần nhất |
| 2..7 | (rsvd)  | -    | -     | -   | -     | -     | -     | Ghi bị bỏ qua |

### B.3. Input Registers — Measurement & Energy (FC 04)

#### B.3.1. Điện áp (V) — float32

| Addr | Name      | Size | Type    | R/W | Scale | Units | Range (typ.) | Description |
|------|-----------|------|---------|-----|-------|-------|--------------|-------------|
| 0    | Voltage_A | 2    | float32 | R   | -     | V     | 0..500       | Điện áp pha L1 |
| 2    | Voltage_B | 2    | float32 | R   | -     | V     | 0..500       | Điện áp pha L2 |
| 4    | Voltage_C | 2    | float32 | R   | -     | V     | 0..500       | Điện áp pha L3 |

#### B.3.2. Dòng điện (A) — float32

| Addr | Name      | Size | Type    | R/W | Scale | Units | Range (typ.) | Description |
|------|-----------|------|---------|-----|-------|-------|--------------|-------------|
| 6    | Current_A | 2    | float32 | R   | -     | A     | 0..1000      | Dòng điện pha L1 |
| 8    | Current_B | 2    | float32 | R   | -     | A     | 0..1000      | Dòng điện pha L2 |
| 10   | Current_C | 2    | float32 | R   | -     | A     | 0..1000      | Dòng điện pha L3 |
| 12   | Current_N | 2    | float32 | R   | -     | A     | 0..1000      | Dòng điện dây trung tính |

#### B.3.3. Công suất tác dụng (W) — float32

| Addr | Name               | Size | Type    | R/W | Scale | Units | Range (typ.) | Description |
|------|--------------------|------|---------|-----|-------|-------|--------------|-------------|
| 14   | ActivePower_A      | 2    | float32 | R   | -     | W     | ±10000       | P pha L1 |
| 16   | ActivePower_B      | 2    | float32 | R   | -     | W     | ±10000       | P pha L2 |
| 18   | ActivePower_C      | 2    | float32 | R   | -     | W     | ±10000       | P pha L3 |
| 20   | ActivePower_Total  | 2    | float32 | R   | -     | W     | ±30000       | Tổng P |

#### B.3.4. Công suất phản kháng (var) — float32

| Addr | Name                 | Size | Type    | R/W | Scale | Units | Description |
|------|----------------------|------|---------|-----|-------|-------|-------------|
| 22   | ReactivePower_A      | 2    | float32 | R   | -     | var   | Q pha L1 |
| 24   | ReactivePower_B      | 2    | float32 | R   | -     | var   | Q pha L2 |
| 26   | ReactivePower_C      | 2    | float32 | R   | -     | var   | Q pha L3 |
| 28   | ReactivePower_Total  | 2    | float32 | R   | -     | var   | Tổng Q |

#### B.3.5. Công suất biểu kiến (VA) — float32

| Addr | Name                 | Size | Type    | R/W | Scale | Units | Description |
|------|----------------------|------|---------|-----|-------|-------|-------------|
| 30   | ApparentPower_A      | 2    | float32 | R   | -     | VA    | S pha L1 |
| 32   | ApparentPower_B      | 2    | float32 | R   | -     | VA    | S pha L2 |
| 34   | ApparentPower_C      | 2    | float32 | R   | -     | VA    | S pha L3 |
| 36   | ApparentPower_Total  | 2    | float32 | R   | -     | VA    | Tổng S |

#### B.3.6. Hệ số công suất — float32

| Addr | Name             | Size | Type    | R/W | Scale | Units | Range | Description |
|------|------------------|------|---------|-----|-------|-------|-------|-------------|
| 38   | PowerFactor_A    | 2    | float32 | R   | -     | -     | -1..+1 | PF pha L1 |
| 40   | PowerFactor_B    | 2    | float32 | R   | -     | -     | -1..+1 | PF pha L2 |
| 42   | PowerFactor_C    | 2    | float32 | R   | -     | -     | -1..+1 | PF pha L3 |
| 44   | PowerFactor_Total| 2    | float32 | R   | -     | -     | -1..+1 | PF tổng |

#### B.3.7. Góc pha (độ) — float32

| Addr | Name         | Size | Type    | R/W | Scale | Units | Description |
|------|--------------|------|---------|-----|-------|-------|-------------|
| 46   | PhaseAngle_A | 2    | float32 | R   | -     | °     | Góc pha L1 |
| 48   | PhaseAngle_B | 2    | float32 | R   | -     | °     | Góc pha L2 |
| 50   | PhaseAngle_C | 2    | float32 | R   | -     | °     | Góc pha L3 |

#### B.3.8. Tần số & Nhiệt độ — float32

| Addr | Name        | Size | Type    | R/W | Scale | Units | Range | Description |
|------|-------------|------|---------|-----|-------|-------|-------|-------------|
| 52   | Frequency   | 2    | float32 | R   | -     | Hz    | 45..65 | Tần số lưới |
| 54   | Temperature | 2    | float32 | R   | -     | °C    | -40..125 | Nhiệt độ ATM90 |

#### B.3.9. Trạng thái ATM90 (raw uint16)

| Addr | Name          | Size | Type   | R/W | Scale | Units | Description |
|------|---------------|------|--------|-----|-------|-------|-------------|
| 56   | SysStatus0    | 1    | uint16 | R   | 1     | -     | EMMState0 |
| 57   | SysStatus1    | 1    | uint16 | R   | 1     | -     | EMMState1 |
| 58   | MeterStatus0  | 1    | uint16 | R   | 1     | -     | EMMIntState0 |
| 59   | MeterStatus1  | 1    | uint16 | R   | 1     | -     | EMMIntState1 |

Bit mapping: xem datasheet ATM90E32AS §EMMState.

#### B.3.10. Energy accumulators — float32

| Addr | Name                   | Size | Type    | R/W | Scale | Units | Description |
|------|------------------------|------|---------|-----|-------|-------|-------------|
| 60   | ActiveEnergy_Import    | 2    | float32 | R   | -     | kWh   | Điện năng tác dụng nhập (tích lũy) |
| 62   | ActiveEnergy_Export    | 2    | float32 | R   | -     | kWh   | Điện năng tác dụng xuất (tích lũy) |
| 64   | ReactiveEnergy_Import  | 2    | float32 | R   | -     | kvarh | Điện năng phản kháng nhập (tích lũy) |
| 66   | ReactiveEnergy_Export  | 2    | float32 | R   | -     | kvarh | Điện năng phản kháng xuất (tích lũy) |

Xó về 0: ghi `HR_RESET_ENERGY = 1`.

#### B.3.11. Current peak (per phase) — float32, A

| Addr | Name          | Size | Type    | R/W | Scale | Units | Description |
|------|---------------|------|---------|-----|-------|-------|-------------|
| 68   | CurrentPeak_A | 2    | float32 | R   | -     | A     | Dòng đỉnh pha L1 |
| 70   | CurrentPeak_B | 2    | float32 | R   | -     | A     | Dòng đỉnh pha L2 |
| 72   | CurrentPeak_C | 2    | float32 | R   | -     | A     | Dòng đỉnh pha L3 |

#### B.3.12. Demand — float32, W

| Addr | Name                    | Size | Type    | R/W | Scale | Units | Description |
|------|-------------------------|------|---------|-----|-------|-------|-------------|
| 74   | ActivePowerDemand       | 2    | float32 | R   | -     | W     | Trung bình trượt P_total (cửa sổ 15 phút mặc định) |
| 76   | ActivePowerDemand_Max   | 2    | float32 | R   | -     | W     | Giá trị max kể từ reset gần nhất |

#### B.3.13. Identification & Diagnostics — uint16

| Addr | Name             | Size | Type   | R/W | Scale | Units | Range | Description |
|------|------------------|------|--------|-----|-------|-------|-------|-------------|
| 100  | DeviceID         | 1    | uint16 | R   | 1     | -     | 0x9032  | Mã thiết bị |
| 101  | FirmwareVersion  | 1    | uint16 | R   | 1     | -     | 0x0100  | v1.00 |
| 102  | HardwareVersion  | 1    | uint16 | R   | 1     | -     | 0x0100  | v1.00 |
| 103  | MeasureValid     | 1    | uint16 | R   | 1     | -     | 0..1    | 1 = đã có số đo hợp lệ |
| 104  | Uptime_H         | 1    | uint16 | R   | 1     | s     | -       | Uptime, word cao |
| 105  | Uptime_L         | 1    | uint16 | R   | 1     | s     | -       | Uptime, word thấp |
| 106  | CommandSequence  | 1    | uint16 | R   | 1     | -     | -       | Tăng 1 mỗi khi có command/action được xử lý |
| 107  | CommandAddress   | 1    | uint16 | R   | 1     | -     | -       | Địa chỉ thanh ghi/action vừa xử lý |
| 108  | CommandError     | 1    | uint16 | R   | 1     | -     | -       | 0 = success; nếu lỗi, giá trị = -result (ESP-IDF error code dương) |
| 109  | CommandStatus    | 1    | uint16 | R   | 1     | -     | 1..3    | 1=pending, 2=success, 3=failed |

`Uptime` (giây) = `(reg[104] << 16) | reg[105]`.

### B.4. Holding Registers (FC 03 / 06 / 16)

| Addr | Name             | Size | Type   | R/W | NV | Scale | Units | Range | Default | Description |
|------|------------------|------|--------|-----|----|-------|-------|-------|---------|-------------|
| 0    | WiringMode       | 1    | uint16 | RW  | —  | 1     | -     | 0..1   | 0     | 0=3P4W, 1=3P3W; áp dụng ngay |
| 1    | LineFrequencySel | 1    | uint16 | RW  | —  | 1     | -     | 0..1   | 0     | 0=50Hz, 1=60Hz; áp dụng ngay |
| 2    | DemandWindowMin  | 1    | uint16 | RW  | N  | 1     | min   | 1..60  | 15    | Cửa sổ trung bình trượt demand; giá trị 0 bị từ chối |
| 3    | ResetEnergy      | 1    | uint16 | W   | N  | 1     | -     | 0..1   | 0     | One-shot: ghi khác 0 = reset 4 energy accumulators, tự xóa về 0 |
| 4    | ResetDemand      | 1    | uint16 | W   | N  | 1     | -     | 0..1   | 0     | One-shot: ghi khác 0 = reset demand + max demand, tự xóa về 0 |
| 5    | LastCommand      | 1    | uint16 | R   | —  | 1     | -     | -      | 0     | Địa chỉ thanh ghi action gần nhất được xử lý |
| 6    | LastResult       | 1    | uint16 | R   | —  | 1     | -     | 0..1   | 0     | 0=success, 1=failed |
| 7    | RebootDevice     | 1    | uint16 | W   | N  | 1     | -     | 0x5AA5 | -     | One-shot: ghi 0x5AA5 = reboot sau 200 ms; giá trị khác bị từ chối |

> **Lưu ý**: HR5 và HR6 là diagnostics chỉ đọc. Mọi ghi dải FC16 chạm HR5/HR6 đều bị từ chối.

### B.5. Tóm tắt số thanh ghi

| Vùng              | Khoảng    | Đã dùng | Dự phòng |
|-------------------|-----------|---------|----------|
| Coils             | 0..1      | 2       | 0        |
| Discrete Inputs   | 0..1      | 2       | 0        |
| Input Registers   | 0..109    | 110     | 0        |
| Holding Registers | 0..7      | 8       | 0        |

Tổng byte-level footprint:

- Coils: 2 bit, đóng gói 1 byte.
- Discrete Inputs: 2 bit, đóng gói 1 byte.
- Input Registers: 110 × 2 byte = 220 byte.
- Holding Registers: 8 × 2 byte = 16 byte.

Tổng = ~240 byte RAM cho backing arrays, xem `MB_*_REG_COUNT` trong [modbus_slave_task.c](../../main/app/modbus_slave_task.c).

---

## Phụ lục C — Xử lý sự cố

| Hiện tượng | Nguyên nhân có thể | Kiểm tra / Xử lý |
|------------|---------------------|---------------------|
| Master gửi frame nhưng không nhận phản hồi | Baud không khớp | Kiểm tra baud qua LCD Info menu |
| Master không nhận phản hồi | Address không khớp | So slave address, mặc định 10 |
| Phản hồi trả về exception 0x02 | Master đọc/ghi địa chỉ ngoài phạm vi | Kiểm tra theo Phụ lục B — tổng thanh ghi đã dùng |
| Phản hồi exception 0x03 | Master ghi giá trị không hợp lệ (ví dụ demand window = 0) | Chỉ ghi trong range cho phép |
| Sau khi đổi baud trên LCD, master mất kết nối | Master chưa đổi baud | Master phải đổi baud trước khi gửi frame kế tiếp |
| `ResetEnergy` / `ResetDemand` không tác động | Master ghi 0 | Phải ghi giá trị khác 0 |
| `RebootDevice` không reboot | Master ghi giá trị ≠ 0x5AA5 | Chỉ chấp nhận magic 0x5AA5 |
| Số đo không cập nhật | Wiring mode không khớp thực tế | LCD Meter Settings để chỉnh 3P3W/3P4W |
| Số đo = 0 | ATM90 chưa lock hoặc CT/PT sai hướng | Xem datasheet ATM90E32AS troubleshooting |
| Slave báo OFFLINE / ERROR trong Diagnostics | Module bị lỗi | Xem log qua console |

---

## Phụ lục D — Ví dụ frame

Mọi ví dụ dùng **slave id = 10 (0x0A)**, **baud 9600**, **8N1**.

### D.1. Đọc 4 thanh ghi điện áp từ addr 0 (FC 04)

**Request** (master → slave):

```
0A 04 00 00 00 04 [CRC-Lo] [CRC-Hi]
```

**Response** (slave → master):

```
0A 04 08 <8 byte float data> [CRC-Lo] [CRC-Hi]
```

Trong đó 8 byte là 4 thanh ghi float32 = Voltage_A + Voltage_B (mỗi cái 4 byte). High-word trước.

### D.2. Đọc WiringMode + LineFrequencySel (FC 03, từ addr 0)

**Request**:
```
0A 03 00 00 00 02 [CRC-Lo] [CRC-Hi]
```

**Response** (slave id=10, wiring=0, freq=0):
```
0A 03 04 00 00 00 00 [CRC-Lo] [CRC-Hi]
```

### D.3. Đổi demand window thành 30 phút (FC 06)

**Request**:
```
0A 06 00 02 00 1E [CRC-Lo] [CRC-Hi]
```

Response echo. Demand window mới có hiệu lực ngay.

### D.4. Reset energy (FC 06)

```
0A 06 00 03 00 01 [CRC-Lo] [CRC-Hi]
```

Response echo:
```
0A 06 00 03 00 01 [CRC-Lo] [CRC-Hi]
```

Sau frame này, các thanh ghi energy (60..67) đọc về 0.

### D.5. Reboot (FC 06 magic)

```
0A 06 00 07 5A A5 [CRC-Lo] [CRC-Hi]
```

Response echo. Sau ~200 ms, thiết bị reboot.

---

## Phụ lục E — Tham chiếu mã nguồn

| Phần tài liệu                | File                                                                  |
|------------------------------|-----------------------------------------------------------------------|
| Slave RTU stack              | [main/app/modbus_slave_task.c](../../main/app/modbus_slave_task.c)    |
| Master RTU stack             | [main/app/modbus_master_task.c](../../main/app/modbus_master_task.c)  |
| Kconfig (default addr/port)  | [main/Kconfig.projbuild](../../main/Kconfig.projbuild)               |
| LCD slave menu               | [main/app/home_screen.c](../../main/app/home_screen.c)                |
| Config manager               | [main/app/config_manager.h](../../main/app/config_manager.h)          |

**Tài liệu tham chiếu khác trong repo**:

- [docs/register_map.md](../register_map.md) — Data Dictionary (định nghĩa data point, không định nghĩa địa chỉ Modbus)
- [docs/modbus_slave_register_map.md](../modbus_slave_register_map.md) — bản đồ thanh ghi Modbus RTU Slave đang được sử dụng

**Phong cách tham khảo**: cấu trúc và layout bảng thanh ghi tham khảo Schneider Electric Power Meter 710 Reference Manual (63230-501-209A1) và EM-07 Modbus Register Table.
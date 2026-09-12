# Modbus RTU Slave — Register Map (FINAL)

Thiết bị ESP32-S3 đóng vai trò **Modbus RTU Slave** (đồng hồ đo 3 pha dựa trên ATM90E32AS), giao tiếp qua **RS485**.

> Trạng thái: **CHỐT** theo phản hồi. Có thể chỉnh sau nếu phát sinh.

---

## 1. Thông số truyền thông

| Mục            | Giá trị           | Ghi chú                                   |
| -------------- | ----------------- | ----------------------------------------- |
| Chuẩn vật lý   | RS485 half-duplex | hướng truyền tự động (không cần chân DE/RE) |
| UART           | UART1             | RX = GPIO13, TX = GPIO14                   |
| Slave address  | **10** (0x0A)     | mặc định từ Kconfig; đổi qua LCD (Settings > RTU Slave), reboot mới có hiệu lực |
| Baud rate      | **9600**          | mặc định; đổi qua LCD (Settings > RTU Slave), reboot mới có hiệu lực. **Độc lập với bus master** — mỗi link một baud riêng |
| Data bits      | 8                 |                                            |
| Parity         | None              | cố định — slave không có cài đặt parity    |
| Stop bits      | 1                 |                                            |
| Framing        | **8N1**           | không đổi được                             |

UART còn lại (RX=GPIO11, TX=GPIO12) dành cho **Modbus Master** (đọc công tơ
downstream qua web portal/LCD RTU Master) — hai link hoạt động độc lập.

---

## 2. Quy ước dữ liệu

- Byte order: **big-endian** (chuẩn Modbus).
- Float 32-bit (IEEE-754) = **2 register liên tiếp**, **high-word trước** (thứ tự ABCD).
- uint32 (energy/uptime): cũng 2 register, high-word trước.
- Đơn vị:
  - Điện áp: **V** — Dòng: **A**
  - P: **W** — Q: **var** — S: **VA**
  - Tần số: **Hz** — Nhiệt độ: **°C**
  - PF: không đơn vị (-1..1) — Góc pha: **độ**
  - Năng lượng tác dụng: **kWh** — phản kháng: **kvarh**
  - Demand: **W**

Địa chỉ dưới đây là **protocol address (0-based)**. Nếu tool của bạn dùng kiểu 3xxxx/4xxxx thì cộng offset tương ứng.

---

## 3. Input Registers (FC 04) — dữ liệu đo, read-only

### 3.1. Điện áp (V) — float

| Addr | Tên       |
| ---- | --------- |
| 0    | Voltage_A |
| 2    | Voltage_B |
| 4    | Voltage_C |

### 3.2. Dòng điện (A) — float

| Addr | Tên       |
| ---- | --------- |
| 6    | Current_A |
| 8    | Current_B |
| 10   | Current_C |
| 12   | Current_N |

### 3.3. Công suất tác dụng (W) — float

| Addr | Tên                |
| ---- | ------------------ |
| 14   | ActivePower_A      |
| 16   | ActivePower_B      |
| 18   | ActivePower_C      |
| 20   | ActivePower_Total  |

### 3.4. Công suất phản kháng (var) — float

| Addr | Tên                  |
| ---- | -------------------- |
| 22   | ReactivePower_A      |
| 24   | ReactivePower_B      |
| 26   | ReactivePower_C      |
| 28   | ReactivePower_Total  |

### 3.5. Công suất biểu kiến (VA) — float

| Addr | Tên                  |
| ---- | -------------------- |
| 30   | ApparentPower_A      |
| 32   | ApparentPower_B      |
| 34   | ApparentPower_C      |
| 36   | ApparentPower_Total  |

### 3.6. Hệ số công suất — float

| Addr | Tên               |
| ---- | ----------------- |
| 38   | PowerFactor_A     |
| 40   | PowerFactor_B     |
| 42   | PowerFactor_C     |
| 44   | PowerFactor_Total |

### 3.7. Góc pha (độ) — float

| Addr | Tên          |
| ---- | ------------ |
| 46   | PhaseAngle_A |
| 48   | PhaseAngle_B |
| 50   | PhaseAngle_C |

### 3.8. Hệ thống — float

| Addr | Tên         |
| ---- | ----------- |
| 52   | Frequency   |
| 54   | Temperature |

### 3.9. Trạng thái ATM90E32AS (uint16, raw)

| Addr | Tên          | Ghi chú      |
| ---- | ------------ | ------------ |
| 56   | SysStatus0   | EMMState0    |
| 57   | SysStatus1   | EMMState1    |
| 58   | MeterStatus0 | EMMIntState0 |
| 59   | MeterStatus1 | EMMIntState1 |

### 3.10. Năng lượng tích lũy — float

Tích lũy trong firmware (thanh ghi energy của chip là read-to-clear, được cộng dồn mỗi chu kỳ đọc).

| Addr | Tên                   | Đơn vị |
| ---- | --------------------- | ------ |
| 60   | ActiveEnergy_Import   | kWh    |
| 62   | ActiveEnergy_Export   | kWh    |
| 64   | ReactiveEnergy_Import | kvarh  |
| 66   | ReactiveEnergy_Export | kvarh  |

### 3.11. Đỉnh (Peak) — float

Dòng đỉnh từng pha, lấy từ thanh ghi IPeak của chip.

| Addr | Tên            | Đơn vị |
| ---- | -------------- | ------ |
| 68   | CurrentPeak_A  | A      |
| 70   | CurrentPeak_B  | A      |
| 72   | CurrentPeak_C  | A      |

### 3.12. Demand (nhu cầu công suất) — float

Trung bình trượt của công suất tác dụng tổng theo cửa sổ thời gian (mặc định 15 phút), và giá trị lớn nhất kể từ lần reset.

| Addr | Tên                    | Đơn vị |
| ---- | ---------------------- | ------ |
| 74   | ActivePowerDemand      | W      |
| 76   | ActivePowerDemand_Max  | W      |

---

## 4. Discrete Inputs (FC 02) — ngõ vào số, read-only

Từ PCF8574.

| Addr | Tên     | Nguồn        |
| ---- | ------- | ------------ |
| 0    | Input0  | PCF8574 P5   |
| 1    | Input1  | PCF8574 P2   |

---

## 5. Coils (FC 01 / 05 / 15) — ngõ ra số, read/write

Từ PCF8574.

| Addr | Tên      | Nguồn        |
| ---- | -------- | ------------ |
| 0    | Output0  | PCF8574 P7   |
| 1    | Output1  | PCF8574 P6   |

---

## 6. Holding Registers (FC 03 / 06 / 16) — cấu hình, read/write

| Addr | Tên               | Kiểu   | R/W | Ghi chú                                            |
| ---- | ----------------- | ------ | --- | -------------------------------------------------- |
| 0    | WiringMode        | uint16 | RW  | 0=3P4W, 1=3P3W; áp dụng ngay vào energy meter |
| 1    | LineFrequencySel  | uint16 | RW  | 0=50Hz, 1=60Hz; áp dụng ngay vào energy meter |
| 2    | DemandWindowMin   | uint16 | RW  | cửa sổ demand; giá trị 0 bị từ chối |
| 3    | ResetEnergy       | uint16 | W   | ghi khác 0 = xóa toàn bộ energy accumulator |
| 4    | ResetDemand       | uint16 | W   | ghi khác 0 = xóa demand + max demand |
| 5    | LastCommand       | uint16 | R   | địa chỉ action gần nhất |
| 6    | LastResult        | uint16 | R   | 0=success, 1=failed |
| 7    | RebootDevice      | uint16 | W   | ghi `0x5AA5` = reboot có chủ đích |

> Cấu hình link truyền thông (slave address, baud) **không** nằm trong Holding
> Register Modbus và cũng **không** chỉnh được qua web portal: hai giá trị này chỉ
> đổi trên LCD (Settings > RTU Slave), reboot để áp dụng. Parity/stop là hằng số
> 8N1, không có cài đặt. Baud/parity của bus **master** (UART2, chỉnh trên web
> portal) là hai tham số hoàn toàn tách biệt — không liên kết với link slave này.
> Modbus không có lệnh ApplyConfig.
>
> HR3, HR4 và HR7 là command one-shot, firmware tự trả chúng về 0 sau khi xử lý. HR5 và HR6 chỉ đọc. Ghi một dải FC16 bao gồm HR5 hoặc HR6 sẽ bị từ chối để tránh ghi đè diagnostics.

---

## 7. Input Registers định danh (FC 04, read-only)

| Addr | Tên             | Kiểu   | Ghi chú                    |
| ---- | --------------- | ------ | -------------------------- |
| 100  | DeviceID        | uint16 | 0x9032                     |
| 101  | FirmwareVersion | uint16 | 0x0100 = v1.00             |
| 102  | HardwareVersion | uint16 | 0x0100                     |
| 103  | MeasureValid    | uint16 | 1 = có số đo hợp lệ        |
| 104  | Uptime_H        | uint16 | uptime giây, high word     |
| 105  | Uptime_L        | uint16 | uptime giây, low word      |

---

## 8. Kiến trúc phần mềm

- **Gỡ** `uart_tasks.c` demo cũ (chuỗi "Hello").
- Thêm component/task **Modbus Slave** trên UART1 (RS485, GPIO13/14), dùng **esp-modbus** (Modbus RTU slave).
- Nguồn dữ liệu Modbus = `energy_meter_get_latest()` (cache có mutex) + energy/peak/demand từ energy meter task. **Không** đọc SPI trực tiếp trong task Modbus.
- Coils → `io_expander_set_out0/1`. Discrete inputs → `io_expander_get_in0/1`.
- **Modbus Master** trên UART (GPIO11/12): define khung + Kconfig, chưa thực thi logic.
- Cấu hình truyền thông + energy lưu NVS.

## 9. Việc kéo theo trong driver ATM90E32AS

- Thêm đọc **energy accumulator** (APenergyT 0x80, ANenergyT 0x84, RPenergyT 0x88, RNenergyT 0x8C) và cộng dồn kWh/kvarh trong firmware.
- Thêm đọc **current peak** (IPeakA/B/C 0xF5/0xF6/0xF7) với công thức `peak * current_gain / 8192000`.
- Bổ sung các field vào `atm90e32as_measurements_t`.

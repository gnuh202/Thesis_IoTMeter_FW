# Data Dictionary / Register Map

Tài liệu gốc mô tả toàn bộ điểm dữ liệu (data point) của firmware. Đây là nguồn
tham chiếu duy nhất để sau này xây dựng Modbus RTU Slave, Modbus TCP, MQTT,
Web API, LCD và Data Logger.

Phạm vi tài liệu này: **chỉ định nghĩa từ điển dữ liệu**. Chưa đánh số địa chỉ
Modbus, chưa gán MQTT topic, chưa định nghĩa Web API — những phần đó sẽ tham
chiếu ngược về tài liệu này khi triển khai.

## Quy ước

- **ID**: định danh nội bộ duy nhất của điểm dữ liệu, dùng để tham chiếu trong
  mọi lớp giao tiếp (Modbus/MQTT/Web/LCD/Logger). Prefix theo nhóm:
  - `MEAS_` — Measurement
  - `ENERGY_` — Energy
  - `SYS_` — System Status
  - `CFG_` — Configuration
  - `DI_` — Digital Input
  - `DO_` — Digital Output
- **Name**: tên field gốc trong struct firmware.
- **Data Type**: kiểu dữ liệu logic của điểm dữ liệu trong firmware
  (`float`, `uint8`, `uint16`, `uint32`, `uint64`, `bool`, `string[n]`, `enum`).
- **Unit**: đơn vị vật lý; `-` nếu không có đơn vị.
- **Access**:
  - `RO` — chỉ đọc (consumer đọc, không ghi).
  - `RW` — đọc/ghi (cấu hình hoặc điều khiển).
- **Source Module**: module firmware sở hữu dữ liệu (nơi cập nhật giá trị).
- **Reserved**: field đã có chỗ trong struct nhưng chưa có nguồn phần cứng/driver;
  hiện giữ giá trị 0/default, ghi rõ trong cột Ghi chú.

---

## 1. Measurement

Nguồn: `measurement_data_t` (`main/app/measurement_data.h`). Snapshot RAM cập
nhật bởi Energy Meter Task từ ATM90E32AS; mọi consumer đọc qua
`measurement_data_get()`.

| ID                       | Name             | Description                          | Data Type | Unit | Access | Source Module    | Ghi chú                         |
|--------------------------|------------------|--------------------------------------|-----------|------|--------|------------------|---------------------------------|
| MEAS_VOLTAGE_L1          | voltage_l1       | Điện áp kênh A: V1N (3P4W) hoặc U12 (3P3W) | float     | V    | RO     | measurement_data |                                 |
| MEAS_VOLTAGE_L2          | voltage_l2       | Điện áp kênh B: V2N (3P4W); không dùng (3P3W) | float     | V    | RO     | measurement_data | `voltage_valid` xác định hợp lệ |
| MEAS_VOLTAGE_L3          | voltage_l3       | Điện áp kênh C: V3N (3P4W) hoặc U32 (3P3W) | float     | V    | RO     | measurement_data |                                 |
| MEAS_VOLTAGE_AVG         | voltage_avg      | Điện áp trung bình các kênh áp hợp lệ | float     | V    | RO     | measurement_data |                                 |
| MEAS_CURRENT_L1          | current_l1       | Dòng kênh A: I1                     | float     | A    | RO     | measurement_data |                                 |
| MEAS_CURRENT_L2          | current_l2       | Dòng kênh B: I2                     | float     | A    | RO     | measurement_data | `I2=N/A` trên LCD 3P3W |
| MEAS_CURRENT_L3          | current_l3       | Dòng kênh C: I3                     | float     | A    | RO     | measurement_data |                                 |
| MEAS_CURRENT_NEUTRAL     | current_neutral  | Dòng RMS N được IC tính toán (`IrmsN`), không phải đầu vào CT thứ tư | float     | A    | RO     | measurement_data | Tạm thời hiển thị ở cả hai wiring mode |
| MEAS_CURRENT_AVG         | current_avg      | Trung bình số học của ba phần tử dòng, không có ý nghĩa metering đặc biệt | float     | A    | RO     | measurement_data | Không dùng trên LCD |
| MEAS_FREQUENCY           | frequency        | Tần số lưới                          | float     | Hz   | RO     | measurement_data |                                 |
| MEAS_POWER_ACTIVE_L1     | p1               | Công suất tác dụng pha L1            | float     | W    | RO     | measurement_data |                                 |
| MEAS_POWER_ACTIVE_L2     | p2               | Công suất tác dụng pha L2            | float     | W    | RO     | measurement_data |                                 |
| MEAS_POWER_ACTIVE_L3     | p3               | Công suất tác dụng pha L3            | float     | W    | RO     | measurement_data |                                 |
| MEAS_POWER_ACTIVE_TOTAL  | p_total          | Tổng công suất tác dụng              | float     | W    | RO     | measurement_data |                                 |
| MEAS_POWER_REACTIVE_L1   | q1               | Công suất phản kháng pha L1          | float     | var  | RO     | measurement_data |                                 |
| MEAS_POWER_REACTIVE_L2   | q2               | Công suất phản kháng pha L2          | float     | var  | RO     | measurement_data |                                 |
| MEAS_POWER_REACTIVE_L3   | q3               | Công suất phản kháng pha L3          | float     | var  | RO     | measurement_data |                                 |
| MEAS_POWER_REACTIVE_TOTAL| q_total          | Tổng công suất phản kháng            | float     | var  | RO     | measurement_data |                                 |
| MEAS_POWER_APPARENT_L1   | s1               | Công suất biểu kiến pha L1           | float     | VA   | RO     | measurement_data |                                 |
| MEAS_POWER_APPARENT_L2   | s2               | Công suất biểu kiến pha L2           | float     | VA   | RO     | measurement_data |                                 |
| MEAS_POWER_APPARENT_L3   | s3               | Công suất biểu kiến pha L3           | float     | VA   | RO     | measurement_data |                                 |
| MEAS_POWER_APPARENT_TOTAL| s_total          | Tổng công suất biểu kiến             | float     | VA   | RO     | measurement_data |                                 |
| MEAS_PF_L1               | pf1              | Hệ số công suất pha L1               | float     | -    | RO     | measurement_data |                                 |
| MEAS_PF_L2               | pf2              | Hệ số công suất pha L2               | float     | -    | RO     | measurement_data |                                 |
| MEAS_PF_L3               | pf3              | Hệ số công suất pha L3               | float     | -    | RO     | measurement_data |                                 |
| MEAS_PF_TOTAL            | pf_total         | Hệ số công suất tổng                 | float     | -    | RO     | measurement_data |                                 |
| MEAS_VOLTAGE_THD         | voltage_thd      | Độ méo hài tổng điện áp              | float     | %    | RO     | measurement_data | **Reserved** — chưa có nguồn    |
| MEAS_CURRENT_THD         | current_thd      | Độ méo hài tổng dòng điện            | float     | %    | RO     | measurement_data | **Reserved** — chưa có nguồn    |
| MEAS_TEMP_ATM90          | temp_atm90       | Nhiệt độ cảm biến ATM90E32AS         | float     | °C   | RO     | measurement_data |                                 |
| MEAS_TEMP_MCU            | temp_mcu         | Nhiệt độ MCU                         | float     | °C   | RO     | measurement_data | **Reserved** — chưa có nguồn    |
| MEAS_TEMP_RESERVED       | temp_reserved    | Nhiệt độ dự phòng                    | float     | °C   | RO     | measurement_data | **Reserved**                    |
| MEAS_LAST_UPDATE_US      | last_update_us   | Mốc thời gian cập nhật cuối          | uint64    | µs   | RO     | measurement_data | esp_timer µs kể từ boot         |
| MEAS_VALID               | valid            | Đã có ít nhất 1 lần cập nhật         | bool      | -    | RO     | measurement_data |                                 |

---

## 2. Energy

Nguồn: `measurement_data_t` (`main/app/measurement_data.h`), nhóm điện năng tích lũy.

| ID                      | Name                    | Description                       | Data Type | Unit  | Access | Source Module    | Ghi chú                      |
|-------------------------|-------------------------|-----------------------------------|-----------|-------|--------|------------------|------------------------------|
| ENERGY_ACTIVE_IMPORT    | energy_import           | Điện năng tác dụng nhập           | float     | kWh   | RO     | measurement_data |                              |
| ENERGY_ACTIVE_EXPORT    | energy_export           | Điện năng tác dụng xuất           | float     | kWh   | RO     | measurement_data |                              |
| ENERGY_REACTIVE_IMPORT  | energy_reactive_import  | Điện năng phản kháng nhập         | float     | kvarh | RO     | measurement_data |                              |
| ENERGY_REACTIVE_EXPORT  | energy_reactive_export  | Điện năng phản kháng xuất         | float     | kvarh | RO     | measurement_data |                              |
| ENERGY_APPARENT         | energy_apparent         | Điện năng biểu kiến               | float     | kVAh  | RO     | measurement_data | **Reserved** — chưa có nguồn |

---

## 3. System Status

Nguồn: `system_status` (`main/app/system_status.h`). Mỗi module có 1 trạng thái
kiểu `enum system_status_state_t`
(`UNKNOWN / INIT / READY / WARNING / ERROR / OFFLINE`). Consumer đọc qua
`system_status_get()`.

| ID                          | Name                   | Description                        | Data Type | Unit | Access | Source Module | Ghi chú |
|-----------------------------|------------------------|------------------------------------|-----------|------|--------|---------------|---------|
| SYS_ATM90_STATUS            | status_atm90           | Trạng thái đo lường ATM90E32AS     | enum      | -    | RO     | system_status |         |
| SYS_RS485_MASTER_STATUS     | status_rs485_master    | Trạng thái Modbus RTU master       | enum      | -    | RO     | system_status |         |
| SYS_RS485_SLAVE_STATUS      | status_rs485_slave     | Trạng thái Modbus RTU slave        | enum      | -    | RO     | system_status |         |
| SYS_ETHERNET_STATUS         | status_ethernet        | Trạng thái Ethernet (W5500)        | enum      | -    | RO     | system_status |         |
| SYS_WIFI_STATUS             | status_wifi            | Trạng thái WiFi                    | enum      | -    | RO     | system_status |         |
| SYS_MQTT_STATUS             | status_mqtt            | Trạng thái MQTT client             | enum      | -    | RO     | system_status |         |
| SYS_SD_CARD_STATUS          | status_sd_card         | Trạng thái thẻ SD                  | enum      | -    | RO     | system_status |         |
| SYS_DIGITAL_INPUT_STATUS    | status_digital_input   | Trạng thái khối ngõ vào số         | enum      | -    | RO     | system_status |         |
| SYS_DIGITAL_OUTPUT_STATUS   | status_digital_output  | Trạng thái khối ngõ ra số          | enum      | -    | RO     | system_status |         |

---

## 4. Configuration

Nguồn: `config_manager_t` (`main/app/config_manager.h`), facade trên
`config_store` (NVS). Consumer đọc/ghi qua Configuration Manager.

| ID                    | Name                 | Description                             | Data Type   | Unit | Access | Source Module   | Ghi chú                                    |
|-----------------------|----------------------|-----------------------------------------|-------------|------|--------|-----------------|--------------------------------------------|
| CFG_VERSION           | config_version       | Phiên bản schema cấu hình               | uint32      | -    | RO     | config_manager  | Dùng cho nâng cấp firmware                 |
| CFG_DEVICE_NAME       | device_name          | Tên thiết bị                            | string[33]  | -    | RW     | config_manager  |                                            |
| CFG_FIRMWARE_VERSION  | firmware_version     | Phiên bản firmware                      | string[24]  | -    | RO     | config_manager  | Lấy từ app descriptor                      |
| CFG_HARDWARE_VERSION  | hardware_version     | Phiên bản phần cứng                     | string[24]  | -    | RO     | config_manager  |                                            |
| CFG_DHCP_ENABLE       | dhcp_enable          | Bật DHCP cho Ethernet                   | bool        | -    | RW     | config_manager  |                                            |
| CFG_STATIC_IP         | static_ip            | Địa chỉ IP tĩnh                         | string[16]  | -    | RW     | config_manager  |                                            |
| CFG_GATEWAY           | gateway              | Gateway                                 | string[16]  | -    | RW     | config_manager  |                                            |
| CFG_NETMASK           | netmask              | Subnet mask                             | string[16]  | -    | RW     | config_manager  |                                            |
| CFG_DNS               | dns                  | Máy chủ DNS                             | string[16]  | -    | RW     | config_manager  |                                            |
| CFG_WIFI_SSID         | wifi_ssid            | WiFi SSID                               | string[33]  | -    | RW     | config_manager  |                                            |
| CFG_WIFI_PASS         | wifi_pass            | WiFi password                           | string[65]  | -    | RW     | config_manager  | Nhạy cảm — không phơi bày khi chỉ đọc      |
| CFG_MQTT_ENABLE       | mqtt_enable          | Bật MQTT client                         | bool        | -    | RW     | config_manager  |                                            |
| CFG_MQTT_BROKER       | mqtt_broker          | Địa chỉ broker (host)                   | string[128] | -    | RW     | config_manager  |                                            |
| CFG_MQTT_PORT         | mqtt_port            | Cổng broker                             | uint16      | -    | RW     | config_manager  |                                            |
| CFG_MQTT_USER         | mqtt_user            | MQTT username                           | string[33]  | -    | RW     | config_manager  |                                            |
| CFG_MQTT_PASS         | mqtt_pass            | MQTT password                           | string[65]  | -    | RW     | config_manager  | Nhạy cảm — không phơi bày khi chỉ đọc      |
| CFG_MQTT_PUBLISH_MS   | mqtt_publish_ms      | Chu kỳ publish telemetry                | uint32      | ms   | RW     | config_manager  |                                            |
| CFG_MQTT_CLIENT_ID    | mqtt_client_id       | MQTT client ID                          | string[64]  | -    | RW     | config_manager  | **Reserved** — sinh runtime, chưa persist  |
| CFG_MB_SLAVE_ID       | mb_slave_id          | Địa chỉ Modbus slave (của thiết bị)     | uint8       | -    | RW     | config_manager  | Link UART1 lên SCADA; **chỉ chỉnh trên LCD** (Settings > RTU Slave) |
| CFG_MB_BAUD_CODE      | mb_baud_code         | Mã baudrate (0=9600…4=115200)           | uint8       | -    | RW     | config_manager  | **Bus master** (UART2) — không liên kết baud slave |
| CFG_MB_PARITY_CODE    | mb_parity_code       | Mã parity (0=none,1=even,2=odd)         | uint8       | -    | RW     | config_manager  | **Bus master** (UART2); slave cố định 8N1   |
| CFG_MB_STOP_BITS      | mb_stop_bits         | Số stop bit                             | uint8       | -    | RW     | config_manager  | **Reserved** — chưa có field trong fw      |
| CFG_LINE_FREQ         | line_freq            | Tần số lưới (mirror calib; 0=50Hz,1=60Hz) | uint8       | -    | RO     | config_manager  | Chỉ set qua console/Kconfig + meter NVS   |
| CFG_CT_RATIO          | ct_ratio             | Tỉ số biến dòng (CT)                     | uint16      | -    | RW     | config_manager  | **Reserved** — ẩn trong gain               |
| CFG_PT_RATIO          | pt_ratio             | Tỉ số biến áp (PT)                       | uint16      | -    | RW     | config_manager  | **Reserved**                               |
| CFG_LCD_BACKLIGHT     | lcd_backlight        | Đèn nền LCD                             | bool        | -    | RW     | config_manager  | **Reserved** — chưa persist-apply          |
| CFG_LCD_SLEEP_TIMEOUT_S| lcd_sleep_timeout_s | Thời gian tự tắt màn hình               | uint32      | s    | RW     | config_manager  | **Reserved** — chưa triển khai             |
| CFG_BUZZER_ENABLE     | buzzer_enable        | Bật còi báo                             | bool        | -    | RW     | config_manager  | **Reserved** — hiện là Kconfig compile-time|
| CFG_MB_SLAVE_BAUD     | mb_slave_baud_code   | Mã baud link slave (0=9600…4=115200)    | uint8       | -    | RW     | config_manager  | Link UART1 của thiết bị; **chỉ chỉnh trên LCD**, độc lập bus master |

---

## 5. Digital Input

Nguồn: `io_expander` (`components/io_expander/include/io_expander.h`). 2 ngõ vào
số qua PCF8574. Đọc qua `io_expander_get_inN()`.

| ID              | Name          | Description        | Data Type | Unit | Access | Source Module | Ghi chú |
|-----------------|---------------|--------------------|-----------|------|--------|---------------|---------|
| DI_INPUT0_STATE | digital_in0   | Ngõ vào số kênh 0  | bool      | -    | RO     | io_expander   |         |
| DI_INPUT1_STATE | digital_in1   | Ngõ vào số kênh 1  | bool      | -    | RO     | io_expander   |         |

---

## 6. Digital Output

Nguồn: `io_expander` (`components/io_expander/include/io_expander.h`). 2 ngõ ra
số (relay) qua PCF8574. Ghi qua `io_expander_set_outN()`, đọc lại giá trị cache
qua `io_expander_get_outN()` (chân OUT của PCF8574 là write-only).

| ID              | Name          | Description         | Data Type | Unit | Access | Source Module | Ghi chú                        |
|-----------------|---------------|---------------------|-----------|------|--------|---------------|--------------------------------|
| DO_RELAY0_STATE | digital_out0  | Ngõ ra số kênh 0    | bool      | -    | RW     | io_expander   | Giá trị đọc là cache lần set    |
| DO_RELAY1_STATE | digital_out1  | Ngõ ra số kênh 1    | bool      | -    | RW     | io_expander   | Giá trị đọc là cache lần set    |

---

## Tổng kết

### Tổng số Data Point: **80**

| Nhóm             | Prefix    | Số điểm dữ liệu | Reserved |
|------------------|-----------|-----------------|----------|
| 1. Measurement   | `MEAS_`   | 33              | 4        |
| 2. Energy        | `ENERGY_` | 5               | 1        |
| 3. System Status | `SYS_`    | 9               | 0        |
| 4. Configuration | `CFG_`    | 29              | 7        |
| 5. Digital Input | `DI_`     | 2               | 0        |
| 6. Digital Output| `DO_`     | 2               | 0        |
| **Tổng**         |           | **80**          | **12**   |

### Các trường còn Reserved (12)

Field đã có chỗ trong struct nhưng chưa có nguồn phần cứng/driver; hiện giữ
giá trị 0/default cho tới khi có nguồn thật.

- **Measurement (4)**: `MEAS_VOLTAGE_THD`, `MEAS_CURRENT_THD`, `MEAS_TEMP_MCU`,
  `MEAS_TEMP_RESERVED`
- **Energy (1)**: `ENERGY_APPARENT`
- **Configuration (7)**: `CFG_MQTT_CLIENT_ID`, `CFG_MB_STOP_BITS`, `CFG_CT_RATIO`,
  `CFG_PT_RATIO`, `CFG_LCD_BACKLIGHT`, `CFG_LCD_SLEEP_TIMEOUT_S`,
  `CFG_BUZZER_ENABLE`

### Deferred (chưa làm ở feature này)

- Đánh số địa chỉ Modbus (RTU/TCP).
- Gán MQTT topic cho từng điểm dữ liệu.
- Định nghĩa Web API endpoint.
- Ánh xạ điểm dữ liệu lên LCD và Data Logger.

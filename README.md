# Thesis IoT Meter — Firmware

Firmware ESP-IDF cho **đồng hồ đo điện năng 3 pha công nghiệp** trên ESP32-S3 (N16R8, 16 MB flash / 8 MB OPI PSRAM), dùng IC đo **ATM90E32AS**.

## Tính năng chính

- **Đo lường 3 pha** (ATM90E32AS qua SPI): điện áp, dòng, công suất P/Q/S, hệ số công suất, tần số, nhiệt độ, năng lượng tích lũy (kWh) và demand. Hiệu chỉnh (calibration) qua console, lưu NVS.
- **Modbus RTU Slave + Master** trên **hai link UART vật lý tách biệt** (RS485): slave cho SCADA/PLC đọc (địa chỉ + baud cấu hình trên LCD, framing 8N1 cố định), master đọc công tơ ngoài (PM710, EM07K; baud/parity/slots cấu hình trên portal). Hai bên độc lập, đổi một không làm đổi bên kia.
- **Network stack** — Ethernet W5500 (SPI) + WiFi STA/SoftAP, failover ưu tiên ETH → WiFi STA, gom init hạ tầng tập trung, SoftAP on-demand + auto-AP recovery khi mất hết mạng.
- **MQTT client** — đúng 1 broker lưu NVS, bật/tắt trên LCD (Settings ▸ MQTT), hỗ trợ TLS, publish telemetry/energy/io/heartbeat (cJSON), LWT online/offline, subscribe điều khiển relay.
- **Web Configuration Portal** — cấu hình mạng/MQTT/RTU master/danh tính thiết bị qua HTTP; mật khẩu là trường chỉ ghi, không bao giờ trả giá trị thật về browser.
- **Giao diện LCD 20x4 + 5 nút bấm** — menu (Settings, RTU Slave/Master, MQTT, Alarm Settings, DISPLAY & KEYS), màn home xoay vòng tự động, Sleep màn hình.
- **Console developer** — cổng đăng nhập + các lệnh đọc/ghi thanh ghi IC, hiệu chỉnh đo, cấu hình mạng/MQTT/RTU, chỉnh mức log runtime, `ping` ICMP, kiểm tra data point.
- **Configuration Manager** — một ảnh cấu hình trong RAM, lưu NVS dạng blob có version (append-only, blob cũ vẫn đọc được), là nguồn duy nhất cho portal / LCD / console / Modbus. `config_store` là tầng ghi đọc NVS phía dưới.
- **Thẻ SD** — mount FAT, trạng thái hiển thị trên LCD, xuất/nhập file hiệu chỉnh (calib) giữa máy và thẻ.

## Cấu trúc dự án

```text
main/
├── app_main.c               điểm vào, gọi app_tasks_start()
└── app/                     tầng ứng dụng (task + điều phối)
    ├── app_tasks.c/.h            thứ tự khởi động toàn hệ thống
    ├── boot_manager.c/.h         trình tự boot, báo cáo lỗi khởi động
    ├── config_manager.c/.h       ảnh cấu hình + DTO versioned (NVS)
    ├── config_apply.c/.h         áp dụng cấu hình theo domain
    ├── register_access.c/.h      Data Point Layer (bảng field cho mọi frontend)
    ├── measurement_data.c/.h     dữ liệu đo gần nhất cho consumer
    ├── system_status.c/.h        trạng thái sức khỏe hệ thống
    ├── energy_meter_task.c/.h    đo lường ATM90E32AS
    ├── modbus_slave_task.c/.h    Modbus RTU slave (link của thiết bị)
    ├── modbus_master_task.c/.h   Modbus RTU master (bus công tơ ngoài)
    ├── network_manager.c/.h      state machine mạng + failover + infra init
    ├── ethernet_driver.c/.h      driver W5500
    ├── wifi_manager.c/.h         WiFi STA/SoftAP
    ├── network_comm_task.c/.h    bootstrap mạng (gọi ethernet_driver_init)
    ├── mqtt_manager.c/.h         MQTT client
    ├── cert_store.c/.h           CA/client cert trên partition FAT
    ├── web_portal.c/.h           web configuration portal
    ├── home_screen.c/.h          menu LCD + hiển thị + alarm settings
    ├── hmi_test_task.c/.h        menu test HMI (vào bằng tổ hợp phím lúc boot)
    └── console_task.c/.h         console + các lệnh

components/                 driver / BSP / thư viện tái dùng
├── config_store/           tầng lưu/nạp NVS cho cấu hình hệ thống
├── modbus_meters/          profile thiết bị đo qua RTU master (PM710, EM07K)
├── atm90e32as/             driver IC đo
├── lcd_menu/, lcd2004_i2c/ framework menu + driver LCD 20x4 qua I2C
├── hmi_bsp/, pcf8575/      BSP bàn phím/LED/buzzer, expander GPIO
├── io_expander/, pcf8574/  BSP PCF8574 (relay out + input + reset W5500)
├── gpio_isr_service/       ISR dùng chung cho input
├── i2c_bus/                hạ tầng I2C dùng chung
├── spi_bus_shared/         SPI dùng chung (ATM90 + W5500)
└── sd_card/                thẻ SD (mount FAT, log sự kiện, export/import calib)
```

Phân lớp app / driver / BSP và vòng đời khởi động: xem [docs/architecture.md](docs/architecture.md).

## Tài liệu

- [docs/architecture.md](docs/architecture.md) — kiến trúc, phân lớp, vòng đời khởi động (đọc trước khi bàn giao).
- [docs/console_commands.md](docs/console_commands.md) — đầy đủ các lệnh console + bảng TAG log.
- [docs/ESP32_Network_Manager_Design.md](docs/ESP32_Network_Manager_Design.md) — thiết kế network stack.
- [docs/mqtt_guide.md](docs/mqtt_guide.md) — hướng dẫn MQTT: cấu hình, lệnh PC/ESP console, testcase.
- [docs/mqtt_payloads.md](docs/mqtt_payloads.md) — nguồn duy nhất cho topic + payload MQTT.
- [docs/ESP32_WebPortal_Design.md](docs/ESP32_WebPortal_Design.md) — thiết kế Web Configuration Portal.
- [docs/atm90e32as_console_calib.md](docs/atm90e32as_console_calib.md) — hướng dẫn hiệu chỉnh đo.
- [docs/modbus_slave_register_map.md](docs/modbus_slave_register_map.md) — bản đồ thanh ghi Modbus slave của thiết bị.
- [docs/register_map.md](docs/register_map.md) — data dictionary: mọi data point + cách ánh xạ sang register.
- [docs/modbus_reference_manual.md](docs/modbus_reference_manual.md) — reference manual bản thiết bị (cho người đọc Modbus bên ngoài).
- [docs/sd_card_fixes.md](docs/sd_card_fixes.md), [docs/sd_card_ram_usage.md](docs/sd_card_ram_usage.md) — quá trình bring-up SD card và ngân sách RAM.
- `docs/*.pdf` — datasheet IC đo và manual công tơ ngoài (PM710, EM07K).

## Build & Flash

Cần **ESP-IDF v5.5.1** (các bản khác chưa kiểm chứng).

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Target: `esp32s3`. Cấu hình flash 16 MB + bảng phân vùng tùy chỉnh ([partitions.csv](partitions.csv)) và các mặc định khác nằm trong [sdkconfig.defaults](sdkconfig.defaults) — chạy `idf.py build` lần đầu sẽ tự sinh `sdkconfig`.

> `sdkconfig.defaults` có khối **Board identity** ghim sẵn hai thứ mà mặc định IDF không tái tạo đúng cho mạch này: REPL console trên USB-Serial-JTAG và buzzer active-high. Đừng xóa; nếu làm hỏng `sdkconfig` thì `del sdkconfig && idf.py set-target esp32s3` sẽ dựng lại đúng từ đó.

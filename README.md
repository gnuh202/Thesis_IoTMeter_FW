# Thesis IoT Meter — Firmware

Firmware ESP-IDF cho **đồng hồ đo điện năng 3 pha công nghiệp** trên ESP32-S3 (N16R2, 16 MB flash / 2 MB PSRAM), dùng IC đo **ATM90E32AS**.

## Tính năng chính

- **Đo lường 3 pha** (ATM90E32AS qua SPI): điện áp, dòng, công suất P/Q/S, hệ số công suất, tần số, nhiệt độ, năng lượng tích lũy (kWh) và demand. Hiệu chỉnh (calibration) qua console, lưu NVS.
- **Modbus RTU Slave + Master** (RS485) — đồng hồ vừa là slave (cho SCADA/PLC đọc) vừa là master (đọc thiết bị khác).
- **Network stack** — Ethernet W5500 (SPI) + WiFi STA/SoftAP, failover ưu tiên ETH → WiFi STA, gom init hạ tầng tập trung, SoftAP on-demand + auto-AP recovery khi mất hết mạng.
- **MQTT client** — 3 broker profile lưu NVS (1 active), hỗ trợ TLS, publish telemetry/energy/io/heartbeat (cJSON), LWT online/offline.
- **Console developer** — cổng đăng nhập + các lệnh cấu hình mạng/MQTT, hiệu chỉnh đo, chỉnh mức log runtime.
- **config_store** — lưu cấu hình network/MQTT/system vào NVS (magic + version, tự rơi về default khi blob hỏng/thiếu).

## Cấu trúc dự án

```text
main/
├── app_main.c              điểm vào, gọi app_tasks_start()
└── app/                    tầng ứng dụng (task + điều phối)
    ├── app_tasks.c/.h          thứ tự khởi động toàn hệ thống
    ├── energy_meter_task.c/.h  đo lường ATM90E32AS
    ├── modbus_slave_task.c/.h  Modbus RTU slave
    ├── modbus_master_task.c/.h Modbus RTU master
    ├── network_manager.c/.h    state machine mạng + failover + infra init
    ├── ethernet_driver.c/.h    driver W5500
    ├── wifi_manager.c/.h        WiFi STA/SoftAP
    ├── network_comm_task.c/.h  ping kiểm tra reachability
    ├── mqtt_manager.c/.h        MQTT client
    └── console_task.c/.h        console + các lệnh

components/                 driver / BSP / thư viện tái dùng
├── config_store/           lưu cấu hình NVS (network/MQTT/system)
├── io_expander/            BSP PCF8574 (2 relay out + 2 input + reset W5500)
├── atm90e32as/             driver IC đo
├── i2c_bus/, pcf8574/      hạ tầng I2C
├── spi_bus_shared/         SPI dùng chung (ATM90 + W5500)
└── sd_card/                thẻ SD
```

Phân lớp app / driver / BSP và vòng đời khởi động: xem [docs/architecture.md](docs/architecture.md).

## Tài liệu

- [docs/architecture.md](docs/architecture.md) — kiến trúc, phân lớp, vòng đời khởi động (đọc trước khi bàn giao).
- [docs/console_commands.md](docs/console_commands.md) — đầy đủ các lệnh console + bảng TAG log.
- [docs/ESP32_Network_Manager_Design.md](docs/ESP32_Network_Manager_Design.md) — thiết kế network stack.
- [docs/ESP32_MQTT_Design.md](docs/ESP32_MQTT_Design.md) — thiết kế MQTT.
- [docs/atm90e32as_console_calib.md](docs/atm90e32as_console_calib.md) — hướng dẫn hiệu chỉnh đo.
- [docs/modbus_slave_register_map.md](docs/modbus_slave_register_map.md) — bản đồ thanh ghi Modbus.

## Build & Flash

```bash
idf.py build
idf.py -p <PORT> flash monitor
```

Target: `esp32s3`. Cấu hình flash 16 MB + bảng phân vùng tùy chỉnh ([partitions.csv](partitions.csv)) và các mặc định khác nằm trong [sdkconfig.defaults](sdkconfig.defaults) — chạy `idf.py build` lần đầu sẽ tự sinh `sdkconfig`.

# Thesis IoT Meter — Firmware

Firmware ESP-IDF cho **đồng hồ đo điện năng 3 pha công nghiệp** trên ESP32-S3 (N16R8, 16 MB flash / 8 MB OPI PSRAM), dùng IC đo **ATM90E32AS**. Thiết bị là điểm đo ĐMTMN tự sản tự tiêu thụ: tự đo và tự hiệu chuẩn, phát số liệu qua Modbus RTU/TCP và MQTT, nhận lệnh điều khiển công suất P/Q từ đơn vị điều độ, và tự cập nhật firmware qua OTA với cơ chế rollback an toàn.

## Tính năng chính

- **Đo lường 3 pha** (ATM90E32AS qua SPI): điện áp, dòng, công suất P/Q/S, hệ số công suất, tần số, nhiệt độ, năng lượng tích lũy (kWh) và demand. Hiệu chỉnh (calibration) qua console, lưu NVS.
- **Modbus RTU Slave + Master** trên **hai link UART vật lý tách biệt** (RS485): slave cho SCADA/PLC đọc (địa chỉ + baud cấu hình trên LCD, framing 8N1 cố định), master đọc công tơ ngoài (PM710, EM07K; baud/parity/slots cấu hình trên portal). Hai bên độc lập, đổi một không làm đổi bên kia.
- **Modbus TCP server** (cổng 502, socket lwIP riêng) — bộ tín hiệu giám sát và điều khiển theo Quyết định EVN về kết nối ĐMTMN với hệ thống GSĐK: input registers công suất phát lên lưới, điện áp, dòng, tần số, hệ số công suất; coil + holding register nhận lệnh cho phép điều khiển và setpoint P/Q theo %. Lệnh nhận được sống qua cả mất kết nối lẫn reboot (lưu NVS ngay khi nhận). Bộ tín hiệu đầy đủ và những điểm cần chốt với đơn vị điều độ: [docs/modbus_tcp_evn_map.md](docs/modbus_tcp_evn_map.md).
- **Network stack** — Ethernet W5500 (SPI) + WiFi STA/SoftAP, failover ưu tiên ETH → WiFi STA, gom init hạ tầng tập trung, SoftAP on-demand + auto-AP recovery khi mất hết mạng.
- **MQTT client** — đúng 1 broker lưu NVS, bật/tắt trên LCD (Settings ▸ MQTT), hỗ trợ TLS, publish telemetry/energy/io/heartbeat (cJSON), LWT online/offline, subscribe điều khiển relay (`cmd/out0`, `cmd/out1`) và điều khiển OTA (`cmd/ota`).
- **OTA firmware update** — check và cài trực tiếp từ GitHub Releases qua `manifest.json` (có sha256). Ảnh mới chạy thử 60 giây kèm self-test (chip đo phải READY) trước khi commit; nếu máy reset giữa chừng, bootloader tự quay về bản cũ mà không cần ai can thiệp. Chủ động được bằng `ota rollback` / `ota confirm` trên console, LCD hoặc MQTT. Toàn bộ vòng đời và quy trình phát hành: [docs/ota_release.md](docs/ota_release.md).
- **Alarm manager** — đánh giá trạng thái IC đo theo từng pha (quá áp, quá dòng, sag, mất pha), latch + hiển thị trên LCD, buzzer/LED qua I/O expander, nhật ký sự cố lưới `FAULTS.CSV` ra thẻ SD; reset latch từ menu LCD.
- **Web Configuration Portal** — cấu hình mạng/MQTT/RTU master/danh tính thiết bị qua HTTP; mật khẩu là trường chỉ ghi, không bao giờ trả giá trị thật về browser.
- **Giao diện LCD 20x4 + 5 nút bấm** — menu (Settings gồm Energy, FW Update, TCP Server; RTU Slave/Master; MQTT; Alarm Settings; DISPLAY & KEYS), màn home xoay vòng tự động, Sleep màn hình.
- **Console developer** — cổng đăng nhập + các lệnh đọc/ghi thanh ghi IC, hiệu chỉnh đo, cấu hình mạng/MQTT/RTU, OTA, chỉnh mức log runtime, `ping` ICMP, kiểm tra data point.
- **Configuration Manager** — một ảnh cấu hình trong RAM, lưu NVS dạng blob có version (append-only, blob cũ vẫn đọc được), là nguồn duy nhất cho portal / LCD / console / Modbus. `config_store` là tầng ghi đọc NVS phía dưới.
- **Thẻ SD** — mount FAT, trạng thái hiển thị trên LCD, CSV năng lượng xoay vòng theo kích thước, nhật ký sự cố lưới, xuất/nhập file hiệu chỉnh (calib) giữa máy và thẻ.

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
    ├── energy_meter_task.c/.h    đo lường ATM90E32AS + tích luỹ energy/demand
    ├── time_source.c/.h          lớp thời gian duy nhất (DS1307 + SNTP, cờ chất lượng)
    ├── alarm_manager.c/.h        chấm trạng thái cảnh báo, latch, FAULTS.CSV
    ├── modbus_slave_task.c/.h    Modbus RTU slave (link của thiết bị)
    ├── modbus_master_task.c/.h   Modbus RTU master (bus công tơ ngoài)
    ├── modbus_tcp_task.c/.h      Modbus TCP server (hồ sơ EVN ĐMTMN)
    ├── ota_manager.c/.h          OTA từ GitHub Releases + self-test/rollback
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
├── ds1307/                 RTC ngoài trên I2C (backend của time_source)
├── lcd_menu/, lcd2004_i2c/ framework menu + driver LCD 20x4 qua I2C
├── hmi_bsp/, pcf8575/      BSP bàn phím/LED/buzzer, expander GPIO
├── io_expander/, pcf8574/  BSP PCF8574 (relay out + input + reset W5500)
├── gpio_isr_service/       ISR dùng chung cho input
├── i2c_bus/                hạ tầng I2C dùng chung
├── spi_bus_shared/         SPI dùng chung (ATM90 + W5500)
└── sd_card/                thẻ SD (CSV năng lượng, FAULTS.CSV, file calib)
```

Phân lớp app / driver / BSP, vòng đời khởi động và ranh giới "lõi đo độc lập hoàn toàn với mạng": xem [docs/architecture.md](docs/architecture.md).

## Releases & OTA

Việc phát hành chạy hết trên CI:

1. Push một tag `vX.Y.Z` lên repo.
2. GitHub Actions build bằng ESP-IDF 5.5.1, nhúng version từ `git describe` vào app descriptor.
3. Release xuất hiện với hai file: `luanvan_firmware.bin` và `manifest.json` (version, URL tải, ghi chú, sha256).

Thiết bị chỉ cần mạng tới GitHub:

```
ota check      # đọc manifest từ /releases/latest, so version
ota update     # tải, kiểm tra, lật boot partition, tự reboot
```

Sau reboot, ảnh mới nằm trong "cửa sổ self-test" 60 giây: đủ thời gian và chip đo READY thì tự commit; crash hoặc chip đo không lên thì lần reset kế bootloader tự trả máy về bản cũ. Trong khi còn ở cửa sổ này vẫn chủ động `ota rollback` được — đây cũng là cảnh rollback trực quan nhất khi trình diễn thiết bị. Điều duy nhất cần tránh là reset máy trong 60 giây đó; vì sao, và xử lý thế nào nếu lỡ tay: [docs/ota_release.md](docs/ota_release.md) (mục khôi phục sự cố).

## Tài liệu

Dành cho người vận hành / tích hợp thiết bị:

- [docs/modbus_slave_register_map.md](docs/modbus_slave_register_map.md) — bản đồ thanh ghi Modbus RTU slave của thiết bị.
- [docs/modbus_tcp_evn_map.md](docs/modbus_tcp_evn_map.md) — bản đồ thanh ghi Modbus TCP theo QĐ EVN, kèm các giả định cần chốt với đơn vị điều độ.
- [docs/modbus_reference_manual.md](docs/modbus_reference_manual.md) — reference manual bản thiết bị (cho người đọc Modbus bên ngoài).
- [docs/register_map.md](docs/register_map.md) — data dictionary: mọi data point + cách ánh xạ sang register.
- [docs/mqtt_payloads.md](docs/mqtt_payloads.md) — nguồn duy nhất cho topic + payload MQTT.
- [docs/mqtt_guide.md](docs/mqtt_guide.md) — hướng dẫn MQTT: cấu hình, lệnh PC/ESP console, testcase.
- [docs/ota_release.md](docs/ota_release.md) — vòng đời OTA, quy trình phát hành release, khôi phục sự cố.
- [docs/energy_logging.md](docs/energy_logging.md) — tích luỹ energy, NVS, demand, CSV thẻ SD, time_source.
- [docs/console_commands.md](docs/console_commands.md) — đầy đủ các lệnh console + bảng TAG log.
- [docs/atm90e32as_console_calib.md](docs/atm90e32as_console_calib.md) — hướng dẫn hiệu chỉnh đo.

Dành cho người phát triển:

- [docs/architecture.md](docs/architecture.md) — kiến trúc, phân lớp, vòng đời khởi động (đọc trước khi bàn giao).
- [docs/ESP32_Network_Manager_Design.md](docs/ESP32_Network_Manager_Design.md) — thiết kế network stack.
- [docs/ESP32_WebPortal_Design.md](docs/ESP32_WebPortal_Design.md) — thiết kế Web Configuration Portal.
- [docs/atm90e32as_test_plan.md](docs/atm90e32as_test_plan.md), [docs/atm90e32as_ct_pga_test_plan.md](docs/atm90e32as_ct_pga_test_plan.md) — kế hoạch kiểm thử driver đo.
- [docs/sd_card_fixes.md](docs/sd_card_fixes.md), [docs/sd_card_ram_usage.md](docs/sd_card_ram_usage.md) — quá trình bring-up SD card và ngân sách RAM.
- `docs/*.pdf` — datasheet IC đo, manual công tơ ngoài (PM710, EM07K) và văn bản quyết định EVN.

## Build & Flash

Cần **ESP-IDF v5.5.1** (các bản khác chưa kiểm chứng).

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash monitor
```

Target: `esp32s3`. Cấu hình flash 16 MB + bảng phân vùng tùy chỉnh ([partitions.csv](partitions.csv)) và các mặc định khác nằm trong [sdkconfig.defaults](sdkconfig.defaults) — chạy `idf.py build` lần đầu sẽ tự sinh `sdkconfig`.

> `sdkconfig.defaults` có khối **Board identity** ghim sẵn hai thứ mà mặc định IDF không tái tạo đúng cho mạch này: REPL console trên USB-Serial-JTAG và buzzer active-high. Đừng xóa; nếu làm hỏng `sdkconfig` thì `del sdkconfig && idf.py set-target esp32s3` sẽ dựng lại đúng từ đó.

Console chạy trên cổng USB-Serial-JTAG native của S3 — một cáp USB cho cả flash lẫn log.

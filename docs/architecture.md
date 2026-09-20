# Kiến trúc Firmware — ESP32-S3 IoT Energy Meter

Tài liệu bàn giao: phân lớp module, thứ tự khởi động, và ranh giới app / driver / BSP. Đọc cùng [console_commands.md](console_commands.md) (giao diện vận hành) và 2 design doc mạng/MQTT (chi tiết từng hệ con).

---

## 1. Tổng quan

Đồng hồ đo điện năng 3 pha công nghiệp trên ESP32-S3 (N16R8: 16MB flash, 8MB OPI PSRAM). Lõi đo dùng IC **ATM90E32AS** (SPI). Dữ liệu ra ngoài qua **Modbus RTU** (RS485) và **MQTT** (Ethernet/WiFi). Cấu hình lưu **NVS**, chỉnh runtime qua console.

Nguyên tắc xuyên suốt: **lõi đo (ATM90 + Modbus RTU) độc lập hoàn toàn với mạng.** Mạng sập, đổi interface, hay đang cấu hình — task đo vẫn chạy.

---

## 2. Phân lớp: app / driver / BSP

Ranh giới quyết định file nằm ở `main/app/` hay `components/`:

- **App** (`main/app/`) — có FreeRTOS task hoặc logic điều phối, biết về nghiệp vụ (đo, Modbus, MQTT, state machine mạng). Được phép phụ thuộc xuống driver/BSP/component.
- **Driver / BSP** (`components/`) — bọc một con chip hoặc một bus, không biết nghiệp vụ. Component **không được** phụ thuộc ngược lên `main`. Đây là lý do một số module mạng còn ở `main/app/` (xem §5).

### app — `main/app/`

| File | TAG log | Vai trò |
|---|---|---|
| `app_tasks.c` | `app_tasks` | Điểm khởi động: dựng mọi task theo thứ tự (§4) |
| `energy_meter_task.c` | `energy_meter` | Đọc ATM90E32AS định kỳ, giữ số đo mới nhất, calib qua NVS; **tích luỹ energy + demand, lưu NVS 2-slot, ghi CSV ra thẻ SD** (xem [energy_logging.md](energy_logging.md)) |
| `time_source.c` | `time_source` | Lớp thời gian duy nhất của firmware: epoch + cờ chất lượng `S`/`E`/`U`, boot counter, mốc epoch trong NVS. Chưa có backend (DS1307 chờ phần cứng) — mọi consumer đã nối sẵn |
| `modbus_slave_task.c` | `modbus_slave` | Modbus RTU slave (RS485) — map thanh ghi số đo |
| `modbus_master_task.c` | `modbus_master` | Modbus RTU master (đọc thiết bị ngoài) |
| `modbus_tcp_task.c` | `mb_tcp` | Modbus TCP server theo hồ sơ EVN ĐMTMN (socket lwIP riêng, xem [modbus_tcp_evn_map.md](modbus_tcp_evn_map.md)) |
| `console_task.c` | `console_task` | Console esp_console + login gate; các lệnh cấu hình |
| `network_manager.c` | `net_mgr` | State machine mạng: failover ETH↔STA, AP on-demand, auto-AP |
| `ethernet_driver.c` | `ethernet_driver` | W5500 (SPI) bring-up + ETH/IP event → còn ở app vì phụ thuộc net infra |
| `wifi_manager.c` | `wifi_manager` | WiFi STA + SoftAP, event, credential runtime |
| `network_comm_task.c` | `network_comm` | Task ping kiểm tra reachability trên interface active |
| `mqtt_manager.c` | `mqtt_mgr` | MQTT client: broker duy nhất, TLS, publish telemetry, LWT |
| `app_main.c` | `app_main` | entry `app_main()` → gọi `app_tasks_start()` |

### driver / BSP — `components/`

| Component | TAG log | Vai trò |
|---|---|---|
| `config_store` | `config_store` | Đọc/ghi NVS (network/MQTT/system), magic+version, trả default khi thiếu |
| `io_expander` | `io_expander` | BSP: PCF8574 — 2 relay out + 2 input + chân reset W5500 |
| `atm90e32as` | `atm90e32as` | Driver IC đo công suất (SPI) |
| `pcf8574` | `pcf8574` | Driver chip I/O expander I2C |
| `i2c_bus` | — | Bus I2C dùng chung |
| `spi_bus_shared` | `spi_bus_shared` | Bus SPI dùng chung (ATM90 + W5500) |
| `sd_card` | `sd_card` | Thẻ SD (SDSPI, dùng chung bus SPI với ATM90E32AS): energy CSV có header tự ghi + xoay vòng theo kích thước, nhật ký sự cố lưới `FAULTS.CSV`, file calib |

---

## 3. Cấu trúc thư mục

```text
main/
├── app_main.c              # entry
├── CMakeLists.txt          # SRCS app + REQUIRES (mọi component dùng phải khai ở đây)
├── Kconfig.projbuild       # tham số cấu hình (GPIO, timeout mạng, MQTT...)
└── app/
    ├── app_tasks.c/.h
    ├── energy_meter_task.c/.h   time_source.c/.h
    ├── modbus_slave_task.c/.h   modbus_master_task.c/.h
    ├── modbus_tcp_task.c/.h
    ├── console_task.c/.h
    ├── network_manager.c/.h     ethernet_driver.c/.h
    ├── wifi_manager.c/.h        network_comm_task.c/.h
    └── mqtt_manager.c/.h

components/
├── config_store/    (config_store.c + include/)
├── io_expander/     (io_expander.c + include/)
├── atm90e32as/  pcf8574/  i2c_bus/  spi_bus_shared/  sd_card/

docs/                # tài liệu (bạn đang đọc)
partitions.csv       # dual-OTA 16MB (ota_0/ota_1 4MB + NVS 64KB + storage 1MB)
sdkconfig.defaults   # cấu hình sống qua regenerate (flash, partition, TLS, log level)
```

> **Quy tắc vàng khi thêm component:** mỗi component đưa xuống `components/` phải được thêm tên vào `REQUIRES` của `main/CMakeLists.txt`, nếu không `main` sẽ báo `fatal error: <header>.h: No such file`.

---

## 4. Thứ tự khởi động (`app_tasks_start`)

```
1. net infra init         (NVS + esp_netif + event loop — sở hữu tập trung, chạy trước mọi thứ dùng NVS)
2. io_expander_start      (I2C + PCF8574; cần trước W5500 vì giữ chân reset của nó)
2b. time_source_init      (TZ + boot counter + khôi phục mốc epoch; phải trước energy task để dòng CSV đầu tiên đã có dấu thời gian)
3. energy_meter_task      (lõi đo — độc lập mạng)
4. modbus_slave / master  (RS485)
5. console_task           (nếu bật)
6. wifi_manager_start     (STA-only lúc boot; AP on-demand)
7. network_comm_task      (khởi W5500 + task ping)
8. network_manager_start  (orchestrator: quan sát, failover, prime STA cred từ NVS)
9. mqtt_manager_start     (nếu APP_MQTT_ENABLE — chờ có IP rồi mới connect)
```

Điểm cốt lõi: bước 3-4 (đo + Modbus) **không phụ thuộc** bước 1,6-9 (mạng). Mạng hỏng không ảnh hưởng đo.

### 4.1. Tạm dừng khi AP config portal mở

`network_manager_is_config_mode()` (đọc `network_status_t.ap_active`) là **nguồn sự
thật duy nhất**. Khi portal mở, energy task / mqtt_manager / modbus master **tự bỏ
qua phần thân vòng lặp** (cooperative, không `vTaskSuspend`) nên không mutex/UART/SPI
nào bị bỏ dở — thoát portal là resume ngay ở tick kế tiếp. Modbus **slave** vẫn chạy
(phải tiếp tục trả lời master phía trên).

Ngoại lệ quan trọng: energy task vẫn **drain thanh ghi read-to-clear mỗi 10 s** trong
suốt thời gian portal mở, nếu không tải lớn sẽ làm tràn count uint16 (trần 204.8 Wh)
và mất năng lượng — chi tiết ở [energy_logging.md §1.1](energy_logging.md#11-trần-2048-wh-mỗi-cửa-sổ-đọc).

---

## 5. Ghi chú kiến trúc mạng

`ethernet_driver` và `wifi_manager` là driver phần cứng nhưng **vẫn nằm ở `main/app/`**, không phải `components/`. Lý do: cả hai gọi hàm khởi tạo hạ tầng mạng (NVS + netif + event loop) hiện thuộc `network_manager` — mà `network_manager` là app-orchestrator (có state machine, phụ thuộc `config_store` + cả 2 driver). Vì component không được phụ thuộc ngược lên `main`, nếu muốn đưa 2 driver này xuống `components/` thì phải tách phần "infra init" ra một component lá riêng trước. Việc đó **chưa làm** (quyết định giữ nguyên network, không can thiệp thêm).

Chi tiết failover / AP on-demand / auto-AP: xem [ESP32_Network_Manager_Design.md](ESP32_Network_Manager_Design.md).

---

## 6. Cấu hình & build

- **Cấu hình runtime** (broker MQTT duy nhất, WiFi cred, chu kỳ publish): lưu NVS qua `config_store`, chỉnh bằng `net-cfg` / `mqtt-cfg` (xem [console_commands.md](console_commands.md)), Web portal hoặc LCD. Portal là "Save and restart" — lưu rồi reboot. LCD apply live (`config_apply`, MQTT rebuild client bất đồng bộ). Lệnh console chỉ sửa RAM → thay đổi mất khi reboot vì không xuống NVS. Bật/tắt MQTT theo đường sản phẩm nằm ở **LCD Settings ▸ MQTT** (portal không còn control này).
- **Cấu hình build** (GPIO, timeout, default): `main/Kconfig.projbuild` → giá trị vào `sdkconfig`. Những gì cần sống qua regenerate nằm trong `sdkconfig.defaults` (flash 16MB, partition, TLS cert bundle, log max level).
- **Build:** `idf.py build flash monitor`.
- `sdkconfig` **không** commit (sinh ra); `sdkconfig.defaults` thì có.

---

## 7. Trạng thái triển khai

| Hệ con | Trạng thái |
|---|---|
| Đo ATM90E32AS + calib console | Xong |
| Energy accumulator + NVS 2-slot/CRC32, demand tích phân thời gian | Xong |
| Energy CSV ra thẻ SD (header tự ghi, xoay vòng 4 thế hệ) | Xong |
| Lớp thời gian `time_source` (epoch + cờ `S`/`E`/`U` + boot counter) | Xong (chưa có backend) |
| RTC DS1307 | Chưa — chờ phần cứng ([4 bước](energy_logging.md#9-gắn-ds1307-sau-này)) |
| Modbus RTU slave / master | Xong |
| Network: ETH+WiFi failover, AP on-demand, auto-AP | Xong |
| config_store (NVS) | Xong |
| MQTT: broker duy nhất, TLS, publish telemetry/energy/io/heartbeat, LWT | Xong |
| MQTT: subscribe điều khiển relay (`cmd/out0`, `cmd/out1`) | Xong |
| Web Config Portal (mạng/MQTT/RTU master/danh tính; cert upload) | Xong |
| LCD 2004 + nút (menu Settings, RTU, Alarm, DISPLAY & KEYS, MQTT, Energy) | Xong |
| Modbus TCP (hồ sơ EVN ĐMTMN) | Xong — giám sát đầy đủ; điều khiển ở mức nhận + lưu NVS |

---

## 8. Tài liệu liên quan

- [energy_logging.md](energy_logging.md) — tích luỹ energy, NVS, demand, CSV thẻ SD, time_source
- [mqtt_payloads.md](mqtt_payloads.md) — toàn bộ topic + payload MQTT
- [modbus_slave_register_map.md](modbus_slave_register_map.md) — bản đồ thanh ghi Modbus RTU
- [modbus_tcp_evn_map.md](modbus_tcp_evn_map.md) — bản đồ thanh ghi Modbus TCP theo QĐ EVN
- [console_commands.md](console_commands.md) — lệnh console

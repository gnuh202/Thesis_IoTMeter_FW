# Energy Accumulation & Logging

> **Single Source of Truth** cho cách thiết bị tích luỹ năng lượng, lưu bền vững và
> ghi log ra thẻ SD.
> Source: [main/app/energy_meter_task.c](../main/app/energy_meter_task.c),
> [main/app/time_source.c](../main/app/time_source.c),
> [components/sd_card/sd_card.c](../components/sd_card/sd_card.c)

**Phiên bản tài liệu:** 1.0 (2026-09-20)

---

## 📋 Mục lục

1. [Vì sao firmware phải tự tích luỹ](#1-vì-sao-firmware-phải-tự-tích-luỹ)
2. [Lưu bền vững vào NVS](#2-lưu-bền-vững-vào-nvs)
3. [Demand (nhu cầu công suất)](#3-demand-nhu-cầu-công-suất)
4. [Nguồn thời gian (time_source)](#4-nguồn-thời-gian-time_source)
5. [CSV trên thẻ SD](#5-csv-trên-thẻ-sd) — [nhật ký sự cố](#56-nhật-ký-sự-cố-faultscsv)
6. [Các đường xuất dữ liệu khác](#6-các-đường-xuất-dữ-liệu-khác)
7. [Reset: cái gì xoá cái gì](#7-reset-cái-gì-xoá-cái-gì)
8. [Kconfig](#8-kconfig)
9. [Gắn DS1307 sau này](#9-gắn-ds1307-sau-này)
10. [Kiểm thử](#10-kiểm-thử)

---

## 1. Vì sao firmware phải tự tích luỹ

Thanh ghi tổng năng lượng của ATM90E32AS là **read-to-clear** (datasheet Table-11
§5.5.1, kiểu **R/C**):

| Thanh ghi | Địa chỉ | Nội dung |
| --------- | ------- | -------- |
| `APenergyT`  | `0x80` | Active energy import, tổng 3 pha |
| `ANenergyT`  | `0x84` | Active energy export |
| `RPenergyT`  | `0x88` | Reactive energy import |
| `RNenergyT`  | `0x8C` | Reactive energy export |

Mỗi lần đọc, thanh ghi trả về **phần tăng thêm kể từ lần đọc trước** rồi **tự xoá về
0**. IC **không lưu** tổng tích luỹ ở bất cứ đâu. Hệ quả trực tiếp:

> **Bộ đếm trong RAM của firmware CHÍNH LÀ chỉ số công-tơ.** Không có bản sao nào
> khác. Nếu firmware quên cộng dồn, hoặc mất RAM mà chưa kịp ghi NVS, phần năng
> lượng đó **biến mất vĩnh viễn**.

Hệ số quy đổi: `ATM90E32AS_ENERGY_COUNT_TO_WH = 10.0 / 3200.0` Wh mỗi count.

### 1.1. Trần 204.8 Wh mỗi cửa sổ đọc

Count là **uint16**, nên một cửa sổ đọc chứa tối đa
`65535 × 10/3200 ≈ **204.8 Wh**`. Với chu kỳ poll bình thường (`APP_ENERGY_METER_POLL_PERIOD_MS`)
điều này không bao giờ chạm tới. Nhưng **khi AP config portal mở**, vòng đo bị tạm
dừng có chủ đích — mà thời gian ở trong portal là **vô hạn**: tải 2 kW sẽ tràn
thanh ghi sau **~6–7 phút** và mất năng lượng mà không có cảnh báo nào.

Cách xử lý ([energy_meter_task.c:909-935](../main/app/energy_meter_task.c#L909-L935)):
trong config mode vòng lặp vẫn chạy nhưng **chỉ làm một việc — drain thanh ghi
energy mỗi 10 s** rồi cộng vào accumulator. Không đọc measurement, không publish,
không ghi CSV. Đồng thời `s_demand_last_us = 0` để cửa sổ demand **không tính thời
gian ở trong portal là dữ liệu đo**.

### 1.2. Đọc energy độc lập với đọc measurement

Trong thân vòng lặp, `atm90e32as_read_energy_counts()` được gọi **tách rời** khỏi
`atm90e32as_read_measurements()` và không phụ thuộc kết quả của nó: bỏ qua lần đọc
energy chỉ vì một lần đọc khác lỗi sẽ chỉ khiến thanh ghi read-to-clear tràn nhanh
hơn.

### 1.3. Rescale theo CT ratio

Nếu người dùng đổi CT sau khi hiệu chuẩn, delta energy được nhân **cùng hệ số
`nct_scale`** với công suất — nếu không, kWh trên màn hình sẽ mâu thuẫn với kW ngay
cạnh nó.

---

## 2. Lưu bền vững vào NVS

Namespace `energy`, hai key `accum_a` / `accum_b`.

### 2.1. Cấu trúc blob

```c
typedef struct {
    uint32_t magic;               /* 0xE4E59001 */
    uint16_t version;             /* 2 */
    uint16_t seq;                 /* slot mới hơn thắng; wrap vô hại */
    double   active_import_wh;
    double   active_export_wh;
    double   reactive_import_varh;
    double   reactive_export_varh;
    float    demand_max_w;
    uint16_t demand_window_min;
    uint16_t reserved;
    uint32_t crc32;               /* CRC32 trên đúng 48 byte ĐỨNG TRƯỚC nó */
} energy_accum_blob_t;
```

> **Bẫy đã vấp và đã sửa (v1 → v2).** Struct căn lề 8 byte vì có `double`, nên nó
> mang **4 byte padding ở ĐUÔI**: tổng field khai báo là 48 byte nhưng
> `sizeof()` = 56. Vì vậy `sizeof(blob) - sizeof(crc32)` = **52**, rơi quá 4 byte
> và **nuốt luôn chính trường `crc32`** vào vùng tính checksum. Lúc ghi trường đó
> đang bằng 0, lúc đọc nó mang giá trị đã lưu ⇒ **mọi lần load đều sai CRC**, blob
> bị loại âm thầm và công-tơ về 0 sau mỗi lần reboot. Đúng phải là
> `offsetof(energy_accum_blob_t, crc32)`. Một `_Static_assert` neo layout lại để
> không ai chèn field mới làm sinh padding **bên trong** vùng checksum.
>
> `version` nâng lên **2** để blob v1 (CRC tính sai) bị bỏ qua **im lặng** theo
> đường kiểm tra version, thay vì in cảnh báo "failed CRC" đổ oan cho flash.
> Hệ quả: lần boot đầu sau khi nạp bản sửa, công-tơ bắt đầu lại từ 0 **đúng một
> lần** — số cũ vốn chưa bao giờ đọc lại được.

### 2.2. Hai slot luân phiên + CRC32

- `seq` **lẻ → slot A**, **chẵn → slot B**. Mỗi lần ghi đổi slot.
- Mất điện giữa lúc ghi ⇒ slot đang ghi hỏng, **slot còn lại vẫn nguyên**.
- Khi boot, loader đọc cả hai, bỏ qua slot sai magic/version/CRC, rồi chọn slot có
  `seq` lớn nhất. So sánh dùng `(int16_t)(blob.seq - best.seq) > 0` để **an toàn khi
  seq wrap** qua 65535.
- Cả hai slot hỏng (hoặc chưa từng ghi) **không phải lỗi** — chỉ nghĩa là bắt đầu
  từ 0.

### 2.3. Khi nào ghi

`energy_accum_service()` chạy mỗi tick của energy task và ghi khi **một trong hai**
điều kiện đúng:

| Điều kiện | Kconfig | Mặc định |
| --------- | ------- | -------- |
| Năng lượng tích luỹ kể từ lần ghi cuối ≥ ngưỡng | `APP_ENERGY_PERSIST_THRESHOLD_WH` | **50 Wh** |
| Có thay đổi **và** đã quá chu kỳ backstop | `APP_ENERGY_PERSIST_PERIOD_S` | **600 s** |

Ngưỡng theo **năng lượng** (không phải theo thời gian) là chủ ý: hao mòn flash tỉ lệ
với điện năng thực sự tiêu thụ, không phải với thời gian bật máy. Backstop theo thời
gian chỉ để một công-tơ tải rất nhẹ vẫn được ghi định kỳ.

**Mất điện đột ngột có thể mất tối đa ~50 Wh** — đây là con số đã được chấp nhận.

### 2.4. Tính hao mòn flash

Trường hợp xấu nhất, tải liên tục 5 kW:

```
5000 W / 50 Wh mỗi lần ghi  ⇒  1 lần ghi mỗi 36 s
                            ⇒  ~876 000 lần ghi / năm
```

Phân vùng `nvs` 64 KB = **16 sector**, NVS trải đều ⇒ **~880 chu kỳ xoá / sector /
năm**, so với **100 000** chu kỳ chịu đựng của flash ⇒ **> 100 năm**. Không phải vấn
đề kể cả ở tải cực đại.

### 2.5. Các điểm commit ngay (flush)

`energy_meter_flush_persist()` ([energy_meter_task.h](../main/app/energy_meter_task.h))
ghi ngay lập tức, không chờ ngưỡng. Nó được gọi ở **mọi đường reboot có kiểm soát**
và khi vào portal:

| Điểm gọi | File |
| -------- | ---- |
| Console `reboot` | [console_task.c](../main/app/console_task.c) |
| Console `cfg-reset` (factory reset) | [console_task.c](../main/app/console_task.c) |
| Web portal apply → deferred reboot | [web_portal.c](../main/app/web_portal.c) |
| Modbus HR7 `RebootDevice` = `0x5AA5` | [modbus_slave_task.c](../main/app/modbus_slave_task.c) |
| Modbus restart task | [modbus_slave_task.c](../main/app/modbus_slave_task.c) |
| **Mở AP config portal** | [network_manager.c](../main/app/network_manager.c) |
| Reset Energy / Reset Demand (LCD, Modbus) | [energy_meter_task.c](../main/app/energy_meter_task.c) |

Riêng hai lệnh reset commit ngay vì lý do khác: người vận hành xoá công-tơ rồi rút
điện **không được** thấy số cũ quay lại ở lần boot sau.

`energy_meter_flush_persist()` cũng gọi `time_source_flush()` — mốc thời gian và chỉ
số công-tơ luôn được commit cùng nhau.

---

## 3. Demand (nhu cầu công suất)

Tính bằng **tích phân theo thời gian**, không phải trung bình cộng số mẫu:

```
accum_ws     += P_total × dt          (dt = khoảng cách thực giữa 2 tick)
elapsed_s    += dt
khi elapsed_s ≥ window_min × 60:
    demand_w  = accum_ws / elapsed_s
    demand_max_w = max(demand_max_w, demand_w)
    reset accum_ws, elapsed_s
```

Vì sao không đếm mẫu: nếu vài tick bị bỏ lỡ (task bận, SPI lỗi), đếm mẫu sẽ làm cửa
sổ "15 phút" dài hơn 15 phút thật. Tích phân giữ đúng thời gian thực.

**Khoảng trống dài hơn 2× chu kỳ poll bị loại bỏ**, không ngoại suy — *không có dữ
liệu tốt hơn dữ liệu bịa*. Đây cũng là cơ chế khiến thời gian ở trong AP portal
không làm bẩn cửa sổ demand.

Cửa sổ mặc định **15 phút**, đổi bằng Modbus HR2 hoặc
`energy_meter_set_demand_window_minutes()`. Đổi cửa sổ **huỷ cửa sổ đang chạy** (zero
`accum_ws`/`elapsed_s`) — một cửa sổ nửa cũ nửa mới không có ý nghĩa vật lý.

`demand_max_w` và `demand_window_min` nằm trong blob NVS nên sống sót qua reboot.

---

## 4. Nguồn thời gian (time_source)

**Thiết bị chưa gắn DS1307.** Mọi consumer cần ngày giờ đều đi qua
[time_source.h](../main/app/time_source.h) thay vì gọi `time()`/`localtime()` trực
tiếp — đó chính là mục đích của lớp này: backend đổi mà không consumer nào phải sửa.

### 4.1. Không có stub, và đó là chủ ý

ESP-IDF để đồng hồ hệ thống ở epoch cho tới khi có ai gọi `settimeofday()`. Vì chưa
có backend nào, `localtime()` **tự nhiên** trả về `1970-01-01` cộng thêm uptime. Đây
**không phải code giả để xoá sau**: thiết bị không bao giờ bịa ra một ngày tháng
trông có vẻ hợp lệ, và cờ chất lượng nói rõ cho người đọc log biết tin được tới đâu.

### 4.2. Cờ chất lượng (`tq`)

| Cờ | enum | Ý nghĩa |
| -- | ---- | ------- |
| `'U'` | `TIME_QUALITY_NONE` | Uptime-only. Phần ngày tháng **vô nghĩa** |
| `'E'` | `TIME_QUALITY_ESTIMATE` | Khôi phục mốc NVS, đang trôi. Ngày là **cận dưới** |
| `'S'` | `TIME_QUALITY_SYNCED` | RTC hoặc NTP — tin được |

Cờ này được publish **nguyên vẹn** ở ba nơi: cột `tq` của CSV, trường `tq` của MQTT
heartbeat, và Modbus IR 112 (mã ASCII).

### 4.3. Trục thời gian tin cậy khi `tq = 'U'`

- `boot` — bộ đếm số lần khởi động, lưu NVS (namespace `timekeep`, key `boots`).
  **Cách duy nhất** phân biệt hai phiên chạy khi mọi dòng đều ghi cùng một ngày.
- `uptime_s` — thứ tự các dòng **trong cùng một phiên**.

Cặp `(boot, uptime_s)` sắp xếp đúng mọi dòng log kể cả khi chưa có RTC.

> **Vì sao `ts` không về 0 sau `reboot` mềm.** ESP32 giữ bộ đếm RTC xuyên qua
> `esp_restart()`, nên đồng hồ hệ thống **đếm tiếp** sau reboot mềm; chỉ **cúp
> điện** hoặc **nhấn nút reset cứng** (kéo chân EN) mới xoá miền RTC và đưa `ts`
> về 0. Đây là hành vi đúng, không phải lỗi — và nó cũng giải thích vì sao
> `uptime_s` (về 0 mỗi lần boot) và `ts` lệch nhau sau reboot mềm. Khi `tq = 'U'`,
> **chỉ `boot` mới phân biệt được phiên chạy**, `ts` thì không.

### 4.4. Mốc thời gian (epoch floor)

Khi đã có backend, `time_source_service()` ghi epoch hiện tại xuống NVS mỗi
`APP_TIME_NVS_SAVE_PERIOD_S` (mặc định 600 s). Lần boot sau, giá trị đó được khôi
phục làm **cận dưới** và chất lượng là `'E'`. Bất kỳ epoch nào nhỏ hơn
`TIME_SOURCE_EPOCH_MIN` (2020-01-01 UTC) bị coi là "chưa từng đặt" — chặn trường hợp
NVS hỏng khôi phục thành một ngày trông hợp lệ.

Múi giờ: `APP_TIME_TZ`, mặc định `"ICT-7"` (UTC+7, không DST). Timestamp **hiển thị**
(CSV, LCD, console) là giờ địa phương; MQTT và Modbus mang **epoch thô**.

---

## 5. CSV trên thẻ SD

Đường dẫn: **`/sdcard/ENERGY/ENERGY.CSV`**. Chu kỳ ghi: `APP_ENERGY_SD_LOG_PERIOD_S`,
mặc định **300 s** (~1 MB/năm).

### 5.1. Schema — 13 cột, KHÔNG được đổi thứ tự

```
timestamp,tq,boot,uptime_s,imp_kwh,exp_kwh,imp_kvarh,exp_kvarh,dmd_w,dmd_max_w,p_kw,pf,freq
```

| # | Cột | Định dạng | Đơn vị | Ý nghĩa |
|---|-----|-----------|--------|---------|
| 1 | `timestamp` | `YYYY-MM-DD HH:MM:SS` | giờ địa phương | **Chỉ tin khi `tq` ≠ `U`** |
| 2 | `tq` | 1 ký tự | — | Time quality: `S` / `E` / `U` |
| 3 | `boot` | uint | — | Số lần khởi động |
| 4 | `uptime_s` | uint | giây | Thời gian từ lúc boot |
| 5 | `imp_kwh` | `%.3f` | kWh | Active energy import |
| 6 | `exp_kwh` | `%.3f` | kWh | Active energy export |
| 7 | `imp_kvarh` | `%.3f` | kvarh | Reactive energy import |
| 8 | `exp_kvarh` | `%.3f` | kvarh | Reactive energy export |
| 9 | `dmd_w` | `%.1f` | W | Demand cửa sổ gần nhất |
| 10 | `dmd_max_w` | `%.1f` | W | Demand đỉnh kể từ lần reset |
| 11 | `p_kw` | `%.3f` | kW | Công suất tác dụng tổng tức thời |
| 12 | `pf` | `%.3f` | — | Hệ số công suất hệ thống |
| 13 | `freq` | `%.2f` | Hz | Tần số lưới |

Mỗi dòng ~100 byte. **Công cụ phía PC đọc theo vị trí cột**, nên thứ tự là hợp đồng
cố định — thêm cột mới phải **nối vào cuối**.

> **Không có cột `warn_bits`.** Sự cố là **sự kiện tức thời**, không phải đại lượng
> lấy mẫu: một cột trong dòng 5 phút sẽ bỏ sót mọi sự cố ngắn hơn chu kỳ đó và
> không nói được sự cố xảy ra **lúc nào**. Sự cố có file riêng —
> [§5.6 FAULTS.CSV](#56-nhật-ký-sự-cố-faultscsv).

Ví dụ (khi chưa có RTC):

```csv
timestamp,tq,boot,uptime_s,imp_kwh,exp_kwh,imp_kvarh,exp_kvarh,dmd_w,dmd_max_w,p_kw,pf,freq
1970-01-01 07:05:00,U,17,300,12.345,0.000,3.210,0.000,1820.4,5010.0,1.834,0.982,50.01
1970-01-01 07:10:00,U,17,600,12.498,0.000,3.244,0.000,1836.1,5010.0,1.841,0.981,49.99
```

Giờ hiển thị là `07:05` chứ không phải `00:05` vì `TZ=ICT-7` cộng thêm 7 giờ vào
epoch 0 — hoàn toàn bình thường, `tq=U` đã nói rõ đây không phải ngày giờ thật.

### 5.2. Header tự ghi

Header được ghi tự động **mỗi khi file có kích thước 0**: thẻ mới, sau khi xoay
vòng, hoặc người dùng tự xoá file. Nhờ vậy tráo thẻ giữa chừng vẫn cho ra file
tự-mô-tả.

### 5.3. Xoay vòng theo kích thước

Khi `ENERGY.CSV` vượt `APP_ENERGY_SD_LOG_MAX_KB` (mặc định **8192 KB = 8 MB**):

```
ENERGY.003  →  xoá
ENERGY.002  →  ENERGY.003
ENERGY.001  →  ENERGY.002
ENERGY.CSV  →  ENERGY.001
(file mới, header tự ghi lại)
```

Giữ **4 thế hệ**, trần ~32 MB. Đặt `max_kb = 0` để tắt xoay vòng.

**Vì sao xoay theo kích thước chứ không theo ngày:** tên file theo ngày cần một đồng
hồ đáng tin — thứ thiết bị chưa có. Xoay theo kích thước đúng trong mọi trường hợp,
kể cả khi `tq = 'U'`.

### 5.4. Thẻ rút ra là trạng thái bình thường

Thẻ hỗ trợ hot-plug. Không có thẻ ⇒ **bỏ qua dòng log, không phải lỗi**; chỉ log
chuyển trạng thái **một lần mỗi lượt** (`no SD card mounted; energy CSV logging
paused` / `SD card back; energy CSV logging resumed`), không spam mỗi chu kỳ.

### 5.5. Ghi FAT nằm ngoài mutex

Một lần append FAT có thể mất hàng chục ms. Giữ `s_measurements_mutex` xuyên qua đó
sẽ chặn MQTT, Modbus và LCD. Vì vậy: **chụp snapshot dưới mutex, ghi sau khi nhả**.

SD dùng SDSPI trên **cùng bus SPI với ATM90E32AS** — đó là lý do việc ghi CSV nằm
trong energy task chứ không phải một task riêng.

### 5.6. Nhật ký sự cố: `FAULTS.CSV`

Đường dẫn: **`/sdcard/EVENTS/FAULTS.CSV`**. Ghi **theo sự kiện**, không theo chu kỳ.

> **File này chỉ chứa sự cố lưới điện.** Không có sự kiện của mạch (boot, gắn/rút
> thẻ, mạng lên/xuống, thao tác cấu hình). Người vận hành mở file ra phải thấy
> đúng một thứ: **lưới đã xảy ra chuyện gì, lúc nào**. Sự kiện mạch nằm ở log
> console/UART, không lẫn vào đây.

Schema 8 cột — bốn cột đầu **trùng trục thời gian của `ENERGY.CSV`** nên ghép hai
file lại được ngay cả khi `tq = 'U'`:

```
timestamp,tq,boot,uptime_s,event,type,phase,value
```

| # | Cột | Ý nghĩa |
|---|-----|---------|
| 1-4 | `timestamp`,`tq`,`boot`,`uptime_s` | Giống `ENERGY.CSV` §5.1 |
| 5 | `event` | `FAULT` = sự cố bắt đầu (đã chốt latch) · `CLEAR` = điều kiện lưới đã hết |
| 6 | `type` | `OV` · `UV` · `OC` · `PLOSS` · `FREQ_HI` · `FREQ_LO` · `IC_ERR` |
| 7 | `phase` | `A`/`B`/`C`, hoặc `-` với lỗi toàn hệ (tần số, IC) |
| 8 | `value` | Đại lượng gây ra: V (OV/UV/PLOSS) · A (OC) · Hz (FREQ) |

```csv
timestamp,tq,boot,uptime_s,event,type,phase,value
1970-01-01 07:12:30,U,17,750,FAULT,OC,A,7.12
1970-01-01 07:14:05,U,17,845,CLEAR,OC,A,3.90
```

Quy tắc ghi:

- **`FAULT`** ghi đúng tại **cạnh chốt latch** — tức là sau khi điều kiện đã duy
  trì liên tục hết cửa sổ xác nhận (`alarm_trigger_delay_s`). Nhiễu thoáng qua
  không sinh dòng nào.
- **`CLEAR`** ghi khi bit `active` của IC tắt. Nó **độc lập với Reset Latch**: xoá
  latch là thao tác của người vận hành trên tủ, không phải chuyện của lưới, nên
  không được phép đóng một sự cố mà lưới vẫn đang có. Firmware giữ bitmap riêng
  (`s_fault_logged`) cho việc này.
- Mỗi pha một dòng riêng: mất 3 pha cùng lúc ⇒ 3 dòng `PLOSS`, biết rõ pha nào.
- Ghi từ energy task, **không giữ mutex**, và chỉ ở cạnh — chi phí FAT không bao
  giờ rơi vào chu kỳ đo bình thường.
- Không có thẻ ⇒ bỏ qua, không phải lỗi (giống `ENERGY.CSV`).

Bảng ý nghĩa từng bit alarm: [mqtt_payloads.md §3.1](mqtt_payloads.md#alarm-warnings-và-warn_bits).

---

## 6. Các đường xuất dữ liệu khác

Cùng một bộ số liệu đi ra bốn hướng:

| Đích | Nội dung | Tài liệu |
| ---- | -------- | -------- |
| **MQTT** `pm/<id>/energy` | `imp_kwh`, `exp_kwh`, `imp_kvarh`, `exp_kvarh`, `dmd_w`, `dmd_max_w` | [mqtt_payloads.md §3.2](mqtt_payloads.md#32-pmidenergy) |
| **MQTT** `pm/<id>/heartbeat` | `ts`, `tq`, `boot`, `uptime_s` | [mqtt_payloads.md §3.4](mqtt_payloads.md#34-pmidheartbeat) |
| **Modbus** IR 60–66, 74–76 | energy + demand (float) | [modbus_slave_register_map.md §3.10](modbus_slave_register_map.md) |
| **Modbus** IR 110–113 | epoch, time quality, boot count | [modbus_slave_register_map.md §7.1](modbus_slave_register_map.md) |
| **Thẻ SD** `/ENERGY/ENERGY.CSV` | cả 13 cột | §5 ở trên |
| **Thẻ SD** `/EVENTS/FAULTS.CSV` | sự cố lưới (FAULT/CLEAR) | [§5.6](#56-nhật-ký-sự-cố-faultscsv) |
| **LCD** | trang Energy, trang Demand | — |

---

## 7. Reset: cái gì xoá cái gì

| Thao tác | Energy | Demand + Max | Cấu hình |
| -------- | ------ | ------------ | -------- |
| **LCD → Settings → Energy → Reset Energy** | ✅ xoá | — | — |
| **LCD → Settings → Energy → Reset Demand** | — | ✅ xoá | — |
| Modbus HR3 `ResetEnergy` | ✅ xoá | — | — |
| Modbus HR4 `ResetDemand` | — | ✅ xoá | — |
| **Factory Reset** (LCD / console `cfg-reset`) | ❌ **giữ nguyên** | ❌ giữ nguyên | ✅ xoá |
| Reboot (mọi loại) | ❌ giữ nguyên | ❌ giữ nguyên | ❌ giữ nguyên |

> **Factory Reset KHÔNG xoá chỉ số công-tơ.** Đây là quyết định thiết kế: chỉ số đo
> là **kết quả đo**, không phải một tuỳ chọn cấu hình. Xoá nó phải là một hành động
> riêng, có xác nhận, và hiện rõ giá trị sắp mất.

Hai mục LCD đều hiện giá trị hiện tại trước khi hỏi xác nhận (`Now: 12.35 kWh` /
`Peak: 5010 W`), vì thao tác này **không hoàn tác được** — không có bản sao nào khác
tồn tại.

---

## 8. Kconfig

`menuconfig → Application → Energy meter`:

| Symbol | Mặc định | Range | Ý nghĩa |
| ------ | -------- | ----- | ------- |
| `APP_ENERGY_PERSIST_THRESHOLD_WH` | 50 | 10–1000 | Ngưỡng Wh kích hoạt ghi NVS = năng lượng tối đa có thể mất khi cúp điện |
| `APP_ENERGY_PERSIST_PERIOD_S` | 600 | 60–3600 | Backstop theo thời gian, chỉ ghi khi có thay đổi |
| `APP_ENERGY_SD_LOG_ENABLE` | y | — | Bật ghi CSV |
| `APP_ENERGY_SD_LOG_PERIOD_S` | 300 | 10–3600 | Chu kỳ một dòng CSV |
| `APP_ENERGY_SD_LOG_MAX_KB` | 8192 | 64–65536 | Ngưỡng xoay vòng |

`menuconfig → Application → Time`:

| Symbol | Mặc định | Ý nghĩa |
| ------ | -------- | ------- |
| `APP_TIME_TZ` | `"ICT-7"` | Chuỗi TZ POSIX cho mọi timestamp hiển thị |
| `APP_TIME_NVS_SAVE_PERIOD_S` | 600 | Chu kỳ ghi mốc epoch xuống NVS |
| `APP_TIME_RTC_ENABLE` | n | **Chưa thực thi** — phần cứng chưa gắn |
| `APP_TIME_RTC_I2C_ADDR` | `0x68` | Địa chỉ cố định của DS1307 |

---

## 9. Gắn DS1307 sau này

Bốn bước, **không consumer nào phải sửa**:

1. Tạo `components/ds1307/` — I2C `0x68` trên bus dùng chung sẵn có
   (`i2c_bus_get_handle()`, cùng bus với PCF8574 / PCF8575 / LCD).
2. Trong `time_source_init()`, dưới `CONFIG_APP_TIME_RTC_ENABLE`, đọc chip và gọi
   `time_source_set(epoch, TIME_SOURCE_RTC)`.
3. Bật `APP_TIME_RTC_ENABLE=y`.
4. Thêm màn hình LCD **Settings → Time → Set Clock**: ghi chip rồi gọi
   `time_source_set()`.

Sau đó: cột `timestamp` của CSV ra ngày thật, `tq` thành `'S'`, MQTT `ts` và Modbus
IR 110–111 đúng ngay. **Schema CSV, payload MQTT và register map giữ nguyên
byte-for-byte** — không có bước migration nào.

---

## 10. Kiểm thử

| # | Kịch bản | Kỳ vọng |
|---|----------|---------|
| 1 | Chạy tích luỹ vài kWh → `reboot` từ console | Chỉ số sau boot **bằng** chỉ số trước reboot |
| 2 | Tích luỹ → **rút điện đột ngột** | Mất tối đa ~50 Wh (một ngưỡng), không bao giờ về 0 |
| 3 | Rút điện **đúng lúc đang ghi NVS** (lặp nhiều lần) | Không bao giờ hỏng dữ liệu — slot kia còn nguyên |
| 4 | Ép lỗi đọc `read_energy_counts` | Các lần đọc sau vẫn cộng dồn đúng, không mất count |
| 5 | Mở AP portal **10 phút** với tải 2 kW | Năng lượng của 10 phút đó **được ghi nhận đầy đủ** (drain 10 s) |
| 6 | Thoát AP portal | Đo/publish/CSV resume ngay, không có burst lỗi |
| 7 | Rút thẻ SD khi đang chạy → cắm lại | Log `paused` **một lần**, cắm lại log `resumed`, file tiếp tục đúng |
| 8 | Thẻ mới toanh | Header tự ghi ở dòng đầu |
| 9 | Ép `max_kb` nhỏ (ví dụ 64) | Xoay vòng đúng `.001/.002/.003`, file mới có header |
| 10 | Đổi cửa sổ demand qua HR2 | Cửa sổ đang chạy bị huỷ, cửa sổ mới dài đúng số phút mới |
| 11 | LCD → Energy → Reset Energy → rút điện ngay | Sau boot chỉ số vẫn là **0**, không quay lại số cũ |
| 12 | Factory Reset | Cấu hình về mặc định, **energy giữ nguyên** |
| 13 | Đọc Modbus IR 110–113 | `TimeQuality` = 85 (`'U'`), `BootCount` tăng mỗi lần boot |
| 14 | Subscribe MQTT heartbeat | Có `ts` / `tq` / `boot`; `tq` = `"U"` |
| 15 | Ép quá dòng một pha quá cửa sổ xác nhận | `FAULTS.CSV` có **một** dòng `FAULT,OC,<pha>` với giá trị dòng thật |
| 16 | Bỏ tải sau khi đã chốt | Thêm dòng `CLEAR,OC,<pha>`; **không** cần Reset Latch mới có |
| 17 | Reset Latch trong lúc sự cố còn | Không sinh dòng nào; khi lưới hết sự cố vẫn ra đúng một `CLEAR` |
| 18 | Boot / rút-cắm thẻ / mất mạng | `FAULTS.CSV` **không** có thêm dòng nào |

---

## Related Documentation

- **MQTT Payloads:** [mqtt_payloads.md](mqtt_payloads.md)
- **Modbus Register Map:** [modbus_slave_register_map.md](modbus_slave_register_map.md)
- **Architecture Overview:** [architecture.md](architecture.md)
- **Console Commands:** [console_commands.md](console_commands.md)
- **SD Card:** [sd_card_fixes.md](sd_card_fixes.md), [sd_card_ram_usage.md](sd_card_ram_usage.md)
</content>
</invoke>

# Hướng dẫn Console & Calib ATM90E32AS

Tài liệu này mô tả cách dùng console (esp_console) để hiệu chỉnh (calibrate) IC đo công suất **ATM90E32AS** mà **không cần nạp lại firmware**. Giá trị calib được lưu vào **NVS**, còn sau khi reboot.

---

## 1. Kết nối console

Console chạy trên **USB Serial/JTAG** (cổng USB on-board của ESP32-S3).

Cấu hình liên quan trong [sdkconfig](../sdkconfig):

```
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
CONFIG_ESP_CONSOLE_SECONDARY_NONE=y
```

Mở terminal:

```
idf.py -p <PORT> flash monitor
```

### Đăng nhập (mặc định bật)

Console là **tool cho nhà phát triển**, nên mặc định có cổng đăng nhập. Sau boot sẽ thấy:

```
=== Power Meter developer console ===
Login required.
username:
password:
```

Tài khoản mặc định (đổi trong `menuconfig` → *Application Configuration → Console Calibration*):

| Config                            | Mặc định      |
| --------------------------------- | ------------- |
| `APP_CONSOLE_AUTH_USERNAME`       | `admin`       |
| `APP_CONSOLE_AUTH_PASSWORD`       | `meter-admin` |
| `APP_CONSOLE_AUTH_MAX_ATTEMPTS`   | `3`           |

- Sai quá số lần cho phép → console **khóa tới khi reboot**.
- Tắt hẳn login: bỏ `APP_CONSOLE_AUTH_ENABLE`.
- Lưu ý: mật khẩu truyền **cleartext** qua serial — chỉ để chặn người dùng cuối, không phải bảo mật mã hóa.
- Việc đăng nhập chạy ở **task riêng**, không ảnh hưởng đo lường / Modbus / WiFi (các dịch vụ này luôn chạy dù chưa login console).

Đăng nhập thành công sẽ thấy prompt:

```
meter>
```

Gõ `help` để liệt kê toàn bộ lệnh.

> Ghi chú: mặc định log đo từng mẫu bị **tắt** (`CONFIG_APP_ENERGY_METER_LOG_EACH_SAMPLE`) để console không bị rối. Muốn xem log mỗi lần đo thì bật lại trong `menuconfig`.

---

## 2. Danh sách lệnh

### 2.1. Đọc số đo

```
meter-latest
```

In toàn bộ số đo mới nhất: V/I/P/Q/S/PF/góc pha từng pha, dòng trung tính, tổng, tần số, nhiệt độ, thanh ghi trạng thái.

### 2.2. Truy cập thanh ghi thô

```
meter-reg read <addr>
meter-reg write <addr> <value>
```

Ví dụ:

```
meter-reg read 0x61
meter-reg write 0x61 0x1C89
```

`addr` và `value` nhận cả hex (`0x..`) lẫn thập phân.

### 2.3. Xem / nạp / lưu calib

```
meter-cal show            # xem calib hiện tại trong RAM
meter-cal guide           # in hướng dẫn calib ngắn ngay trên console
meter-cal apply           # ghi calib hiện tại xuống chip
meter-cal save            # lưu calib xuống NVS (chỉ lưu sau khi verify)
meter-cal load --apply    # nạp calib từ NVS và ghi xuống chip
meter-cal default --apply # trả calib về mặc định bring-up rồi ghi xuống chip
```

### 2.4. Đặt giá trị calib

Cú pháp chung:

```
meter-cal set --field <field> --phase <a|b|c> [tham số] [--apply]
```

| field          | tham số dùng          | ý nghĩa                                  |
| -------------- | --------------------- | ---------------------------------------- |
| `gain`         | `--u`, `--i`          | voltage gain / current gain              |
| `offset`       | `--u`, `--i`          | voltage offset / current offset          |
| `power-offset` | `--p`, `--q`          | active / reactive power offset           |
| `phase`        | `--phi`               | bù pha (phase compensation)              |
| `ref`          | `--u`, `--i`          | điện áp/dòng tham chiếu (chỉ lưu, không ghi chip) |

- `--apply`: ghi ngay xuống chip sau khi set. Không có `--apply` thì chỉ đổi trong RAM (phải gọi `meter-cal apply` sau).
- Không truyền tham số nào thì giữ nguyên giá trị cũ của field đó.

Ví dụ:

```
meter-cal set --field gain --phase a --u 7305 --i 27961 --apply
meter-cal set --field offset --phase a --u 0 --i 0 --apply
meter-cal set --field power-offset --phase a --p 0 --q 0 --apply
meter-cal set --field ref --phase a --u 220 --i 5
```

### 2.5. Cấu hình chế độ (chip-wide, KHÔNG dùng `--phase`)

Các field sau áp cho toàn chip, không theo pha, dùng `--value`:

```
meter-cal set --field pga    --value 1|2|4      --apply
meter-cal set --field wiring --value 3p4w|3p3w  --apply
meter-cal set --field freq   --value 50|60      --apply
```

| field    | value        | ý nghĩa                                                        |
| -------- | ------------ | -------------------------------------------------------------- |
| `pga`    | `1`/`2`/`4`  | PGA gain **kênh dòng** (MMode1). Đổi xong phải calib lại igain. |
| `wiring` | `3p4w`/`3p3w`| Kiểu đấu dây (MMode0). Khi `--apply` sẽ **kéo relay MODE_SEL** tương ứng. |
| `freq`   | `50`/`60`    | Tần số lưới (MMode0 + ngưỡng freq).                            |

Lưu ý:
- `pga` là gain của **kênh dòng** (CT/shunt), **không** phải kênh áp. Dùng để tận dụng dải ADC cho dòng nhỏ/lớn. Đổi PGA thì hệ số số igain thay đổi → **phải calib lại current gain**.
- `wiring 3p3w`: chip lấy pha B làm chung; áp dây lên ~380V. **Toàn thang áp do mạch chia áp phần cứng + ugain quyết định**, không phải PGA. Phải chắc divider chịu được 380V và calib lại ugain cho tầm mới.
- Đổi 3 field này rồi nhớ `meter-cal save` để giữ qua reboot (blob NVS đã bao gồm pga/wiring/freq).

Ví dụ chuyển thiết bị sang khu 3 pha 3 dây:

```
meter-cal set --field wiring --value 3p3w --apply   # relay tự sang mode 3 dây
meter-cal set --field freq --value 50 --apply
# calib lại ugain/igain cho tầm áp/dòng mới, rồi:
meter-cal save
```

---

## 3. Quy trình calib chuẩn

> **Quan trọng (luận văn):** giá trị mặc định chỉ để kiểm tra thông SPI, **không phải** hiệu chỉnh đo lường. Bắt buộc calib bằng nguồn/tải chuẩn (đồng hồ chuẩn, nguồn AC chuẩn).

### Bước 0 — Kiểm tra giao tiếp

```
help
meter-latest
```

Nếu đọc được số (dù chưa đúng) là SPI đã thông.

### Bước 1 — Voltage gain

Cấp một điện áp đã biết `ref_V` (ví dụ 220V) vào pha A.

1. Xem gain hiện tại:
   ```
   meter-cal show
   ```
   Ghi lại `ugain` (gọi là `old_ugain`).
2. Đọc điện áp đo được:
   ```
   meter-latest
   ```
   Lấy `measured_V` của pha A.
3. Tính gain mới:
   ```
   new_ugain = round(old_ugain * ref_V / measured_V)
   ```
4. Ghi:
   ```
   meter-cal set --field gain --phase a --u <new_ugain> --apply
   ```
5. Kiểm tra lại `meter-latest`. Lặp lại tinh chỉnh nếu cần.

Làm tương tự cho pha **B**, **C**.

### Bước 2 — Current gain

Cấp một dòng/tải đã biết `ref_I` (ví dụ 5A).

1. `meter-cal show` → lấy `old_igain`.
2. `meter-latest` → lấy `measured_I`.
3. Tính:
   ```
   new_igain = round(old_igain * ref_I / measured_I)
   ```
4. Ghi:
   ```
   meter-cal set --field gain --phase a --i <new_igain> --apply
   ```
5. Kiểm tra `meter-latest`. Lặp cho **B**, **C**.

### Bước 3 — Offset (nếu cần)

Chỉ đặt từ điều kiện **không tải / zero thực đo**, không đoán:

```
meter-cal set --field offset --phase a --u <v> --i <v> --apply
```

### Bước 4 — Power / phase offset (nếu cần)

Cần đồng hồ chuẩn + tải có PF đã biết:

```
meter-cal set --field power-offset --phase a --p <n> --q <n> --apply
meter-cal set --field phase --phase a --phi <n> --apply
```

### Bước 5 — Kiểm tra & lưu

1. Kiểm tra tổng thể:
   ```
   meter-latest
   ```
2. Chỉ khi đã đúng, lưu vĩnh viễn:
   ```
   meter-cal save
   ```
3. Reboot → firmware **tự nạp** calib từ NVS. Muốn nạp lại thủ công:
   ```
   meter-cal load --apply
   ```

### Làm lại từ đầu

```
meter-cal default --apply
```

(giữ nguyên tần số lưới và kiểu đấu dây 3P3W/3P4W đã cấu hình).

---

## 4. Công thức nhanh

| Đại lượng      | Công thức                                        |
| -------------- | ------------------------------------------------ |
| Voltage gain   | `new = round(old * V_ref / V_measured)`          |
| Current gain   | `new = round(old * I_ref / I_measured)`          |

Ví dụ: `old_ugain = 7305`, cấp `ref_V = 220V`, đo được `measured_V = 218.5V`:

```
new_ugain = round(7305 * 220 / 218.5) = round(7355.1) = 7355
meter-cal set --field gain --phase a --u 7355 --apply
```

---

## 5. Nơi lưu calib

- Namespace NVS: `atm90e32as`
- Key: `calib_v1`
- Có magic + version để chống dữ liệu hỏng/cũ.
- Boot: tự load từ NVS; nếu chưa có, log cảnh báo và dùng default bring-up.

---

## 6. Lưu ý phần cứng

- ATM90E32AS dùng SPI **chung** với thẻ SD (MISO 16, SCK 17, MOSI 18); CS ATM90E32AS = 38, CS SD = 8.
- Chân MODE_SEL (GPIO21): `0` = 3 pha 4 dây (mặc định), `1` = 3 pha 3 dây. Chân này dùng để **kéo relay** chuyển đấu dây; `meter-cal set --field wiring ... --apply` sẽ tự set mức tương ứng. Mạch relay thực tế define sau (hiện chỉ điều khiển GPIO).
- Cấu hình chế độ (pga/wiring/freq) có thể chỉnh bằng **console** (đã có) hoặc **HMI LCD2004 + 5 nút** (sẽ thêm sau khi có mạch) — cả hai cùng đi qua facade `energy_meter_set/apply/save_calibration`, nên logic dùng chung.
- Nguồn cấp cho ATM90E32AS phải ổn định; nguồn kém dễ gây sai số hoặc lỗi SPI.

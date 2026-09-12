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
| `APP_CONSOLE_AUTH_PASSWORD`       | `admin`       |
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
meter-cal default --apply # trả TOÀN BỘ calib về mặc định bring-up rồi ghi xuống chip
meter-cal default --field phi [--phase a|b|c]
                          # reset CHỌN LỌC một field về baseline, ÁP NGAY XUỐNG CHIP
                          # (không cần --apply); bỏ --phase = cả 3 pha.
                          # field: phi|pqgain|uigain|uioffset|power-offset|fundamental|all
                          # (alias: phase, pq-gain, gain, offset, fundamental-power-gain). Xem §4.3.
```

> **Auto-apply:** `default --field <f>` (per-phase reset) **ghi chip ngay** — `--apply` là no-op. Chỉ `default` KHÔNG kèm `--field` (reset toàn image) mới cần `--apply`.

### 2.4. Đặt giá trị calib

Cú pháp chung:

```
meter-cal set --field <field> --phase <a|b|c> [tham số] [--apply]
```

| field                    | tham số dùng          | ý nghĩa                                  |
| --------------           | --------------------- | ---------------------------------------- |
| `uigain` (alias `gain`)  | `--u`, `--i`          | voltage gain / current gain              |
| `uioffset` (alias `offset`) | `--u`, `--i`       | voltage offset / current offset          |
| `power-offset`           | `--p`, `--q`          | active / reactive power offset           |
| `phase`                  | `--phi`               | bù pha (phase compensation)            |
| `pq-gain`                | `--value`             | active/reactive power gain (PQGain)      |
| `fundamental-power-gain` | `--value`             | fundamental active power gain (PGainF)   |

- **Auto-apply (per-phase):** mọi field calib theo pha ở trên **ghi ngay xuống chip** khi gọi lệnh — KHÔNG cần `--apply`. Người vận hành thấy thay đổi tức thì qua `meter-latest`. Cờ `--apply` vẫn được chấp nhận nhưng là **no-op** ở đây.
- **Chỉ chip-wide mới cần `--apply`:** `pga|wiring|freq` (xem §2.5) và `default` không `--field` (reset toàn bộ image) vẫn giữ gate `--apply`.
- Không truyền tham số nào thì giữ nguyên giá trị cũ của field đó.
- `save` tách riêng: calib áp vào chip ngay, nhưng chỉ bền qua reboot sau `meter-cal save`.
- Vref/Iref đã gỡ: auto-cal nhận ref một lần qua `meter-cal auto`, không lưu trong calib blob.

Ví dụ (không cần `--apply`):

```
meter-cal set --field uigain --phase a --u 7305 --i 27961
meter-cal set --field uioffset --phase a --u 0 --i 0
meter-cal set --field power-offset --phase a --p 0 --q 0
meter-cal set --field pq-gain --phase a --value 5237
meter-cal set --field fundamental-power-gain --phase a --value 100
```

### 2.5. Cấu hình chế độ (chip-wide, KHÔNG dùng `--phase`)


## 8. SPI-only power calibration notes

Board không nối các chân CF/pulse output; toàn bộ hiệu chỉnh và xác minh công suất dùng thanh ghi ATM90E32AS qua SPI. Không sử dụng đồng hồ tham chiếu Modbus trong quy trình nhà máy.

### Offset P/Q không tải

Dùng điều kiện không tải và thực hiện riêng từng pha:

```text
meter-cal auto-power-offset --field p --phase a
meter-cal auto-power-offset --field q --phase a
```

Firmware tạm đưa offset tương ứng về zero, chờ ổn định, đọc nhiều mẫu signed 32-bit `Pmean` hoặc `Qmean`, rồi tính `new_offset = -round(average_raw_counts)`. Offset phải nằm trong `int16_t`; firmware ghi, đọc lại và kiểm tra residual raw counts. Nếu thất bại, calibration cũ được rollback. Lặp lại cho B/C, sau đó chạy `meter-cal save` chỉ khi đã xác minh.

### Quy ước raw power

`Pmean`, `Qmean`, `Smean` là signed 32-bit; mỗi count tương đương `0.00032 W/var/VA`. Không dùng chuyển đổi RMS hoặc làm tròn float để tính P/Q offset. `PQGain` là correction gain có dấu; zero nghĩa là không hiệu chỉnh thêm. `Phi` được lưu logic signed và mã hóa thành magnitude 8-bit cùng sign bit khi ghi. Tải thuần trở PF xấp xỉ 1 phù hợp để kiểm tra active power/PQGain, nhưng không đủ để xác định độc lập `Phi`; muốn hiệu chỉnh Phi cần fixture PF khoảng 0.5 lagging.

### Quy trình xưởng

1. Cấp nguồn, kiểm tra `meter-latest` và SPI.
2. Chọn đúng wiring, tần số và PGA.
3. Hiệu chỉnh offset U/I ở điều kiện zero nếu cần.
4. Dùng tải thuần trở bất kỳ cùng đồng hồ chuẩn để hiệu chỉnh U/I gain và kiểm tra active power.
5. Chạy P/Q offset khi không tải; kiểm tra active-power/PQGain với tải và đồng hồ chuẩn.
6. Kiểm tra P/Q/S/PF và energy accumulator qua SPI.
7. Chỉ sau khi đạt yêu cầu mới chạy `meter-cal save`.

Không dùng nguồn reference Modbus cho quy trình SPI-only này.

```
meter-cal set --field pga    --value 1|2|4      --apply
meter-cal set --field wiring --value 3p4w|3p3w  --apply
meter-cal set --field freq   --value 50|60      --apply
```

| field    | value        | ý nghĩa                                                        |
| -------- | ------------ | -------------------------------------------------------------- |
| `pga`    | `1`/`2`/`4`  | **DEV override** system PGA (config-owned). Product path = LCD CT Apply. Đổi xong phải calib lại igain. |
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

> **Chỉ calib ở 3P4W** (cần trung tính). Nếu đang ở 3P3W: `meter-cal set --field wiring --value 3p4w --apply`. Gain theo pha dùng chung giữa 2 mode nên sau khi calib xong có thể chuyển lại 3P3W.

#### Cách nhanh (cả 3 pha một lệnh)

Cấp MỘT điện áp đã biết `ref_V` (ví dụ 220V) + trung tính vào **cả 3** kênh áp Ua/Ub/Uc (một nguồn AC 1 pha là đủ):

```
meter-cal auto --field u --value 220
```

Lệnh tự capture đồng thời cả 3 pha, tính `new_ugain = round(old_ugain * ref_V / measured_V)` từng pha, áp xuống chip **một lần**, rồi log residual error% từng pha. Không cần `--apply`.

Nếu chỉ muốn calib một pha: `meter-cal auto --field u --phase a --value 220`.

#### Cách thủ công (tự tính gain)

1. `meter-cal show` → ghi lại `ugain` (`old_ugain`).
2. `meter-latest` → lấy `measured_V`.
3. `new_ugain = round(old_ugain * ref_V / measured_V)`.
4. `meter-cal set --field uigain --phase a --u <new_ugain>` (áp ngay).
5. Kiểm tra `meter-latest`; lặp cho B, C.

### Bước 2 — Current gain

> Vẫn ở 3P4W.

#### Cách nhanh (cả 3 pha một lệnh)

Kẹp **3 CT trên cùng một tải** đã biết `ref_I` (ví dụ 5A):

```
meter-cal auto --field i --value 5
```

Hoặc lấy reference từ đồng hồ chuẩn qua Modbus: `meter-cal auto --field i --value external`.

Một pha: `meter-cal auto --field i --phase a --value 5`.

#### Cách thủ công

1. `meter-cal show` → lấy `old_igain`.
2. `meter-latest` → lấy `measured_I`.
3. `new_igain = round(old_igain * ref_I / measured_I)`.
4. `meter-cal set --field uigain --phase a --i <new_igain>` (áp ngay).
5. Kiểm tra `meter-latest`; lặp cho B, C.

> **Lưu ý all-or-nothing:** nếu một pha không đo được (CT hở, dòng ≤ 0) khi calib cả 3, lệnh **fail và không ghi gì** — chip giữ nguyên calib cũ, log chỉ rõ pha lỗi. Muốn bỏ qua pha đó thì calib riêng các pha còn lại bằng `--phase`.

### Bước 3 — Offset (nếu cần)

Chỉ đặt từ điều kiện **không tải / zero thực đo**, không đoán. Auto-cal (cả 3 pha hoặc một pha):

```
meter-cal auto --field u --value offset      # U offset cả 3 pha
meter-cal auto --field i --value offset      # I offset cả 3 pha
```

Hoặc đặt tay từng pha (áp ngay, không cần `--apply`):

```
meter-cal set --field uioffset --phase a --u <v> --i <v>
```

### Bước 4 — Hiệu chỉnh công suất / phase (sau khi U/I đã calib xong)

> Giả định bước này: **voltage gain và current gain đã calib chuẩn**. Cần tiếp tục có **đồng hồ chuẩn** (PM710, EM-07K, bộ nguồn/tải chuẩn…) để so sánh công suất.
>
> Lưu ý quan trọng về console hiện tại:
> - `meter-cal set --field phase ... --phi` **chỉ điều khiển thanh ghi Phi** (phase angle compensation, 0x48/0x4A/0x4C).
> - **PQGain** (GainA/B/C, 0x47/0x49/0x4B) được set qua `meter-cal set --field pq-gain --value <n>`.
> - **Fundamental power gain** (0x54/0x55/0x56) được set qua `meter-cal set --field fundamental-power-gain --value <n>`.
> - Các giá trị này được lưu vào **NVS** khi chạy `meter-cal save` (cùng với toàn bộ calib blob).

#### 4.1 Chuẩn bị test bench

##### 1. Kiểm tra wiring mode trước khi đấu dây

| Wiring mode | Đấu áp | Đấu dòng | Chú ý |
| --- | --- | --- | --- |
| **3P4W** (mặc định) | L1/L2/L3 đo so với trung tính N | CT trên pha A, B, C | Pha A/B/C độc lập, dùng cho lưới 3 pha 4 dây |
| **3P3W** | L1/L3 đo so với L2 | CT trên pha A và C, pha B làm trung tính tính toán | Chọn khi lưới 3 dây; `meter-cal set --field wiring --value 3p3w --apply` |

> Đổi wiring mode sẽ **kéo relay MODE_SEL** trên board. Kiểm tra phần cứng đã nối relay chưa trước khi đổi.

##### 2. Đấu dây đo pha A (ví dụ calib pha A)

> **⚠️ Cảnh báo an toàn:** Lưới điện 220 V / 380 V có thể gây giật nguy hiểm. Chỉ người có chuyên môn điện được thực hiện. Đảm bảo đã ngắt nguồn trước khi đấu dây. Kiểm tra lại bằng đồng hồ VOM trước khi cấp điện.

Theo ATMEL Application Note, khi calib gain cho một pha (ví dụ pha A), điều kiện bench chuẩn là:

```text
Ua = Ub = Uc = Un      (3 kênh áp đều ở điện áp định mức)
IA = Ib                (dòng pha A = dòng định mức CT)
IB = IC = 0            (pha B và C không có dòng)
PF = 1.0               (tải thuần trở)
```

Trên board cụ thể:

Bước | Vị trí kết nối | Chi tiết
---|---|----
**Cấp áp pha A** | Đầu vào áp **VA / V1** trên board | Nối `L1` (pha A) với đầu vào áp pha A, `N` với đầu vào trung tính (3P4W). **Theo app note, cũng nên cấp áp định mức cho các kênh VB và VC** (có thể nối song song L1→VA, VB, VC và N→trung tính nếu bench cho phép).
**Cấp dòng pha A** | CT trên pha A | Cho CT qua dây dẫn pha A. Ngõ ra CT (`IA+`, `IA-`) nối vào đầu vào dòng **IA / I1**. **Không** để CT hở mạch khi có dòng điện.
**Pha B và C** | Không cho dòng | CT pha B và C có thể để hở hoặc ngắn, miễn là không có dòng chạy qua. Nếu đã lắp CT, không kẹp vào dẫn điện.
**Tải** | Nối giữa L1 và N (3P4W) hoặc giữa L1 và L2/L3 (3P3W theo thiết kế) | Dùng tải thuần trở ổn định: bóng đèn sợi đốt, tủ lạnh ổn định, lò nhiệt… Không dùng động cơ, bơm, biến áp — PF sai.
**Đồng hồ chuẩn** | Đo cùng pha A với board | PM710 / EM-07K / bộ nguồn tải chuẩn. Lấy giá trị `P_ref` của **pha A**.
**Đấu pha B/C tương tự** | Thay VA→VB, IA→IB | Lặp lại khi calib pha B hoặc C.

> **Lưu ý phân biệt:** Đầu vào áp là mạch chia áp (điện trở lớn), đầu vào dòng là CT (nguồn dòng). Không đảo vị trí hai loại đầu vào.

##### 3. Chọn load phù hợp

| Thông số | Yêu cầu | Lý do |
| --- | --- | --- |
| **Công suất** | Khoảng 30%–100% dòng định mức CT (`Ib`) | Đủ lớn để giảm nhiễu, đủ nhỏ để không bão hòa |
| **PF** | ≈ 1.0 thuần trở | PQGain calib chỉ đúng khi P ≈ S |
| **Ổn định** | Công suất không dao động trong 2–3 giây | `auto-pq-gain` lấy mẫu 3 lần (~300 ms) + 700 ms settle + verify |
| **Ví dụ** | Bóng đèn sợi đốt tổng công suất ~100–1000 W, lò nhiệt, bếp điện | Không dùng: máy bơm, máy nén khí, tủ lạnh đang khởi động |

##### 4. Chuẩn bị phần mềm trước khi calib

1. Cấp nguồn cho board, đợi boot xong, mở console.
2. Kiểm tra giao tiếp SPI:
   ```text
   meter-latest
   meter-cal show
   ```
3. Xác nhận tần số lưới đúng:
   ```text
   meter-cal show
   ```
   Nếu sai, set lại:
   ```text
   meter-cal set --field freq --value 50 --apply
   ```
4. Xác nhận **voltage gain** và **current gain** đã calib xong cho pha định calib.
5. Nếu chưa có calib U/I, làm theo Bước 1 và Bước 2 trước.

##### 5. Kiểm tra nhanh trước lệnh calib

Với pha A đã đấu như trên, chạy:

```text
meter-latest
```

Kỳ vọng thấy:
- `V_A` gần đúng điện áp lưới (ví dụ 220 V ± vài V).
- `I_A` dương, gần đúng dòng tải thực tế.
- `P_A` dương, có giá trị rõ ràng.
- `PF_A` gần 1.0.

Nếu `V_A` hoặc `I_A` sai, phải kiểm tra lại đấu dây/mạch chia áp/CT trước khi chạy `auto-pq-gain`.

##### 6. Thứ tự calib

Sản phẩm calib **một lần** trong xưởng: đưa field về baseline rồi calib, ghi trực tiếp (không cộng dồn).

0. **Chuyển sang 3P4W** để calib (cần trung tính): `meter-cal set --field wiring --value 3p4w --apply`. Gain theo pha dùng chung 2 mode, calib xong có thể chuyển lại 3P3W.
1. **Baseline**: `meter-cal default --apply` (toàn bộ) hoặc reset chọn lọc từng field khi cần
   (`--field phi`, `--field pqgain`, ...). `default --field` áp ngay xuống chip.
2. **U/I gain cả 3 pha** (auto-cal, một nguồn AC + trung tính cho 3 kênh áp / 3 CT trên cùng tải):
   `meter-cal auto --field u --value <ref_V>` và `meter-cal auto --field i --value <ref_A>`.
   Rồi **PQGain** (tải PF≈1).
3. **Phi sau**: `meter-cal default --field phi --phase <p>` để về baseline, **rồi**
   `meter-cal auto-phi --phase <p> --value <P_ref_W>` với tải **PF≈0.5L**, chỉ sau khi PQGain đã xong.
4. Cuối cùng lưu:
   ```text
   meter-cal save
   ```

---

#### 4.2 Active-power gain (PQGain)

##### Điều kiện trước khi calib

| STT | Điều kiện | Cách kiểm tra / lưu ý |
| --- | --- | --- |
| 1 | **Voltage gain đã calib xong** | `meter-latest` đọc điện áp đúng với đồng hồ chuẩn |
| 2 | **Current gain đã calib xong** | `meter-latest` đọc dòng đúng với đồng hồ chuẩn |
| 3 | **3 kênh áp đều ở Un** (theo app note `Ua=Ub=Uc=Un`) | Nếu bench cho phép, cấp áp định mức cho VA, VB, VC. Nếu chỉ có một pha, vẫn có thể calib nhưng kết quả có thể kém tối ưu. |
| 4 | **Tải PF ≈ 1.0 thuần trở** | Dùng tải thuần trở ổn định: đèn sợi đốt, tủ lạnh ổn định, bóng sợi đốt… Không dùng tải cảm kháng |
| 5 | **Dòng pha cần calib ≈ Ib, các pha còn lại = 0** (theo app note `IB=IC=0`) | Tránh dòng qua pha chưa calib; dòng pha calib đủ lớn để giảm nhiễu nhưng không bão hòa |
| 6 | **Đồng hồ chuẩn đo P_ref của pha cần calib** | PM710, EM-07K, bộ nguồn/tải chuẩn… |
| 7 | **P_ref > 0** | Công suất phải dương, pha cần calib có tải |
| 8 | **Tần số lưới đã đúng** | `meter-cal show` hiển thị 50/60 Hz đúng |

> Nếu chưa đạt một trong các điều kiện trên, **không chạy auto-pq-gain**, vì kết quả sẽ sai hoặc bị rollback.

##### Các bước

1. Cấp áp định mức cho 3 kênh áp (`Ua=Ub=Uc=Un`) theo điều kiện app note nếu bench cho phép.
2. Đấu tải thuần trở PF≈1 trên pha cần calib sao cho dòng ≈ Ib, các pha còn lại không có dòng (`Ib=Ic=0` nếu calib pha A).
3. Ghi lại công suất tác dụng từ đồng hồ chuẩn `P_ref` (đơn vị W).
4. Chạy lệnh:

```text
meter-cal auto-pq-gain --phase a --value <P_ref_W>
```

Ví dụ: đồng hồ chuẩn báo pha A tiêu thụ 123.4 W:

```text
meter-cal auto-pq-gain --phase a --value 123.4
```

5. Firmware sẽ chạy 4 bước:
   - **B1 — Thu thập + log inputs**: `P_ref` (từ console), `P_chip` = trung bình
     `Pmean` pha chọn (3 mẫu, cách 100 ms — tổng ~300 ms), `old_pq_gain` hiện tại,
     tolerance. Tất cả đổi sang **số nguyên milliwatt (mW)** và được log ra trước
     khi ghi bất cứ thứ gì xuống chip.
   - **B2 — Tính PQGain** (số học nguyên int64, không dùng float):

```text
PQGain = round( (32768 + old_pq_gain) * P_ref_mW / P_chip_mW ) - 32768
```

   - Khi `old_pq_gain = 0`, công thức rút gọn đúng về ATMEL app note AN46103:
     `PQGain = round((-error/(1+error)) * 32768)` với `error = (P_chip - P_ref)/P_ref`.
   - Khi calib lại (old ≠ 0), `old_pq_gain` **bắt buộc** có mặt trong công thức:
     `P_chip` đo được đã chứa hiệu chỉnh cũ, nếu bỏ qua sẽ xóa mất gain cũ.
   - Kết quả ngoài khoảng int16 → báo lỗi `ESP_ERR_INVALID_SIZE`, không ghi.
   - **B3 — Nạp gain**: ghi `PQGain` mới vào pha chọn, apply xuống chip (kèm
     re-enable meter để DSP nạp lại tham số), chờ **700 ms** ổn định.
   - **B4 — Verify**: đọc lại `Pmean` (3 mẫu), tính
     `error% = |P_after − P_ref| / P_ref × 100` (lượng tử 0.01%). Nếu vượt
     tolerance (console đang dùng **2%**) → tự động rollback về PQGain cũ.
   - In ra `before` / `after` và giá trị `PQGain` cũ/mới.
   - **Hỗ trợ calib lặp lại**: có thể chạy lại lệnh nhiều lần; mỗi lần hiệu chỉnh
     cộng dồn đúng từ giá trị PQGain hiện tại.

6. Kiểm tra lại `meter-latest` và đồng hồ chuẩn.
7. Lặp lại cho pha B, C nếu cần.
8. Sau khi xác nhận chính xác, lưu:

```text
meter-cal save
```

> Lưu ý: lệnh này chỉ thay đổi RAM. Nếu không `meter-cal save`, giá trị sẽ mất sau reboot.

##### Cách hoạt động / công thức

- `PQGain` là số nguyên **có dấu 16-bit** (two’s complement), mặc định `0x0000`.
- Nếu chip **đo thấp hơn** chuẩn (`error < 0`) → PQGain **dương**.
- Nếu chip **đo cao hơn** chuẩn (`error > 0`) → PQGain **âm**.

Ví dụ từ app note: chip đo thấp hơn chuẩn, lỗi `-13.78%`:

```text
-error/(1+error) = 0.1378 / 0.8622 ≈ 0.1598
PQGain = round(0.1598 * 32768) = 5237 = 0x1475
```

##### Calib thủ công (nếu không muốn dùng auto)

```text
meter-cal set --field pq-gain --phase a --value 5237
```

(Áp ngay xuống chip, không cần `--apply`.) Lặp lại cho pha B, C. Kiểm tra lại `meter-latest`, sau đó `meter-cal save`.

##### Xử lý lỗi thường gặp

| Hiện tượng | Nguyên nhân thường gặp | Cách xử lý |
|------------|-----------------------|------------|
| `auto-pq-gain` báo rollback | P_ref hoặc P_chip không dương; tải không ổn định; PF khác 1 | Kiểm tra tải thuần trở, đồng hồ chuẩn, dây nối |
| Residual error > tolerance | Nhiễu, tải thay đổi, dòng quá nhỏ | Tăng số mẫu, dùng tải ổn định hơn, tăng tolerance nếu cần |
| `P_chip is not positive` | Pha chọn chưa có tải hoặc đấu dây sai | Kiểm tra CT, pha, relay MODE_SEL |

##### Lưu ý quan trọng

- Chỉ calib **một pha một lần** (`--phase a|b|c`).
- Đừng chạy `meter-cal auto-pq-gain` khi tải đang thay đổi (ví dụ: máy bơm/máy nén khí vừa khởi động).
- Giữ tải ổn định từ lúc nhập lệnh đến khi firmware in kết quả (~1 giây tổng cộng).
- `P_ref` phải đại diện cho cùng điểm hoạt động với `P_chip`; firmware không thể đồng bộ thời điểm đọc đồng hồ chuẩn thủ công.
- Có thể chạy lại `auto-pq-gain` nhiều lần để tinh chỉnh; mỗi lần hiệu chỉnh tăng dần từ PQGain hiện tại.
- `meter-cal auto-pq-gain` giữ mutex đo lường trong ~1 giây; các lệnh khác liên quan calib/measurements sẽ bị block trong thời gian này.

#### 4.3 Phase angle compensation (Phi)

Bước này dùng tải **PF≈0.5L**, dòng định mức Ib, **sau khi PQGain đã calib xong** (thứ tự app note:
gain @PF=1 → phase @PF=0.5L). Sản phẩm chỉ calib một lần trong xưởng, nên Phi được tính từ baseline
và **ghi trực tiếp** — không cộng với giá trị Phi cũ.

##### Đưa Phi về baseline trước (bắt buộc)

Công thức AN46103 giả định **Phi = 0 lúc đo sai số εp**. `auto-phi` vì thế **reject nếu `phase_comp`
hiện tại ≠ 0**. Đưa pha cần calib về baseline bằng lệnh reset chọn lọc (không đụng PQGain/U-I gain):

```text
meter-cal default --field phi --phase a
```

(Áp ngay xuống chip, không cần `--apply`.)

`--field` chọn nhóm thanh ghi cần đưa về baseline (theo từng pha; bỏ `--phase` = cả 3 pha):

| `--field` | Alias | Thanh ghi | Baseline | Ý nghĩa |
| --------- | ----- | --------- | -------- | ------- |
| `phi` | `phase` | PHI_A/B/C (48/4A/4C) | `0` | bù góc pha V–I (calib PF=0.5L, `auto-phi`) |
| `pqgain` | `pq-gain` | PQ_GAIN_A/B/C (47/49/4B) | `0` | gain hiệu chỉnh P/Q (calib PF=1, `auto-pq-gain`) |
| `uigain` | `gain` | U_GAIN/I_GAIN (61–6C) | `0x8000` | gain điện áp + dòng điện (`auto --field u\|i`) |
| `uioffset` | `offset` | U_OFFSET/I_OFFSET (63–6C) | `0` | offset DC kênh áp/dòng |
| `power-offset` | — | P/Q_OFFSET (41–46) | `0` | offset công suất P/Q không tải |
| `fundamental` | `fundamental-power-gain` | P_GAIN_F (54/55/56) | `0` | gain công suất cơ bản (PGainF) |
| `all` | — | cả 6 nhóm | — | reset mọi field của (các) pha đã chọn |

> `uigain`/`uioffset` là tên rõ nghĩa cho gain/offset **U+I**; `gain`/`offset` vẫn nhận như alias tương
> thích ngược. Lưu ý `--field all` **giữ** wiring/PGA/line_freq, khác `meter-cal default` (không `--field`)
> vốn nạp lại PGA/line_freq từ config. `meter-cal default` **không** kèm `--field` vẫn reset toàn bộ như cũ.

##### Auto-calib Phi (khuyến nghị)

```text
meter-cal auto-phi --phase a --value <P_ref_W>
```

`P_ref_W` = công suất tác dụng thật của tải PF=0.5L do đồng hồ chuẩn đo. Thuật toán 4 bước (mirror
`auto-pq-gain`):

1. **Validate + guard**: `0 < P_ref ≤ 1 MW`; đọc `phase_comp` đã áp dụng — **nếu ≠ 0 → `ESP_ERR_INVALID_STATE`**
   và yêu cầu chạy `meter-cal default --field phi`.
2. **Gather + LOG**: `P_ref_mW`, `P_chip_mW` (3 mẫu, helper int64 mW), `line_freq` → chọn `Gphase`;
   log toàn bộ inputs **trước khi ghi**.
3. **Compute (integer, có dấu)**:
   ```text
   GP_NUM  = 3763739 (50 Hz) | 3136449 (60 Hz)     # = round(Gphase × 1000)
   diff_mw = P_chip_mW - P_ref_mW                   # = εp × P_ref_mW (signed)
   Phi     = round( diff_mw × GP_NUM / (P_ref_mW × 1000) )
   ```
   Range-check `|Phi| ≤ 255`; vượt → `ESP_ERR_INVALID_SIZE` (không clamp, không ghi).
4. **Write + verify**: ghi `phase_comp = Phi` → apply + reload DSP → settle → đọc lại `P_chip`;
   `error% = |P_after − P_ref|/P_ref`; > tolerance → **rollback** về Phi=0. `PAngle` (0.1°/LSB) được
   đọc để **log sanity** (kỳ vọng ≈60° tại PF=0.5L) nhưng **không** tham gia PASS/FAIL.

Ví dụ app note (50 Hz, εp = +0.95%): `round(0.0095 × 3763.739) = 36 = 0x24`. Kiểm bằng công thức integer:
`P_ref=1000 W, P_chip=1009.5 W → diff=9500 mW, Phi = round(9500×3763739/1e9) = 36`. ✓

##### Calib thủ công (fallback)

Nếu muốn tự tính và ghi:

```text
P_error = (P_chip - P_ref) / P_ref          # tại PF=0.5L
Phi     = round( P_error * Gphase )         # Gphase = 3763.739 (50Hz) | 3136.449 (60Hz)
meter-cal set --field phase --phase a --phi <Phi>
```

Giá trị `phi` có thể **âm**; phạm vi hợp lệ **-255..255**. Firmware mã hóa: dương → magnitude 0..255;
âm → set bit 15 (0x8000) cùng magnitude, theo quy ước Application Note.

Lưu ý tài liệu: **Application Note** coi Phi là số có dấu 16-bit (MSB=1 → âm); **Datasheet** định nghĩa
bit 15 là `DelayV` (chọn kênh trễ), bits 7:0 là `DelayCycles` @2.048MHz. Firmware theo Application Note.
Nếu Phi dương làm công suất lệch ngược chiều, thử đổi dấu.

#### 4.4 Power offset P/Q (zero năng lượng khi không tải)

Sau khi gain/phase đã calib, nếu khi **không tải** vẫn thấy P hoặc Q khác 0:

```text
meter-cal auto-power-offset --field p --phase a
meter-cal auto-power-offset --field q --phase a
```

Firmware sẽ:

1. Zero hóa offset tương ứng tạm thời.
2. Đọc 20 mẫu raw signed `Pmean`/`Qmean`.
3. Tính `new_offset = -round(average_raw_counts)`.
4. Ghi offset, kiểm tra residual. Nếu không ổn, rollback về giá trị cũ.

Lặp lại cho các pha B, C. Sau khi xác nhận ổn định mới `meter-cal save`.

#### 4.5 Fundamental power gain (PGainF)

Bước này tùy chọn, dùng nếu bạn cần calib công suất **fundamental**. Thực hiện sau khi PQGain đã xong.

1. Dùng tải PF≈1, dòng Ib (giống PQGain).
2. Tính gain tương tự PQGain từ sai số công suất fundamental so với đồng hồ chuẩn.
3. Ghi PGainF qua console:

```text
meter-cal set --field fundamental-power-gain --phase a --value 100
```

Giá trị là **signed 16-bit**, mặc định `0x0000`.

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

| Đại lượng               | Công thức                                                                 |
| ----------------------- | --------------------------------------------------------------------------- |
| Voltage gain            | `new = round(old * V_ref / V_measured)`                                     |
| Current gain            | `new = round(old * I_ref / I_measured)`                                     |
| PQGain / PGainF         | `new = round((32768 + old) * P_ref_mW / P_chip_mW) - 32768` |
| Phi 50 Hz (`auto-phi`)  | `round((P_chip_mW - P_ref_mW) * 3763739 / (P_ref_mW * 1000))` — yêu cầu Phi=0 baseline |
| Phi 60 Hz (`auto-phi`)  | `round((P_chip_mW - P_ref_mW) * 3136449 / (P_ref_mW * 1000))` — yêu cầu Phi=0 baseline |

Ví dụ: `old_ugain = 7305`, cấp `ref_V = 220V`, đo được `measured_V = 218.5V`:

```
new_ugain = round(7305 * 220 / 218.5) = round(7355.1) = 7355
meter-cal set --field uigain --phase a --u 7355
```

> Công thức gain `new = round(old * ref / measured)` được firmware tính bằng **số học nguyên int64** (scale ref/measured ra µ-đơn vị, chia nguyên, làm tròn half-up) — deterministic, không phụ thuộc FPU double. Kết quả khớp công thức double đến từng LSB.

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
- **PGA / line frequency — ownership nghiêm:**
  - **Chỉ** (1) Kconfig **ATM90E32AS Parameters** (first boot / `meter-cal default`) và (2) console `meter-cal set --field pga|freq` được **đổi** hai thông số trên chip.
  - `energy_meter_set_calibration`, auto-cal, SD import, portal, LCD, Modbus, `CFG_LINE_FREQ`, `config_apply(MEASUREMENT)` **không** được ghi PGA/freq (set_calibration/import **giữ** giá trị đang chạy).
  - Persist: `meter-cal save` / portal **Save and restart** (blob calib). NVS config có thể **mirror** `line_freq` cho alarm/register (RO).
  - LCD Calib Info: xem Mode/Freq/PGA. Portal Active profile: chỉ mode.
### Quy ước điện áp theo wiring mode

- **3P4W:** `Ua = Uan`, `Ub = Ubn`, `Uc = Ucn`; cả ba kênh áp hợp lệ.
- **3P3W:** theo datasheet, `Ua = Uab`, `Uc = Ucb`, còn `Ub` không được sử dụng. Firmware đánh dấu kênh B không hợp lệ và không đưa nó vào điện áp trung bình.
- Mỗi mode có một profile calibration riêng. Khi chuyển mode, firmware chọn profile tương ứng và apply đồng thời relay MODE_SEL lẫn MMode0/MMode1 của ATM90E32AS.
- Có thể copy thông số giữa hai profile để làm điểm khởi đầu, nhưng profile đích phải được hiệu chỉnh/kiểm tra lại trước khi lưu dùng chính thức.

### PGA và auto-calibration

PGA được biểu diễn logic là x1/x2/x4 và áp dụng đồng thời cho IA/IB/IC trong MMode1.

**Kconfig defaults** (`menuconfig` → Application Configuration → ATM90E32AS Energy Meter → **ATM90E32AS Parameters**):

| Option | Default | Dùng khi |
|--------|---------|----------|
| Default current PGA | **x4** | First boot / no NVS calib / `meter-cal` factory defaults |
| Default grid frequency | **50 Hz** | Same |

NVS blob đã save **ghi đè** các default này. Đổi Kconfig rồi rebuild chỉ ảnh hưởng board chưa có calib (hoặc sau erase calib).

Khi auto-calibration tính ra gain ngoài range (`ESP_ERR_INVALID_SIZE`):

1. Firmware **không** đổi PGA (PGA chỉ console/Kconfig).
2. Fail + rollback gain; message gợi ý set PGA tay nếu là kênh dòng.
3. Dev: `meter-cal set --field pga --value 1|2|4 --apply` rồi chạy auto lại với giá trị thực mới.
4. Kênh áp ngoài range → kiểm tra divider / đấu dây (không đụng PGA).

PGA không tác động kênh áp; Ugain ngoài range yêu cầu kiểm tra divider và cách đấu dây.

Auto-calibration không tự save NVS. Sau khi kiểm tra kết quả, người vận hành vẫn phải gọi `meter-cal save` (portal: **Save and restart**).

#### Calib một pha hoặc cả 3 pha cùng lúc

`meter-cal auto` hiệu chỉnh **U/I gain hoặc offset**. `--phase` **tùy chọn**:

```text
# CẢ 3 PHA cùng lúc, dùng MỘT reference chung (bỏ --phase hoặc --phase all):
meter-cal auto --field u --value 220            # U gain cả 3 pha, ref 220V
meter-cal auto --field i --value 5             # I gain cả 3 pha, ref 5A
meter-cal auto --field u --value external      # U cả 3 pha, ref từ Modbus meter
meter-cal auto --field i --value offset        # I offset cả 3 pha (không tải)

# MỘT PHA:
meter-cal auto --field u --phase a --value 220
meter-cal auto --field i --phase b --value 5
meter-cal auto --field i --phase c --value external
```

Kịch bản all-3-phase (nhanh nhất ở xưởng):
- **Voltage:** một nguồn AC 1 pha + trung tính cấp cho cả 3 kênh áp Ua/Ub/Uc → `--field u --value <ref_V>`.
- **Current:** 3 CT kẹp trên cùng một tải → `--field i --value <ref_A>`.

Hành vi multi-phase:
- **Capture đồng thời:** mỗi mẫu đọc chip MỘT lần, lấy tất cả pha được chọn từ cùng snapshot (các pha đồng bộ thời gian).
- **Compute trước, apply sau (all-or-nothing):** tính gain cho TẤT CẢ pha được chọn trước; nếu một pha không tính được (CT hở, giá trị ≤ 0, gain ngoài range) → **không ghi gì cả**, chip giữ nguyên calib cũ, trả lỗi kèm log chỉ rõ pha nào fail.
- **Single-shot:** áp một lần, đo lại chỉ để **log residual error%** từng pha. Trượt tolerance chỉ **WARN**, không rollback (giá trị đã tính đúng). Rollback **chỉ** khi compute/apply/SPI fail.
- **Chỉ calib ở 3P4W:** ở 3P3W lệnh trả `ESP_ERR_NOT_SUPPORTED`. Vì gain theo pha được **dùng chung** giữa 2 wiring mode, calib ở 3P4W đã đúng cho 3P3W — chuyển `meter-cal set --field wiring --value 3p4w --apply`, calib, rồi chuyển lại 3P3W.

`external` lấy reference từ PM710 hoặc EM-07K đang được Modbus Master chọn và phải ở trạng thái online; với multi-phase, reference external được đọc **riêng từng pha** (`voltage[p]`/`current[p]`).

---

## 7. Lưu ý triển khai mới

- Calibration được validate trước khi ghi; gain bằng 0, enum sai, NaN/Inf và giá trị console vượt kiểu dữ liệu đều bị từ chối.
- Khi apply, driver luôn thử khóa lại vùng configuration kể cả khi SPI lỗi giữa chừng và chỉ cập nhật state sau readback thành công.
- Đổi 50/60 Hz cập nhật cả MMode0 lẫn `FreqLoTh/FreqHiTh`.
- Reference tự động có thể lấy thủ công hoặc từ đồng hồ Modbus công nghiệp đang online (PM710/EM-07K). Với 3P3W, reference áp ngoài chỉ được dùng nếu đồng hồ chuẩn cung cấp đúng điện áp dây; dữ liệu L-N không được dùng thay thế.

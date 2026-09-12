# Test Plan — ATM90E32AS Calibration & Mode Switching

**Firmware:** FIRMWARE/uart_echo  
**Date:** 2026-08-16  
**Board S/N:** ____________________  
**Tester:** ____________________  

---

## Mục lục

- [A. Build & Boot](#a-build--boot)
- [B. SPI & Register](#b-spi--register)
- [C. MMode0 & Frequency](#c-mmode0--frequency)
- [D. PGA & MMode1](#d-pga--mmode1)
- [E. Relay & Wiring Transition](#e-relay--wiring-transition)
- [F. LCD Menu Wiring](#f-lcd-menu-wiring)
- [G. Two Profile Calibration](#g-two-profile-calibration)
- [H. Console Range Validation](#h-console-range-validation)
- [I. Manual Auto-Calibration — Voltage](#i-manual-auto-calibration--voltage)
- [J. Manual Auto-Calibration — Current](#j-manual-auto-calibration--current)
- [K. External Reference (PM710 / EM-07K)](#k-external-reference-pm710--em-07k)
- [L. 3P4W Display Semantics](#l-3p4w-display-semantics)
- [M. 3P3W Display Semantics](#m-3p3w-display-semantics)
- [N. Save / Load / Factory Reset](#n-save--load--factory-reset)
- [O. Accuracy Verification](#o-accuracy-verification)
- [P. Long-Term Stability](#p-long-term-stability)
- [Q. Web Portal Calibration](#q-web-portal-calibration)

---

## Bảng tra thanh ghi liên quan

Các test dưới đây dùng lệnh `meter-reg read <địa chỉ>` để đọc thanh ghi của ATM90E32AS rồi so sánh với giá trị kỳ vọng.

| Address | Tên thanh ghi | Giá trị các bit | Ý nghĩa |
|---------|---------------|-----------------|---------|
| `0x33`  | MMode0        | bit 12 = 1 → 60Hz, = 0 → 50Hz; bit 8 = 1 → 3P3W, = 0 → 3P4W | Cấu hình chế độ đo + tần số |
| `0x34`  | MMode1        | `IA[1:0]`, `IB[3:2]`, `IC[5:4]` mỗi cặp: 00=x1, 01=x2, 10=x4 | PGA gain kênh dòng |
| `0x0C`  | FreqLoTh      | Số nguyên 16-bit, đơn vị 0.01 Hz | Ngưỡng tần số thấp |
| `0x0D`  | FreqHiTh      | Số nguyên 16-bit, đơn vị 0.01 Hz | Ngưỡng tần số cao |
| `0x61`  | UgainA        | Số nguyên 16-bit | Voltage gain phase A |
| `0x62`  | IgainA        | Số nguyên 16-bit | Current gain phase A |

**Giá trị kỳ vọng — tra nhanh:**

| MMode0 | MMode1 | FreqLoTh | FreqHiTh | Ý nghĩa |
|--------|--------|----------|----------|---------|
| `0x0087` | `0x0000` | `4700` | `5300` | 3P4W + 50Hz + PGA ×1 |
| `0x0185` | `0x0015` | `4700` | `5300` | 3P3W + 50Hz + PGA ×2 |
| `0x1087` | `0x002A` | `5700` | `6300` | 3P4W + 60Hz + PGA ×4 |
| `0x1185` | — | `5700` | `6300` | 3P3W + 60Hz |

**Cách đọc:** ghi giá trị kỳ vọng vào bảng, rồi gõ lệnh `meter-reg read 0x33` xem ra số bao nhiêu. Nếu khớp → PASS, không khớp → FAIL.

---

## A. Build & Boot

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| A01 | Build sạch | `cmake --build build` | Build OK, sinh .bin | ☐ PASS ☐ FAIL | |
| A02 | Boot — chưa có profile NVS | Xoá NVS calib, boot | Meter init, log "uncalibrated", không crash | ☐ PASS ☐ FAIL | |
| A03 | Boot — profile đã lưu | `meter-cal save` → reboot | Relay + MMode0 + MMode1 + gain đúng | ☐ PASS ☐ FAIL | |
| A04 | Profile NVS hỏng | Ghi blob sai magic/version | Fallback default, log rõ lý do | ☐ PASS ☐ FAIL | |

---

## B. SPI & Register

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| B01 | Đọc register cơ bản | `meter-reg read 0x33 0x34 0x61 0x62` | OK, trả giá trị ổn định | ☐ PASS ☐ FAIL | |
| B02 | Readback sau apply | `meter-cal apply` → `meter-reg read 0x33 0x34 0x61 0x62` | Khớp giá trị vừa ghi | ☐ PASS ☐ FAIL | |
| B03 | Lock sau apply | `CfgRegAccEn` (0x10) phải về 0 sau apply | Confirm bằng debug hoặc scope | ☐ PASS ☐ FAIL | |
| B04 | Apply lỗi → state cũ | Inject SPI fail → apply → `meter-reg read 0x61 0x62` | Gain cũ vẫn còn | ☐ PASS ☐ FAIL | |

---

## C. MMode0 & Frequency

Cách test: chạy từng lệnh, đọc thanh ghi, so với bảng tra nhanh ở trên.

| # | Test | Command | Expected | Result | Notes |
|---|-------|---------|----------|--------|-------|
| C01 | 3P4W + 50 Hz | `meter-cal set --field wiring --value 3p4w --apply` + `meter-cal set --field freq --value 50 --apply` + `meter-reg read 0x33 0x0C 0x0D` | MMode0=`0x0087`, FreqLoTh=`4700`, FreqHiTh=`5300` | ☐ PASS ☐ FAIL | |
| C02 | 3P3W + 50 Hz | `meter-cal set --field wiring --value 3p3w --apply` + `meter-cal set --field freq --value 50 --apply` + `meter-reg read 0x33` | MMode0=`0x0185` | ☐ PASS ☐ FAIL | |
| C03 | 3P4W + 60 Hz | `meter-cal set --field wiring --value 3p4w --apply` + `meter-cal set --field freq --value 60 --apply` + `meter-reg read 0x33 0x0C 0x0D` | MMode0=`0x1087`, FreqLoTh=`5700`, FreqHiTh=`6300` | ☐ PASS ☐ FAIL | |
| C04 | 3P3W + 60 Hz | `meter-cal set --field wiring --value 3p3w --apply` + `meter-cal set --field freq --value 60 --apply` + `meter-reg read 0x33` | MMode0=`0x1185` | ☐ PASS ☐ FAIL | |
| C05 | Đổi freq runtime 50↔60 | `meter-cal set --field freq --value 60 --apply` + `meter-cal set --field freq --value 50 --apply` (không reboot giữa 2 lệnh) | MMode0 + FreqLoTh/FreqHiTh đổi ngay theo lần cuối | ☐ PASS ☐ FAIL | |

---

## D. PGA & MMode1

Cách test: đặt PGA, đọc thanh ghi 0x34 xem giá trị hex.

| # | Test | Command | Expected | Result | Notes |
|---|------|---------|----------|--------|-------|
| D01 | PGA ×1 | `meter-cal set --field pga --value 1 --apply` + `meter-reg read 0x34` | MMode1=`0x0000` | ☐ PASS ☐ FAIL | |
| D02 | PGA ×2 | `meter-cal set --field pga --value 2 --apply` + `meter-reg read 0x34` | MMode1=`0x0015` | ☐ PASS ☐ FAIL | |
| D03 | PGA ×4 | `meter-cal set --field pga --value 4 --apply` + `meter-reg read 0x34` | MMode1=`0x002A` | ☐ PASS ☐ FAIL | |
| D04 | PGA invalid | `meter-cal set --field pga --value 3 --apply` hoặc `meter-cal set --field pga --value abc --apply` hoặc `meter-cal set --field pga --value 8 --apply` | Bị từ chối, register không đổi | ☐ PASS ☐ FAIL | |
| D05 | Clipping full-load | Ứng mỗi PGA, cấp dòng max → `meter-latest` kiểm tra dòng đo | Không clip, tuyến tính | ☐ PASS ☐ FAIL | |

---

## E. Relay & Wiring Transition

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| E01 | Relay 3P4W | `meter-cal set --field wiring --value 3p4w --apply` | `MODE_SEL=0`, relay chuyển | ☐ PASS ☐ FAIL | |
| E02 | Relay 3P3W | `meter-cal set --field wiring --value 3p3w --apply` | `MODE_SEL=1`, relay chuyển | ☐ PASS ☐ FAIL | |
| E03 | Đồng bộ relay+IC | Chuyển 3P4W→3P3W→3P4W nhiều lần | Relay và MMode0 luôn cùng mode | ☐ PASS ☐ FAIL | |
| E04 | Rollback khi apply lỗi | Inject SPI lỗi, yêu cầu 3P3W | Relay về 3P4W, IC apply lại 3P4W | ☐ PASS ☐ FAIL | |
| E05 | Relay settle time | Dùng scope đo MODE_SEL + tiếp điểm | Firmware chờ >= 20ms, không publish khi bounce | ☐ PASS ☐ FAIL | |

---

## F. LCD Menu Wiring

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| F01 | 3P4W → 3P3W từ LCD | Menu → Wiring Mode → đổi → Confirm | Relay+IC cùng 3P3W, LCD hiển thị đúng | ☐ PASS ☐ FAIL | |
| F02 | 3P3W → 3P4W từ LCD | Tương tự | Relay+IC cùng 3P4W | ☐ PASS ☐ FAIL | |
| F03 | Cancel trên LCD | Vào menu → Cancel | Không đổi relay, không đổi IC | ☐ PASS ☐ FAIL | |
| F04 | Apply thất bại từ LCD | Inject SPI lỗi, đổi mode | LCD báo fail, relay+IC rollback | ☐ PASS ☐ FAIL | |
| F05 | Reboot sau đổi mode LCD | Đổi mode → reboot | Boot đúng mode mới, relay+IC đồng bộ | ☐ PASS ☐ FAIL | |

---

## G. Two Profile Calibration

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| G01 | Profile 3P4W độc lập | `meter-cal set --field gain --phase a --u 10000` (áp ngay) + `meter-cal save` → chuyển sang 3P3W → quay lại 3P4W → `meter-cal show` | UgainA=10000 còn nguyên | ☐ PASS ☐ FAIL | `--apply` giờ no-op với set per-phase |
| G02 | Profile 3P3W độc lập | Tương tự G01, set giá trị khác ở chế độ 3P3W | Không ảnh hưởng profile 3P4W | ☐ PASS ☐ FAIL | |
| G04 | Save/load 2 profile | Set gain khác nhau ở 2 mode, `meter-cal save`, reboot, chuyển mode kiểm tra | Cả 2 profile đúng | ☐ PASS ☐ FAIL | |
| G05 | Profile slot sai mode | Blob 3P3W slot chứa wiring=3P4W | Bị từ chối, không apply | ☐ PASS ☐ FAIL | |

---

## H. Console Range Validation

| # | Test | Command | Expected | Result | Notes |
|---|------|---------|----------|--------|-------|
| H01 | Gain âm | `meter-cal set --field gain --phase a --u -1` | `voltage gain must be in range 1..65535` | ☐ PASS ☐ FAIL | |
| H02 | Gain = 0 | `meter-cal set --field gain --phase a --u 0` | Bị từ chối | ☐ PASS ☐ FAIL | |
| H03 | Gain > 65535 | `meter-cal set --field gain --phase a --u 70000` | Bị từ chối, không wrap | ☐ PASS ☐ FAIL | |
| H04 | Offset > int16 | `meter-cal set --field offset --phase a --u 40000` | `voltage offset out of int16 range` | ☐ PASS ☐ FAIL | |
| H05 | Enum sai | `meter-cal set --field wiring --value 3p5w --apply` hoặc `meter-cal set --field freq --value 100 --apply` | Bị từ chối, state cũ giữ nguyên | ☐ PASS ☐ FAIL | |

---

## I. Manual Auto-Calibration — Voltage

**Trình tự bắt buộc:** offset trước (no-load, 0V) → gain sau (cấp áp chuẩn). Đo offset khi đang có áp sẽ sai; calib gain khi chưa bù offset sẽ lệch nền. Làm đúng thứ tự I01 → I02.

| # | Test | Command | Expected | Result | Notes |
|---|------|---------|----------|--------|-------|
| **Bước 1 — Offset (no-load)** | | | | | |
| I01 | Auto offset Ua (0V) | Ngắt áp pha A (0V) → `meter-cal auto --field u --phase a --value offset` | Đo residual, ghi Uoffset = -residual, `meter-cal show` thấy uoffset≠0, **không đổi gain** | ☐ PASS ☐ FAIL | |
| I02 | Offset ngoài range | Residual quá lớn (>int16 sau scale) | `ESP_ERR_INVALID_SIZE`, rollback offset cũ | ☐ PASS ☐ FAIL | |
| **Bước 2 — Gain (cấp áp chuẩn)** | | | | | |
| I03 | 3P4W auto Ua | Sau I01, cấp 220V vào pha A → `meter-cal auto --field u --phase a --value 220` | 20 mẫu, tính gain từ nền đã bù offset, apply, verify, log, **không save** | ☐ PASS ☐ FAIL | |
| I04 | Offset giữ nguyên sau gain | Chạy I01 rồi I03, `meter-cal show` | Uoffset từ I01 còn nguyên, chỉ gain đổi | ☐ PASS ☐ FAIL | |
| I05 | 3P4W auto Ub/Uc | `meter-cal auto --field u --phase b --value 220` + `meter-cal auto --field u --phase c --value 220` | Gain riêng từng phase, không ảnh hưởng pha khác | ☐ PASS ☐ FAIL | |
| I06 | 3P3W chặn mọi calib (Ua) | Chuyển sang 3P3W + `meter-cal auto --field u --phase a --value 380` | `ESP_ERR_NOT_SUPPORTED` — calib bị chặn hoàn toàn ở 3P3W (cần trung tính). Log gợi ý chuyển 3P4W | ☐ PASS ☐ FAIL | Gain dùng chung 2 mode: calib ở 3P4W rồi chuyển lại 3P3W |
| I07 | 3P3W chặn mọi calib (Uc) | 3P3W + `meter-cal auto --field u --phase c --value 380` | `ESP_ERR_NOT_SUPPORTED` | ☐ PASS ☐ FAIL | |
| I08 | 3P3W chặn auto cả 3 pha | 3P3W + `meter-cal auto --field u --value 380` | `ESP_ERR_NOT_SUPPORTED`, không ghi gì | ☐ PASS ☐ FAIL | |
| I09 | Ref = 0 hoặc âm | `meter-cal auto --field u --phase a --value 0` | Bị từ chối (dùng `--value offset` cho no-load) | ☐ PASS ☐ FAIL | |
| I10 | Ugain > 65535 | Điện áp thực tế quá nhỏ so với ref đã khai báo | `ESP_ERR_INVALID_SIZE`; compute fail TRƯỚC khi ghi → **không ghi gì** (không cần rollback), PGA không đổi | ☐ PASS ☐ FAIL | |
| I11 | Không tự lưu | Auto thành công, reboot → `meter-cal show` | Giá trị mới mất, profile cũ còn | ☐ PASS ☐ FAIL | |

---

## J. Manual Auto-Calibration — Current

**Trình tự bắt buộc:** offset trước (no-load, 0A) → gain sau (cấp dòng chuẩn). Làm đúng thứ tự J01 → J03.

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| **Bước 1 — Offset (no-load)** | | | | | |
| J01 | Auto offset Ia (0A) | Ngắt tải pha A (0A) → `meter-cal auto --field i --phase a --value offset` | Đo residual, ghi Ioffset = -residual, **không đổi gain/PGA** | ☐ PASS ☐ FAIL | |
| J02 | Offset ngoài range | Residual quá lớn (>int16 sau scale) | `ESP_ERR_INVALID_SIZE`, rollback offset cũ | ☐ PASS ☐ FAIL | |
| **Bước 2 — Gain (cấp dòng chuẩn)** | | | | | |
| J03 | Igain trong range, PGA giữ nguyên | Sau J01, cấp 5A vào pha A → `meter-cal auto --field i --phase a --value 5` | Apply, verify, gain tính từ nền đã bù offset, PGA không đổi | ☐ PASS ☐ FAIL | |
| J04 | Offset giữ nguyên sau gain | Chạy J01 rồi J03, `meter-cal show` | Ioffset từ J01 còn nguyên, chỉ gain đổi | ☐ PASS ☐ FAIL | |
| J05 | PGA ×1 → ×2 | Tín hiệu dòng rất nhỏ, Igain > 65535 ở ×1 | PGA tự tăng lên ×2, chờ, đo lại, calib thành công | ☐ PASS ☐ FAIL | |
| J06 | PGA ×1 → ×2 → ×4 | Tín hiệu rất nhỏ, cần đến ×4 | Tuần tự ×1→×2→×4, mỗi bước apply+chờ | ☐ PASS ☐ FAIL | |
| J07 | PGA ×4 vẫn overflow | Tín hiệu quá nhỏ, ×4 không đủ | Rollback PGA+Igain ban đầu, `rolled_back=true` | ☐ PASS ☐ FAIL | |
| J08 | Apply lỗi sau tăng PGA | Inject SPI sau khi PGA đã tăng | Rollback PGA+gain ban đầu | ☐ PASS ☐ FAIL | |
| J09 | Dòng = 0 | `meter-cal auto --field i --phase a --value 5` khi không tải | Capture fail (mẫu ≤ 0) TRƯỚC khi ghi → `ESP_ERR_INVALID_RESPONSE`, **không ghi gì**, không rollback, PGA không đổi | ☐ PASS ☐ FAIL | |
| J10 | Full-scale sau auto-PGA | Sau auto ×2/×4, cấp dòng max | Không clipping | ☐ PASS ☐ FAIL | |

---

## K. External Reference (PM710 / EM-07K)

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| K01 | External offline | `meter-cal auto --field i --phase a --value external` (khi meter ngoài offline) | `ESP_ERR_INVALID_STATE` | ☐ PASS ☐ FAIL | |
| K02 | External chưa config | Modbus Master disabled hoặc chưa config | Bị từ chối, log rõ | ☐ PASS ☐ FAIL | |
| K03 | PM710 online | Set PM710 online → `meter-cal auto --field i --phase a --value external` | Log nguồn PM710, calib bình thường | ☐ PASS ☐ FAIL | |
| K04 | EM-07K online | Set EM-07K online → `meter-cal auto --field i --phase a --value external` | Log nguồn EM-07K, calib bình thường | ☐ PASS ☐ FAIL | |
| K05 | External voltage 3P4W | `meter-cal auto --field u --phase a --value external` | Ref từ L1-N của meter ngoài | ☐ PASS ☐ FAIL | |
| K06 | External voltage 3P3W | `meter-cal auto --field u --value external` ở 3P3W | `ESP_ERR_NOT_SUPPORTED` (mọi calib bị chặn ở 3P3W, xem I06) | ☐ PASS ☐ FAIL | |
| K07 | Ref = 0 hoặc invalid | Ngắt kết nối sensor, đọc về 0 | `ESP_ERR_INVALID_RESPONSE` | ☐ PASS ☐ FAIL | |
| K08 | Ref stale | Ngắt RS485, chạy auto ngay | Tuỳ trạng thái, bị từ chối khi offline | ☐ PASS ☐ FAIL | |

---

## L. 3P4W Display Semantics

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| L01 | Data model 3P4W | `meter-cal set --field wiring --value 3p4w --apply` + `meter-latest` | `voltage_l1/2/3` = U1N/U2N/U3N, cả 3 hợp lệ; dòng hiển thị I1/I2/I3 | ☐ PASS ☐ FAIL | |
| L02 | LCD 3P4W | Quan sát LCD | Trang VOLTAGE hiển thị V1N/V2N/V3N; trang CURRENT hiển thị I1/I2/I3 | ☐ PASS ☐ FAIL | |
| L03 | MQTT/Modbus 3P4W | Quan sát dữ liệu MQTT/Modbus | 3 điện áp hợp lệ, có wiring mode/validity | ☐ PASS ☐ FAIL | |

---

## M. 3P3W Display Semantics

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| M01 | Data model 3P3W | `meter-cal set --field wiring --value 3p3w --apply` + `meter-latest` | `voltage_l1=U12`, `voltage_l2=0`, `voltage_l3=U32`, `voltage_avg=(U12+U32)/2`; dòng hợp lệ I1/I3 | ☐ PASS ☐ FAIL | |
| M02 | Ub không hợp lệ | `meter-latest` | Ub không hiển thị hoặc báo invalid | ☐ PASS ☐ FAIL | |
| M03 | LCD 3P3W | Quan sát LCD | Trang VOLTAGE hiển thị U12/U32/U13=N/A; trang CURRENT hiển thị I1/I2=N/A/I3 | ☐ PASS ☐ FAIL | |
| M04 | Modbus validity | Đọc thanh ghi Modbus | Master đọc được wiring mode, biết kênh B invalid | ☐ PASS ☐ FAIL | |
| M05 | Chuyển mode giữa lúc đọc | Chuyển 3P4W↔3P3W liên tục | Không publish snapshot trộn mode cũ+điện áp mới | ☐ PASS ☐ FAIL | |
| M06 | LCD total page | Quan sát LCD | Trang TOTAL hiển thị I từ IrmsN tạm thời ở cả 3P3W và 3P4W, cùng P/Q tổng | ☐ PASS ☐ FAIL | |

---

## N. Save / Load / Factory Reset

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| N01 | Save sau calib thành công | `meter-cal auto --field u --phase a --value 220` + `meter-cal save` → reboot → `meter-cal show` | Blob 2 profile lưu, phục hồi đúng | ☐ PASS ☐ FAIL | |
| N02 | Save sau calib thất bại | Rollback rồi `meter-cal save` | Lưu profile đã rollback | ☐ PASS ☐ FAIL | |
| N04 | Reset defaults giữ mode | `meter-cal default --apply` + `meter-cal show` | Gain về default, wiring mode không đổi | ☐ PASS ☐ FAIL | |

---

## O. Accuracy Verification

| # | Test | Method | Criteria | Result | Notes |
|---|------|--------|----------|--------|-------|
| O01 | Voltage đa điểm | Cấp 50%, 80%, 100%, 110% Uref, so sánh `meter-latest` với đồng hồ chuẩn | Sai số trong yêu cầu thiết kế | ☐ PASS ☐ FAIL | |
| O02 | Current đa điểm | Cấp 1%, 10%, 50%, 100% Iref, so sánh `meter-latest` với đồng hồ chuẩn | Sai số trong yêu cầu thiết kế | ☐ PASS ☐ FAIL | |
| O03 | 3 pha cân bằng | Cả 3 pha = nhau | Sai số A/B/C đồng đều | ☐ PASS ☐ FAIL | |
| O04 | 3 pha không cân bằng | Từng pha khác nhau | Từng pha đúng, tổng hợp lý | ☐ PASS ☐ FAIL | |
| O05 | PF ≈ 1 | Tải thuần trở | P ≈ U×I, PF ≈ 1 | ☐ PASS ☐ FAIL | |
| O06 | PF thấp | Tải inductive/capacitive | P/Q/PF hợp lý | ☐ PASS ☐ FAIL | |
| O07 | No-load | Có áp, không tải | Current ≈ 0, P/Q không trôi | ☐ PASS ☐ FAIL | |

---

## P. Long-Term Stability

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| P01 | Chạy 1 giờ liên tục | Monitor SPI, watchdog, drift | Không lỗi, số đo ổn định | ☐ PASS ☐ FAIL | |
| P02 | Reboot 20–50 lần | Power cycle hoặc reset | Profile luôn load đúng, relay+IC đồng bộ | ☐ PASS ☐ FAIL | |
| P03 | Chuyển mode 50 lần | 3P4W↔3P3W, không tải | Không kẹt relay, không lỗi tích luỹ | ☐ PASS ☐ FAIL | |
| P04 | Mất nguồn giữa lúc save | Cắt nguồn ở nhiều thời điểm | NVS không crash, blob hỏng bị từ chối | ☐ PASS ☐ FAIL | |

---

## Q. Web Portal Calibration

> Phản ánh đúng code hiện tại trong `main/app/web_portal.c`.  
> **Có:** form auto-cal (nhập giá trị chuẩn V/I hoặc `offset`).  
> **Không có:** Live reading trên web, Save to NVS riêng, download `.bin`, import file.  
> Đọc số trên **LCD**. File backup/restore = **LCD + SD**.  
> Persist calib = nút **Save and restart** (`POST /save` → `config_manager_save` + `energy_meter_save_calibration` rồi reboot).  
> Portal chỉ bật khi SoftAP / AP Setup đang chạy. Auth = session cookie (cùng tài khoản console nếu `CONFIG_APP_CONSOLE_AUTH_ENABLE`).

### Q.0 Chuẩn bị

| # | Bước | Expected |
|---|------|----------|
| Q00 | LCD → Settings → AP Setup → Start AP; join AP; mở `http://192.168.4.1` | Trang login / settings load |
| Q01 | Đăng nhập (nếu auth bật) | Vào `/`, thấy section **Calibration** |
| Q02 | Cuộn tới **Calibration** | Active profile (**mode only** 3P4W/3P3W) + Status + form Phase/Channel/True value + **Calibrate**; **không** Live/Refresh/Save NVS/Download; **không** hiển thị freq/PGA |

**API calib trên web (auth bắt buộc):**

| Method | URI | Body | Vai trò |
|--------|-----|------|---------|
| `POST` | `/api/calib/auto?api=1` | `application/x-www-form-urlencoded`: `phase=A\|B\|C&field=V\|I&value=<ref\|offset>` | `energy_meter_auto_calibrate` (manual ref, 20 samples, 200 ms, tol 0.2%) |
| `POST` | `/save` | form config (cùng form settings) | Lưu config + **calib NVS** + reboot |
| ~~`GET /api/calib`~~ | — | **Không tồn tại** | Số đo xem trên LCD |
| ~~`POST /api/calib/save`~~ | — | **Không tồn tại** | Dùng Save and restart |
| ~~`GET /api/calib/export`~~ | — | **Không tồn tại** | Backup = SD/LCD |
| ~~`POST /api/calib/import`~~ | — | **Không tồn tại** | Import file = SD/LCD only |

Browser form calib: `fetch(...?api=1)` + urlencoded. Không script: form POST thường → redirect `/#calib` khi OK.

---

### Q.1 PASS — UI gọn

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| Q10 | Section layout | Mở `/#calib` | Muted help + **Active profile** = `3P4W` hoặc `3P3W` only + Status `Ready` + form + Calibrate | ☐ PASS ☐ FAIL | no freq/PGA on portal |
| Q14 | Profile refresh | Calibrate OK | `#calib-profile` refresh via `GET /api/calib/profile` | ☐ PASS ☐ FAIL | |
| Q15 | 3P3W hide phase B V | Mode 3P3W, Channel=V | Option B ẩn/disabled; Channel=I thì B hiện lại | ☐ PASS ☐ FAIL | UI cũ giữ nguyên; server giờ chặn MỌI calib ở 3P3W |
| Q16 | 3P3W reject mọi calib | curl `phase=A&field=V` (hoặc `phase=B&field=V`, hoặc field=I) khi 3P3W | `400` + "Calibration is disabled in 3P3W. Switch to 3P4W…" | ☐ PASS ☐ FAIL | Gate ở server (`web_portal.c`) khớp gate trong `energy_meter_auto_calibrate_multi` |
| Q11 | Không Live/Refresh | Xem HTML `#calib` | Không chữ Live, không nút Refresh, không gọi `/api/calib` | ☐ PASS ☐ FAIL | |
| Q12 | Không Save NVS / Download | Xem `#calib` | Không nút Save to NVS, không link Download .bin | ☐ PASS ☐ FAIL | |
| Q13 | Removed endpoints | `GET /api/calib`, `POST /api/calib/save`, `GET /api/calib/export` | **404** / không handler | ☐ PASS ☐ FAIL | |

---

### Q.2 PASS — Auto-calibrate (giá trị thực)

> Cần nguồn/tải chuẩn. Auto-cal **apply chip ngay**. NVS chỉ khi **Save and restart**.

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| Q20 | Voltage phase A | Áp chuẩn (vd 230 V) pha A; Phase=A, Channel=V, value=`230` → Calibrate | Status: `OK VA 230.000 -> …. Check LCD, then Save and restart.` HTTP 200 | ☐ PASS ☐ FAIL | before→after trên status |
| Q21 | Voltage B, C | Lặp Q20 cho B, C | Tương tự `OK VB…` / `OK VC…` | ☐ PASS ☐ FAIL | |
| Q22 | Current phase A | Dòng chuẩn (vd 5 A); Channel=I, value=`5` | `OK IA …. Check LCD, then Save and restart.` | ☐ PASS ☐ FAIL | |
| Q22b | I out of range | PGA bất kỳ; ref/meas khiến Igain >65535 | `Failed: current out of range at PGA xN…` console set PGA; **không** auto tăng PGA; **không** OK gain | ☐ PASS ☐ FAIL | Console: `meter-cal set --field pga` |
| Q22c | I out of range still | Sau khi set PGA tay vẫn overflow | Fail tương tự; không đổi PGA | ☐ PASS ☐ FAIL | |
| Q23 | Current B, C | Lặp Q22 | `OK IB…` / `OK IC…` | ☐ PASS ☐ FAIL | |
| Q24 | Offset no-load V | Không áp; value=`offset`, Channel=V, Phase=A | `OK VA offset old->new. Check LCD, then Save and restart.` | ☐ PASS ☐ FAIL | Chỉ `offset` / `OFFSET` |
| Q25 | Offset no-load I | value=`offset`, Channel=I | `OK IA offset …` | ☐ PASS ☐ FAIL | |
| Q26 | Field aliases | curl `field=u` hoặc `field=U` | Coi như Voltage, auto-cal chạy | ☐ PASS ☐ FAIL | UI chỉ gửi `V`/`I` |
| Q27 | Phase case | curl `phase=a` | Coi như A | ☐ PASS ☐ FAIL | |
| Q28 | UI không reload | Calibrate từ browser (JS bật) | `#calib-st` cập nhật; ô value **giữ** (`data-keep`); WiFi/MQTT typed chưa save còn nguyên | ☐ PASS ☐ FAIL | |
| Q29 | Verify trên LCD | Sau Q20: xem home LCD + `meter-cal show` | Gain mới trên chip; reading gần ref hơn | ☐ PASS ☐ FAIL | Không Refresh web |
| Q2A | Curl auto | `curl -b c -X POST -d "phase=A&field=V&value=230" "http://192.168.4.1/api/calib/auto?api=1"` | `200` + dòng `OK VA…` | ☐ PASS ☐ FAIL | urlencoded |

---

### Q.3 PASS — Persist qua Save and restart

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| Q30 | Save after verify | Q20 → **Save and restart** | Reboot; config + calib NVS được ghi (`energy_meter_save_calibration` trong `/save`) | ☐ PASS ☐ FAIL | |
| Q31 | Persist reboot | Sau Q30 → `meter-cal show` | Gain/mode khớp trước reboot | ☐ PASS ☐ FAIL | |
| Q32 | Calib only path | Chỉ Calibrate, **không** Save and restart → hard reboot | Gain **không** còn (chưa NVS) | ☐ PASS ☐ FAIL | |
| Q33 | Backup vẫn SD | LCD → Calibration → Export to SD | File `calib_*W_*.bin` như cũ | ☐ PASS ☐ FAIL | Không qua web |

---

### Q.4 REJECT / FAIL — Input & protocol

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| Q40 | Bad phase | `phase=X&field=V&value=230` | `400` + `phase must be A, B or C` | ☐ PASS ☐ FAIL | |
| Q41 | Bad field | `phase=A&field=P&value=230` | `400` + `field must be V or I` | ☐ PASS ☐ FAIL | |
| Q42 | Empty value | `value=` (hoặc thiếu) | `400` + `enter a positive value, or offset` | ☐ PASS ☐ FAIL | |
| Q43 | Zero / negative | `value=0` hoặc `value=-1` | `400` same as Q42 | ☐ PASS ☐ FAIL | |
| Q44 | Non-numeric | `value=abc` (không phải offset) | `400` same | ☐ PASS ☐ FAIL | |
| Q45 | `Offset` mixed case | `value=Offset` | **REJECT** (chỉ exact `offset`/`OFFSET`) | ☐ PASS ☐ FAIL | |
| Q46 | Bad / empty body | POST auto không body hoặc body > 8192 | `bad form` (api) / 400 | ☐ PASS ☐ FAIL | `read_form_body` |
| Q47 | Multipart auto body | POST auto với `multipart/form-data` | Parse fail / bad fields → 400 (JS **phải** urlencoded) | ☐ PASS ☐ FAIL | |
| Q48 | Auto fail hardware | Ref lệch lớn / không tín hiệu → auto fail | `400` + `Failed: <esp_err> (rolled back)` nếu rollback; gain cũ giữ | ☐ PASS ☐ FAIL | |
| Q49 | 3P3W chặn calib | Mode 3P3W, auto (V hoặc I, phase bất kỳ) | `400` + "Calibration is disabled in 3P3W…" (server chặn trước khi gọi auto_calibrate_multi) | ☐ PASS ☐ FAIL | Gate web khớp gate console/multi |
| Q4A | Import endpoint gone | `POST /api/calib/import` | **404** | ☐ PASS ☐ FAIL | |
| Q4B | No file picker | Xem HTML `#calib` | Không `<input type=file>` | ☐ PASS ☐ FAIL | |

---

### Q.5 REJECT — Auth & session

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| Q50 | No cookie POST auto | POST auto không cookie | Không chạy calib; redirect login | ☐ PASS ☐ FAIL | Nếu auth bật |
| Q51 | No cookie /save | POST `/save` không cookie | Không ghi config/calib | ☐ PASS ☐ FAIL | |
| Q52 | Auth off | `CONFIG_APP_CONSOLE_AUTH_ENABLE=n` | Portal dùng được không login | ☐ PASS ☐ FAIL | Ghi rõ build |

---

### Q.6 Tích hợp / không phá portal khác

| # | Test | Method | Expected | Result | Notes |
|---|------|--------|----------|--------|-------|
| Q60 | Calibrate alone | Chỉ Calibrate | **Không** reboot; MQTT/WiFi NVS không đổi | ☐ PASS ☐ FAIL | |
| Q61 | Save and restart | Đổi device name + đã calib → Save and restart | Reboot; cả config và calib NVS còn | ☐ PASS ☐ FAIL | |
| Q62 | Cert upload vẫn OK | Upload PEM MQTT | Cert status cập nhật; `max_uri_handlers=22` đủ | ☐ PASS ☐ FAIL | |
| Q63 | Concurrent measure | Auto-cal đang chạy; LCD/home vẫn poll | Không deadlock; sau calib reading ổn | ☐ PASS ☐ FAIL | Mutex meter |
| Q64 | Active profile only | Calib web khi 3P4W; chuyển 3P3W (LCD) | Profile 3P3W **không** bị ghi đè bởi auto-cal 4W | ☐ PASS ☐ FAIL | |

---

### Q.7 Gợi ý curl (session)

```bash
# Login (lấy cookie) — chỉnh user/pass theo menuconfig
curl -c cookie.txt -X POST -d "user=admin&pass=admin" http://192.168.4.1/login

# Auto voltage A @ 230 V
curl -b cookie.txt -X POST \
  -H "Content-Type: application/x-www-form-urlencoded" \
  -d "phase=A&field=V&value=230" \
  "http://192.168.4.1/api/calib/auto?api=1"

# Auto offset current A
curl -b cookie.txt -X POST \
  -d "phase=A&field=I&value=offset" \
  "http://192.168.4.1/api/calib/auto?api=1"

# Persist config + calib then reboot (submit full settings form in browser,
# or POST /save with the same fields the page sends)
# curl -b cookie.txt -X POST -d "..." http://192.168.4.1/save
```

---

## R. Multi-Phase Auto-Calibration (cả 3 pha một lệnh)

> `meter-cal auto` bỏ `--phase` (hoặc `--phase all`) calib **cả 3 pha** với một reference chung,
> capture đồng thời (mỗi mẫu đọc chip MỘT lần, lấy tất cả pha từ cùng snapshot), áp **một lần**.
> Chỉ chạy ở **3P4W**. All-or-nothing: một pha fail → không ghi gì.

| # | Test | Command / Method | Expected | Result | Notes |
|---|------|------------------|----------|--------|-------|
| R01 | U gain cả 3 pha | 3P4W; một nguồn AC + trung tính cấp Ua=Ub=Uc=220V → `meter-cal auto --field u --value 220` | 3 pha capture cùng snapshot, tính gain từng pha, apply 1 lần, log residual error% A/B/C, `calibrated_mask=0x07` | ☐ PASS ☐ FAIL | Không cần `--apply` |
| R02 | I gain cả 3 pha | 3 CT kẹp cùng tải 5A → `meter-cal auto --field i --value 5` | Như R01 cho dòng | ☐ PASS ☐ FAIL | |
| R03 | `--phase all` tường minh | `meter-cal auto --field u --phase all --value 220` | Giống hệt R01 (all = 0x07) | ☐ PASS ☐ FAIL | |
| R04 | Một pha vẫn đúng | `meter-cal auto --field u --phase b --value 220` | Chỉ pha B đổi gain; A, C giữ nguyên | ☐ PASS ☐ FAIL | mask=0x02 |
| R05 | Một CT hở → fail không ghi | 3 CT nhưng hở 1 (vd pha C) → `meter-cal auto --field i --value 5` | Capture/compute fail pha C → `ESP_ERR_INVALID_RESPONSE`, **không ghi gì**, chip giữ calib cũ, log chỉ rõ pha C | ☐ PASS ☐ FAIL | All-or-nothing |
| R06 | Một pha gain ngoài range | Ref/meas lệch lớn trên 1 pha → gain >65535 | `ESP_ERR_INVALID_SIZE`, không ghi gì, PGA không đổi, log pha lỗi | ☐ PASS ☐ FAIL | |
| R07 | Offset cả 3 pha | Không tải (0V) → `meter-cal auto --field u --value offset` | Zero cả 3 offset → apply → đo residual → ghi offset từng pha; int16 range-check TẤT CẢ trước khi ghi | ☐ PASS ☐ FAIL | |
| R08 | Offset 1 pha ngoài int16 | Residual 1 kênh quá lớn | `ESP_ERR_INVALID_SIZE`, không ghi offset nào, rollback về original | ☐ PASS ☐ FAIL | |
| R09 | External ref per-phase | 3P4W, meter ngoài online → `meter-cal auto --field u --value external` | Ref đọc riêng `voltage[p]` từng pha; pha nào invalid → fail pha đó, không ghi | ☐ PASS ☐ FAIL | |
| R10 | 3P3W chặn all-3 | 3P3W → `meter-cal auto --field u --value 220` | `ESP_ERR_NOT_SUPPORTED`, không ghi, log gợi ý chuyển 3P4W | ☐ PASS ☐ FAIL | |
| R11 | Verify fail không rollback | Inject SPI lỗi ở lần đo AFTER (sau khi apply gain thành công) | Gain GIỮ trên chip, `rolled_back=false`, WARN "written but verify read failed"; trả mã lỗi verify | ☐ PASS ☐ FAIL | committed=true |
| R12 | Apply fail → rollback | Inject SPI lỗi ở lần apply gain | `apply_locked(&original)`, `rolled_back=true`, không có gain mới | ☐ PASS ☐ FAIL | |
| R13 | Compute fail → không write | Inject gain compute fail (trước apply) | Không có lệnh ghi chip nào (`applied=false`), log "failed before any chip write", `rolled_back=false` | ☐ PASS ☐ FAIL | Tránh write thừa |
| R14 | Không đổi gain pha ngoài mask | `meter-cal auto --field u --phase a --value 220`, `meter-cal show` trước/sau | Chỉ voltage_gain A đổi; B, C và mọi offset/pq_gain/phi giữ nguyên | ☐ PASS ☐ FAIL | |
| R15 | Không đổi PGA/freq | R01/R02, `meter-cal show` trước/sau | pga_gain, line_freq, wiring_mode không đổi | ☐ PASS ☐ FAIL | working re-pin từ original |
| R16 | int64 khớp double | Ghi lại ref/meas từ R01; tính tay `round(old*ref/meas)` | Gain firmware = kết quả tính tay (round half-up) | ☐ PASS ☐ FAIL | Đã verify offline bằng Python |

---

## Ghi chép từng test

```text
Test ID:   ________
Board:     ________
Firmware:  ________
Wiring:    ☐ 3P4W  ☐ 3P3W
Frequency: ☐ 50 Hz  ☐ 60 Hz
PGA:       ☐ ×1  ☐ ×2  ☐ ×4
Lệnh chạy: __________________________________________________
Expected:  ________
Actual:    ________
PASS/FAIL: ________
Notes:     __________________________________________________
```

---

## Tiêu chí tối thiểu để dùng đo thực tế

| Nhóm | Bắt buộc? | Ghi chú |
|------|-----------|---------|
| A–H | ✅ Bắt buộc | Core safety |
| I–J | ✅ Bắt buộc | Auto-calibration (một pha) |
| K | ⚠️ Nếu dùng external | Bỏ qua nếu chỉ calib thủ công |
| L–M | ✅ Bắt buộc | Semantics |
| N | ✅ Bắt buộc | Persistence |
| O | ✅ Bắt buộc | Accuracy |
| P | ✅ Khuyến nghị | Tin cậy dài hạn |
| Q | ⚠️ Nếu dùng Config Portal | Web calib + export; **không** test import file web |
| R | ✅ Bắt buộc | Multi-phase auto-cal (cả 3 pha một lệnh) |

---

## Các mục đã loại khỏi bảng test

| Mục | Lý do loại |
|-----|-----------|
| ~~G03~~ Copy profile | API `energy_meter_copy_calibration_profile()` đã có trong code C nhưng **chưa có lệnh console**. Cần thêm subcommand `meter-cal copy` trước |
| ~~N03~~ Factory reset | API `energy_meter_erase_calibration()` đã có trong code C nhưng **chưa có lệnh console**. Cần thêm subcommand `meter-cal erase` trước |
| ~~Web calib import~~ | **Cố ý không implement** — import `.bin` chỉ LCD + SD (`sd_card_calib_import` → apply current mode). Portal chỉ nhập reference + download backup |
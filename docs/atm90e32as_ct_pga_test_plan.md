# Test Plan — CT / PGA / Factory Gain (Locked Plan)

**Firmware:** FIRMWARE/uart_echo  
**Date:** 2026-08-20  
**Board S/N:** ____________________  
**Tester:** ____________________  
**Scope:** Verify the locked CT/PGA/factory-gain design described in
[`main/Kconfig.projbuild`](main/Kconfig.projbuild) ATM90 block,
[`components/atm90e32as/atm90e32as.c`](components/atm90e32as/atm90e32as.c),
[`main/app/config_manager.c`](main/app/config_manager.c),
[`main/app/energy_meter_task.c`](main/app/energy_meter_task.c),
[`main/app/home_screen.c`](main/app/home_screen.c) (Current CT menu),
and [`docs/atm90e32as_test_plan.md`](docs/atm90e32as_test_plan.md).

> Test plan cũ (A–Q) vẫn chạy song song. File này **chỉ** focus các thay đổi
> sau chốt: factory gain = `0x8000`, CT params (NCT / I_Rated / I_Expected)
> là single-set trong menu LCD, PGA chip-wide từ CT Apply, SD bin format C.

---

## Khái niệm bắt buộc nắm trước khi test

| Symbol | Ý nghĩa | Nguồn giá trị |
|---|---|---|
| NCT | Tỉ số CT (vd 2000:5) | `config_manager.ct_ratio` (CT Apply) |
| I_Rated | Dòng danh định CT (vd 100 A) | `config_manager.i_rated_a` |
| I_Expected | Dòng vận hành kỳ vọng (vd 75 A) | `config_manager.i_expected_a` |
| R_BURDEN | Điện trở burden (4400 mΩ = 4.4 Ω) | `CONFIG_APP_ATM90E32AS_R_BURDEN_MOHM` (Kconfig only) |
| PGA | Gain chip-wide (1, 2, 4) | `energy_meter_ct_apply()` chọn tự động |
| VADC_LIMIT | Ngưỡng an toàn ADC = 0.72 Vrms | const `ENERGY_METER_VADC_LIMIT_V` |
| Ilim | Dòng giới hạn ở PGA đang chọn: `0.72 × NCT / (R × PGA)` | công thức runtime |
| FACTORY_GAIN | Ugain/IGain/PqGain/FundGain mặc định = **0x8000** | const `ENERGY_METER_FACTORY_GAIN` |

### Công thức chọn PGA (CT Apply)

```
Ilim(PGA=4) = 0.72 * NCT / (R_BURDEN * 4)
Ilim(PGA=2) = 0.72 * NCT / (R_BURDEN * 2)
Ilim(PGA=1) = 0.72 * NCT / (R_BURDEN * 1)
PGA chọn = PGA cao nhất có Ilim ≥ I_Expected (gate duy nhất).
Nếu PGA đó có Ilim < I_Rated → trade-off (Rated headroom bị cắt), LCD cảnh báo.
Nếu PGA=1 v�n < I_Expected → giữ PGA=1, range warning, I_Expected không bị mutate.
```

Ví dụ (default N16R8 menuconfig): NCT=2000, R=4.4 Ω → Ilim(×4)=81.8 A,
Ilim(×2)=163.6 A, Ilim(×1)=327.3 A.

- `Rated=100, Expected=75`: walk 1→2→4. Ilim(×4)=81.8 ≥ 75 → chọn ×4 (Rated bị cắt).
- `Rated=100, Expected=100`: walk 1→2. Ilim(×2)=163.6 ≥ 100 → chọn ×2 (không thử ×4 vì Ilim(×4)=81.8 < 100 = Expected).
- `Rated=500, Expected=200`: walk 1. Ilim(×1)=327.3 ≥ 200 → chọn ×1.

### Boundary ownership

| Domain | Owner |
|---|---|
| Phase gains (Ugain/Igain/PqGain, offsets, phase_comp, fundamental) | NVS `profiles_v2` + SD bin format C |
| NCT / I_Rated / I_Expected | `config_manager` (DTO `has_ct_params` tail) |
| R_BURDEN | Kconfig (build-time only) |
| PGA | Hệ thống: `energy_meter_ct_apply()` hoặc NVS restore |

---

## Bảng tra nhanh thanh ghi

| Address | Tên | Ý nghĩa |
|---|---|---|
| `0x33` | MMode0 | wiring + freq |
| `0x34` | MMode1 | PGA bit map |
| `0x61`/`0x64`/`0x67` | Ugain A/B/C | factory = `0x8000` |
| `0x62`/`0x65`/`0x68` | Igain A/B/C | factory = `0x8000` |
| `0x32`/`0x35`/`0x38` | Uoffs A/B/C | 0 sau factory |
| `0x53`/`0x54`/`0x55` | P/Q offset (combined) | 0 |
| `0x07`/`0x08`/`0x09` | Phase Comp A/B/C | 0 |

Công thức đọc nhanh: dùng `meter-reg read 0x61 0x62 0x64 0x65 0x67 0x68`.

---

## R. Chuẩn bị

| # | Bước | Expected |
|---|---|---|
| R00 | `idf.py erase-flash` (đổi partition sang dual-OTA) | OK |
| R01 | `idf.py -p PORT flash` | Boot, log "Calibration loaded" / "Factory defaults applied" |
| R02 | Vào LCD → Settings → Meter Setup → Current CT | Thấy 3 dòng: NCT, I Rated, I Expected |
| R03 | `meter-cal show` | Hiện 3 giá trị = default Kconfig (2000 / 100 / 75) |
| R04 | `meter-reg read 0x61 0x62 0x64 0x65 0x67 0x68` | Tất cả = `0x8000` |

---

## S. Factory Gain (đo factory trước khi calib)

| # | Test | Method | Expected | Result | Notes |
|---|---|---|---|---|---|
| S01 | Default Ugain/Igain | Sau R04 | Mỗi register `=0x8000` (32768) | ☐ PASS ☐ FAIL | |
| S02 | 3 profile đồng nhất | Làm S01 cho cả 3P4W và 3P3W (chuyển mode + `meter-reg read`) | Cả 2 profile factory = `0x8000` | ☐ PASS ☐ FAIL | |
| S03 | Reboot giữ factory | `meter-cal default --apply` + reboot + `meter-reg read` | Vẫn `0x8000` | ☐ PASS ☐ FAIL | |
| S04 | Kconfig đổi R_BURDEN | Sửa `CONFIG_APP_ATM90E32AS_R_BURDEN_MOHM=2200`, build, `meter-reg read` | Vẫn `0x8000` (R_BURDEN không đụng gain) | ☐ PASS ☐ FAIL | |
| S05 | Không có công thức factory math | grep `components/atm90e32as/atm90e32as.c` | Không có phép tính `Vref/Iref→gain` | ☐ PASS ☐ FAIL | code review |

---

## T. LCD Current CT Menu (draft + Apply)

> Mục đích: thao tác **draft → Back: xác nhận Apply hoặc hủy → save**.
> 3 field: NCT, I_Rated, I_Expected. **Không có** per-phase; **không có**
> R_BURDEN trên LCD (chỉ Kconfig).

| # | Test | Method | Expected | Result | Notes |
|---|---|---|---|---|---|
| T01 | Vào menu Current CT | LCD → Settings → Meter Setup → Current CT | Hiện 3 dòng (NCT/Rated/Expected) | ☐ PASS ☐ FAIL | |
| T02 | Sửa 1 field | Đổi NCT 2000 → 2100 (step 100), Back không lưu | LCD vẫn `NCT=2000`, NVS không đổi | ☐ PASS ☐ FAIL | cancel path; NCT step=100 min=1000 max=6000 |
| T03 | Back không dirty | Mở menu, không sửa, Back | Ra Settings ngay, không hỏi Apply | ☐ PASS ☐ FAIL | |
| T04 | Back dirty → Ask Apply | Đổi 1 field, Back | LCD hỏi "Apply changes?" Yes/No | ☐ PASS ☐ FAIL | |
| T05 | Yes → Apply, vẫn ở menu | Trong T04, chọn Yes | Áp dụng vào RAM + NVS, LCD vẫn ở Current CT (không thoát) | ☐ PASS ☐ FAIL | |
| T06 | No → Discard | Trong T04, chọn No | LCD thoát Settings, NVS không đổi | ☐ PASS ☐ FAIL | |
| T07 | Apply clamp Expected (range warning) | Set R_BURDEN=2200 mΩ, CT=2000/100/250 → Apply | `Ilim(×1)=163.6 < 250` → range warning, giữ PGA=1, Expected KHÔNG bị mutate (vẫn 250 trong NVS), LCD hiện "RANGE WARN" | ☐ PASS ☐ FAIL | Kconfig-only; build khác |
| T08 | Trade-off Rated (không phải dirty user) | Default R=4.4Ω, Expected=75 → Apply | PGA=×4, `rated_truncated=true`, LCD hiện "TRADE-OFF / Rated headroom reduced" | ☐ PASS ☐ FAIL | |
| T09 | Edit 1 trong 3 → Igain reset cả 2 profile | Sau factory (`0x8000`), set IgainA = 20000 (`meter-cal set`), rồi LCD Current CT → đổi Expected 75→80 → Apply | `meter-reg read 0x62 0x65 0x68` = `0x8000` cho cả 2 profile (3P4W & 3P3W) | ☐ PASS ☐ FAIL | |
| T10 | Edit 1 → Ugain KHÔNG reset | Trước T09 set UgainA=10000; sau T09 | UgainA = 10000 còn; chỉ Igain reset | ☐ PASS ☐ FAIL | chỉ reset Igain |
| T11 | PGA stamped chip-wide | Sau CT Apply, chuyển 3P4W→3P3W | MMode1 không đổi (cùng PGA) | ☐ PASS ☐ FAIL | |
| T12 | Apply giá trị invalid | Set NCT ngoài 1000..6000 hoặc không chia hết 100 → Apply | LCD báo lỗi, NVS không đổi | ☐ PASS ☐ FAIL | min 1000 max 6000 step 100 |

---

## U. PGA Auto-Select

> Gate duy nhất là `I_Expected`. `I_Rated` chỉ đánh dấu `rated_truncated`
> (trade-off) trên LCD — không cản việc chọn PGA cao hơn.

| # | Test | Method | Expected | Result | Notes |
|---|---|---|---|---|---|
| U01 | CT 2000 / 100 / 75 (default) | Apply | `Ilim(×4)=81.8 ≥ 75` → chọn ×4. `Ilim(×4)<100` → `rated_truncated=true`. MMode1 = `0x002A`, LCD "TRADE-OFF" | ☐ PASS ☐ FAIL | trade-off |
| U02 | CT 2000 / 100 / 100 | Apply | `Ilim(×4)=81.8 < 100` (Expected) → dừng ở ×2. MMode1 = `0x0015` | ☐ PASS ☐ FAIL | Expected gate |
| U03 | CT 2000 / 50 / 30 | Apply | `Ilim(×4)=81.8 ≥ 30` và `≥ 50` → ×4. MMode1 = `0x002A`, không trade-off | ☐ PASS ☐ FAIL | |
| U04 | CT 2000 / 500 / 200 | Apply | `Ilim(×1)=327.3 ≥ 200`; `Ilim(×2)=163.6 < 200` → dừng ở ×1. MMode1 = `0x0000`, `rated_truncated=true` | ☐ PASS ☐ FAIL | Rated trade-off |
| U05 | Range Warning (CT 2000 / 500 / 600) | Apply | `Ilim(×1)=327.3 < 600` → range warning. PGA=1, Expected không bị mutate (vẫn 600), LCD "RANGE WARN / PGA=1x" | ☐ PASS ☐ FAIL | clamp flag |
| U05 | Đổi R_BURDEN (Kconfig) → Apply lại | Sửa `R_BURDEN_MOHM=2200`, build, Apply | PGA chọn lại theo R mới | ☐ PASS ☐ FAIL | |
| U06 | Không đổi PGA qua LCD riêng | Sau Apply, LCD không có menu "PGA" | PGA chỉ thay qua CT Apply | ☐ PASS ☐ FAIL | menu không tồn tại |
| U07 | Reboot giữ PGA | Apply + reboot + `meter-reg read 0x34` | PGA giữ nguyên (CT Apply lưu NVS + system-owned PGA) | ☐ PASS ☐ FAIL | |

---

## V. SD Bin Format C

> Định dạng mới: phase calib only (U/I gain+offset, phase_comp, P/Q offset,
> pq_gain, fundamental ×3). **Không** chứa PGA/freq/CT/R_BURDEN.

| # | Test | Method | Expected | Result | Notes |
|---|---|---|---|---|---|
| V01 | Export format C | LCD → Calibration → Export to SD | File `calib_*.bin`, hex dump header 4 byte = `'CALB'` + 16 bytes header + payload | ☐ PASS ☐ FAIL | |
| V02 | Payload không chứa PGA | hex dump blob | Không có trường PGA/freq_line/wiring_mode/CT trong payload; chỉ phase fields | ☐ PASS ☐ FAIL | |
| V03 | CSV metadata | Trong cùng dir có file `.csv` | Có: timestamp, NCT, PGA, wiring (display only) | ☐ PASS ☐ FAIL | display-only |
| V04 | Import — giữ PGA | Apply → Export → đổi PGA tay (`meter-reg write 0x34 …` → `meter-cal apply`) → Import file cũ | PGA quay về giá trị lúc Export | ☐ PASS ☐ FAIL | loaded calib gains thắng |
| V05 | Import — giữ freq | Sau V04, đổi freq, Import | Freq quay về giá trị lúc Export | ☐ PASS ☐ FAIL | |
| V06 | Import — giữ CT | Sau V04, đổi CT qua LCD, Import | CT không bị ghi đè bởi Import (CT ở config_manager, không nằm trong bin C) | ☐ PASS ☐ FAIL | |
| V07 | Backward compat v1/v2 | Import file cũ v1 (full) hoặc v2 (`atm90e32as_calib_t` full) | Hệ thống accept; phase fields dùng được; PGA/freq/CT refs giữ nguyên running | ☐ PASS ☐ FAIL | |
| V08 | CRC32 fail | Sửa 1 byte giữa payload, Import | Reject, LCD báo CRC fail | ☐ PASS ☐ FAIL | |
| V09 | Magic fail | Sửa magic thành `XXXX` | Reject ngay | ☐ PASS ☐ FAIL | |
| V10 | Version fail | Sửa version=99 | Reject | ☐ PASS ☐ FAIL | |

---

## W. CT ↔ Calib tương tác

| # | Test | Method | Expected | Result | Notes |
|---|---|---|---|---|---|
| W01 | Auto-cal V không đụng CT | Sau CT Apply, chạy `meter-cal auto --field u --phase a --value 230` + Save | CT không đổi; Ugain thay đổi; NVS CT params còn | ☐ PASS ☐ FAIL | |
| W02 | Auto-cal I không tăng PGA | Sau CT Apply, `meter-cal auto --field i --phase a --value 5` | Igain tính lại trên PGA hiện tại; **không** auto tăng PGA | ☐ PASS ☐ FAIL | |
| W03 | Wiring switch không reset CT | Sau CT Apply, chuyển 3P4W↔3P3W (LCD) | CT params còn; PGA còn | ☐ PASS ☐ FAIL | |
| W04 | Factory reset | LCD → Settings → Factory Reset → Confirm | CT params về default (2000/100/75), PGA theo lại, gain = `0x8000` cả 2 profile | ☐ PASS ☐ FAIL | |
| W05 | Reboot sau factory reset | W04 → reboot → `meter-cal show` + `meter-reg read` | CT=2000/100/75, gain=0x8000, PGA theo CT | ☐ PASS ☐ FAIL | |
| W06 | Web Portal không sửa CT | Mở portal, đổi các field khác (WiFi, MQTT), Save and restart | CT params không bị reset về default | ☐ PASS ☐ FAIL | |
| W07 | Console `meter-cal show` hiện CT | `meter-cal show` | Có 3 dòng `CT: ratio=… rated=… expected=…` | ☐ PASS ☐ FAIL | |

---

## X. Negative / Edge

| # | Test | Method | Expected | Result | Notes |
|---|---|---|---|---|---|
| X01 | NCT ngoài cửa sổ (0 / 999 / 6001 / 2050) | LCD Apply / update | Reject (LCD edit snap step 100; API reject) | ☐ PASS ☐ FAIL | 1000..6000 step 100 |
| X02 | I_Rated = 0 | Apply | Reject (I_Rated > 0) | ☐ PASS ☐ FAIL | |
| X03 | I_Expected > I_Rated | Apply | Không reject (Expected có thể > Rated); nhưng max(Rated, Expected) mới là input PGA | ☐ PASS ☐ FAIL | check formula |
| X04 | R_BURDEN đổi build-time | Edit Kconfig, build, Apply | PGA đổi theo R mới | ☐ PASS ☐ FAIL | rebuild required |
| X05 | Kconfig CT_RATIO khác 2000 | Edit `CONFIG_APP_ATM90E32AS_CT_RATIO=4000`, build factory | Default CT = 4000 (lưu NVS lần đầu) | ☐ PASS ☐ FAIL | |
| X06 | LCD menu giữa lúc auto-cal | Đang auto-cal V, vào Current CT | Bị block hoặc busy; tránh đụng CT khi chip đang apply | ☐ PASS ☐ FAIL | mutex |
| X07 | Power cycle giữa Apply | Cắt nguồn đúng lúc Apply CT | NVS nguyên (atomic); boot về giá trị trước Apply | ☐ PASS ☐ FAIL | |

---

## Y. Tiêu chí pass

| Nhóm | Bắt buộc? |
|---|---|
| R, S | ✅ Build OK + factory = `0x8000` cho mọi phase |
| T | ✅ LCD Current CT draft/Apply UX đúng |
| U | ✅ PGA auto-select đúng công thức |
| V | ✅ SD bin C: payload phase-only, Import không đụng PGA/freq/CT |
| W | ✅ CT độc lập với auto-cal/wiring/factory reset |
| X | ⚠️ Negative — ít nhất X01–X03 + X05 |

---

## Ghi chép

```text
Test ID:   ________
Board:     ________
Firmware:  ________
Wiring:    ☐ 3P4W  ☐ 3P3W
Frequency: ☐ 50 Hz  ☐ 60 Hz
PGA:       ☐ ×1  ☐ ×2  ☐ ×4
CT:        NCT=_____ Rated=_____A Expected=_____A
R_BURDEN:  _____ mΩ   (Kconfig)
Lệnh chạy: __________________________________________________
Expected:  ________
Actual:    ________
PASS/FAIL: ________
Notes:     __________________________________________________
```
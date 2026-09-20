# MQTT Payload Reference

> **Single Source of Truth** cho toàn bộ MQTT topics và payload format của thiết bị.  
> Source code: [main/app/mqtt_manager.c](../main/app/mqtt_manager.c)  
> Data structures: [main/app/mqtt_telemetry.h](../main/app/mqtt_telemetry.h)

**Phiên bản tài liệu:** 2.5 (cập nhật 2026-09-20)  
**Thay đổi chính (2.5):**
- `heartbeat`: thêm `ts` (epoch giây), `tq` (time quality: `U`/`E`/`S`) và `boot`
  (số lần khởi động). **Phải đọc `tq` trước khi tin `ts`** — xem [§3.4](#34-pmidheartbeat)
- Đính chính: energy **KHÔNG** nằm trong thanh ghi ATM90E32AS. Thanh ghi tổng năng
  lượng của IC là **read-to-clear** (đọc xong tự về 0), nên **bộ đếm trong RAM của
  firmware chính là chỉ số công-tơ**; nó được ghi vào NVS theo ngưỡng/chu kỳ và
  khôi phục khi boot — xem [§3.2](#32-pmidenergy)

**Thay đổi chính (2.4):**
- `io`: 2 digital input giờ **event-driven** — đổi mức là publish ngay (≤ 250 ms),
  không chờ hết chu kỳ publish
- Thêm `warn_bits`: bitmap alarm **từng pha** (16-bit) bên cạnh `warnings` (byte
  tóm tắt theo nhóm) — cùng dữ liệu mà trang ALARMS trên LCD hiển thị

**Thay đổi chính (2.3):**
- `warnings` không còn là reserved: đã nối với alarm backend (ATM90E32AS native
  warning) — xem bảng bit bên dưới

**Thay đổi chính (2.2):**
- Thêm `p_kw_ph`: công suất P từng pha (kW) cho main và slaves
- Noise floor cho slave meters (|PF| < 0.1, |P|/|Q|/|S| < 1) như main meter
- Giá trị sau làm tròn về 0 luôn là +0 — không bao giờ publish/hiển thị `-0`
- No-load gate theo dòng (mặc định 50 mA, `APP_METER_NOLOAD_CURRENT_MA`):
  pha không có tải → I/P/Q/S/PF = 0, chống nhiễu xuyên âm giữa các CT xếp sát

**Thay đổi chính (2.1):**
- Slaves: thêm trường `state` (on/off/inactive), `online` true chỉ khi `state="on"`
- Data authenticity: khi state ≠ on, mọi giá trị đo về 0 — không publish stale data

**Thay đổi chính (2.0):**
- Power units: W/var/VA → **kW/kvar/kVA** (2 decimal places)
- Multi-device support: main device + up to 5 Modbus slaves
- IO states moved into telemetry payload
- Relay command simplified to plain string

---

## 📋 Mục lục

1. [Topic Naming & Device ID](#1-topic-naming--device-id)
2. [Topic Overview Table](#2-topic-overview-table)
3. [Published Topics (Device → Broker)](#3-published-topics-device--broker)
4. [Subscribed Topics (Broker → Device)](#4-subscribed-topics-broker--device)
5. [Field Reference Dictionary](#5-field-reference-dictionary)
6. [Integration Examples](#6-integration-examples)

---

## 1. Topic Naming & Device ID

### Device ID Format

**Topic prefix:** `pm/<device_id>/...`

- `<device_id>` được lấy từ NVS config `device_name` (default: `"PM-" + 6 hex MAC`)
- **Sanitized:** ký tự không hợp lệ MQTT topic (`/`, `+`, `#`, space) → thay bằng `_`
- Ví dụ:
  - Config name: `Power Meter` → Topic: `pm/Power_Meter/...`
  - Config name: `Lab/Meter#1` → Topic: `pm/Lab_Meter_1/...`

### MQTT Client ID

**Format:** `<device_name>-<last_3_bytes_MAC>`

- Ví dụ: `Power_Meter-3AF2C1`
- Đảm bảo unique khi nhiều device cùng tên trên một broker

---

## 2. Topic Overview Table

| Topic | Direction | QoS | Retain | Publish Interval | Description |
|-------|-----------|-----|--------|------------------|-------------|
| `pm/<id>/telemetry` | Publish | 0 | No | Configurable (5-60s, default 5s) | Real-time measurements (main + slaves) |
| `pm/<id>/energy` | Publish | 1 | No | Same as telemetry | Accumulated energy + demand |
| `pm/<id>/io` | Publish | 1 | **Yes** | Same as telemetry + **đổi mức input** + relay echo | Digital I/O state snapshot |
| `pm/<id>/heartbeat` | Publish | 0 | No | Same as telemetry | System health + metadata |
| `pm/<id>/status` | Publish | 1 | **Yes** | On connect + LWT | Online/offline presence |
| `pm/<id>/cmd/out0` | Subscribe | 1 | — | On demand | Relay output 0 control |
| `pm/<id>/cmd/out1` | Subscribe | 1 | — | On demand | Relay output 1 control |

**Notes:**
- Publish interval configurable via: LCD Menu (Settings → MQTT → Period), Web Portal (MQTT section), Console (`mqtt-cfg period`)
- Range: 5-60 seconds (stored in NVS as milliseconds: 5000-60000 ms)

---

## 3. Published Topics (Device → Broker)

### 3.1. `pm/<id>/telemetry`

**QoS:** 0 (fire-and-forget, real-time priority)  
**Retain:** No  
**Frequency:** Every publish interval (default 5s)

**Purpose:** Real-time electrical measurements from main meter (ATM90E32AS) and optional Modbus slave devices (PM710/EM07K).

#### Payload Structure

```json
{
  "main": {
    "v": [230.1, 230.2, 230.3],
    "i": [5.23, 5.18, 5.31],
    "pf": [0.98, 0.97, 0.99],
    "in": 0.05,
    "p_kw": 3.45,
    "p_kw_ph": [1.10, 1.15, 1.20],
    "q_kvar": 0.23,
    "s_kva": 3.46,
    "pf_total": 0.98,
    "freq": 50.01,
    "temp": 42.3,
    "energy_kwh": 1234.56,
    "relay1": false,
    "relay2": true,
    "input1": false,
    "input2": true,
    "warnings": 0,
    "warn_bits": 0
  },
  "slaves": [
    {
      "name": "PM710-01",
      "id": 1,
      "type": "PM710",
      "state": "on",
      "online": true,
      "v": [230.0, 230.1, 230.2],
      "i": [2.10, 2.15, 2.18],
      "p_kw": 1.52,
      "p_kw_ph": [0.50, 0.51, 0.51],
      "q_kvar": 0.11,
      "s_kva": 1.53,
      "pf": 0.99,
      "freq": 50.00,
      "energy_kwh": 567.89
    }
  ]
}
```

#### Field Details

**`main` object** (always present):

| Field | Type | Unit | Description | Range/Notes |
|-------|------|------|-------------|-------------|
| `v` | number[3] | V | Phase voltages (L1, L2, L3) | 0-300 V typical |
| `i` | number[3] | A | Phase currents (L1, L2, L3) | 0-rated current |
| `pf` | number[3] | — | Per-phase power factor | -1.0 to 1.0 |
| `in` | number | A | Neutral current | 0-rated current |
| `p_kw` | number | kW | Total active power | **2 decimals** |
| `p_kw_ph` | number[3] | kW | Active power per phase (L1, L2, L3) | **2 decimals** |
| `q_kvar` | number | kvar | Total reactive power | **2 decimals** |
| `s_kva` | number | kVA | Total apparent power | **2 decimals** |
| `pf_total` | number | — | System power factor | -1.0 to 1.0 |
| `freq` | number | Hz | Line frequency | 45-65 Hz typical |
| `temp` | number | °C | ATM90E32AS chip temperature | Internal sensor |
| `energy_kwh` | number | kWh | Accumulated active energy (import) | Bộ đếm trong RAM + NVS, xem [§3.2](#32-pmidenergy) |
| `relay1` | boolean | — | Relay output 0 state | true=closed/on |
| `relay2` | boolean | — | Relay output 1 state | true=closed/on |
| `input1` | boolean | — | Digital input 0 state | true=high/active |
| `input2` | boolean | — | Digital input 1 state | true=high/active |
| `warnings` | number | — | Alarm latched, tóm tắt theo nhóm (8-bit) | 0 = không có cảnh báo |
| `warn_bits` | number | — | Alarm latched, chi tiết từng pha (16-bit) | 0 = không có cảnh báo |

#### Alarm: `warnings` và `warn_bits`

Cả hai đều là trạng thái **latched** (chốt): bit đã set thì **giữ nguyên kể cả khi
sự cố đã hết**, chỉ xoá bằng **LCD → Settings → Alarm → Reset Latch**. Sau khi
reset, nếu sự cố vẫn còn thì bit sẽ chốt lại sau hết cửa sổ xác nhận
(`alarm_trigger_delay_s`, mặc định 2 s).

Nguồn: cảnh báo **native của ATM90E32AS** (IC tự so ngưỡng trong thanh ghi OVth /
SagTh / PhaseLossTh / OIth / FreqLoTh / FreqHiTh, firmware chỉ đọc kết quả ở
EMMState0 `0x71` / EMMState1 `0x72`) — không phải firmware tự lọc.

**`warnings`** — 1 byte, mỗi bit gộp cả 3 pha (dùng cho dashboard/cảnh báo nhanh):

| Bit | Giá trị | Ý nghĩa | Phục vụ cho | Nguồn (ATM90E32AS) |
|-----|---------|---------|-------------|--------------------|
| 0 | 0x01 | Over-voltage (pha bất kỳ) | Quá áp lưới — bảo vệ tải | EMMState0 b12–10 |
| 1 | 0x02 | Under-voltage / sag (pha bất kỳ) | Sụt áp — motor/UPS, chất lượng điện | EMMState1 b14–12 |
| 2 | 0x04 | Over-current (pha bất kỳ) | Quá tải / ngắn mạch — cắt tải | EMMState0 b15–13 |
| 3 | 0x08 | Phase loss (pha bất kỳ) | Mất pha — nguy hiểm cho motor 3 pha | EMMState1 b10–8 |
| 4 | 0x10 | Frequency high | Tần số cao — lỗi nguồn/máy phát | EMMState1 b15 |
| 5 | 0x20 | Frequency low | Tần số thấp — quá tải máy phát | EMMState1 b11 |
| 6 | 0x40 | IC error (Internal / CfgCRC) | Lỗi chính IC đo — **số liệu không tin được** | chân WarnOut (GPIO 41) |
| 7 | 0x80 | — | dự phòng, luôn 0 | — |

**`warn_bits`** — 16-bit, cho biết **pha nào** bị (đúng dữ liệu trang ALARMS trên LCD):

| Bit | Giá trị | Ý nghĩa | | Bit | Giá trị | Ý nghĩa |
|-----|---------|---------|-|-----|---------|---------|
| 0 | 0x0001 | Over-voltage pha A | | 8 | 0x0100 | Under-voltage pha C |
| 1 | 0x0002 | Over-voltage pha B | | 9 | 0x0200 | Phase loss pha A |
| 2 | 0x0004 | Over-voltage pha C | | 10 | 0x0400 | Phase loss pha B |
| 3 | 0x0008 | Over-current pha A | | 11 | 0x0800 | Phase loss pha C |
| 4 | 0x0010 | Over-current pha B | | 12 | 0x1000 | Frequency high |
| 5 | 0x0020 | Over-current pha C | | 13 | 0x2000 | Frequency low |
| 6 | 0x0040 | Under-voltage pha A | | 14 | 0x4000 | IC error (WarnOut) |
| 7 | 0x0080 | Under-voltage pha B | | 15 | 0x8000 | — dự phòng |

Quan hệ: `warnings` chính là `warn_bits` gộp nhóm — ví dụ `warn_bits = 0x0020`
(quá dòng pha C) ⇒ `warnings = 0x04`.

**Lưu ý về thời điểm báo:** alarm chỉ được đánh giá sau khi firmware đã ghi được
ngưỡng thật vào IC (cần ≥ 2 pha có điện áp ≥ 50 V; riêng over-current còn cần tải
≥ 0.5 A để neo ngưỡng, thử lại mỗi 5 s). Trước đó mọi bit đều 0 — đây là chủ ý,
vì ngưỡng mặc định sau reset của IC sẽ báo sag + mất pha + tần số thấp khi chưa
có lưới. Đổi ngưỡng alarm hoặc hiệu chuẩn (calibration) cũng làm firmware neo
lại ngưỡng, trong lúc đó alarm tạm ngưng đánh giá.

**`slaves` array** (optional, only present if Modbus slaves configured):

| Field | Type | Unit | Description | Range/Notes |
|-------|------|------|-------------|-------------|
| `name` | string | — | Device name from config | User-defined label |
| `id` | number | — | Modbus slave address | 1-247 |
| `type` | string | — | Device model | `"PM710"` or `"EM07K"` |
| `state` | string | — | Device state (tri-state) | `"on"` answering / `"off"` down (5 failed polls) / `"inactive"` master not polling |
| `online` | boolean | — | Comms status (compat) | true chỉ khi `state="on"` |
| `v` | number[3] | V | Phase voltages | Same as main |
| `i` | number[3] | A | Phase currents | Same as main |
| `p_kw` | number | kW | Total active power | **2 decimals** |
| `p_kw_ph` | number[3] | kW | Active power per phase (L1, L2, L3) | **2 decimals** |
| `q_kvar` | number | kvar | Total reactive power | **2 decimals**, 0 for EM07K |
| `s_kva` | number | kVA | Total apparent power | **2 decimals** |
| `pf` | number | — | Total power factor | 0 for EM07K |
| `freq` | number | Hz | Line frequency | Same as main |
| `energy_kwh` | number | kWh | Accumulated active energy | Device's internal counter |

**Notes:**
- Maximum 5 slaves (firmware limit for stack safety)
- Only **used slots** are published (no empty padding)
- If no slaves configured: `slaves` key is **not present** in JSON
- **Data authenticity:** giá trị đo chỉ có nghĩa khi `state="on"`. Khi `state="off"`
  (mất kết nối ≥ 5 lần poll liên tiếp) hoặc `"inactive"` (master không poll — bus/slot
  bị tắt hoặc portal config đang bật), **mọi giá trị đo về 0 (mặc định)** — thiết bị
  KHÔNG BAO GIỜ publish dữ liệu cũ (stale).

---

### 3.2. `pm/<id>/energy`

**QoS:** 1 (at-least-once delivery)  
**Retain:** No  
**Frequency:** Every publish interval

**Purpose:** Accumulated energy counters and demand measurements (main device only).

#### Payload Structure

```json
{
  "imp_kwh": 1234.56,
  "exp_kwh": 0.00,
  "imp_kvarh": 123.45,
  "exp_kvarh": 0.00,
  "dmd_w": 3450.5,
  "dmd_max_w": 5000.0
}
```

#### Field Details

| Field | Type | Unit | Description |
|-------|------|------|-------------|
| `imp_kwh` | number | kWh | Active energy import (consumed) |
| `exp_kwh` | number | kWh | Active energy export (generated) |
| `imp_kvarh` | number | kvarh | Reactive energy import |
| `exp_kvarh` | number | kvarh | Reactive energy export |
| `dmd_w` | number | W | Current demand (sliding window) |
| `dmd_max_w` | number | W | Maximum demand since reset |

**Notes:**

- **Nguồn số liệu — quan trọng:** thanh ghi tổng năng lượng của ATM90E32AS
  (`0x80 APenergyT`, `0x84 ANenergyT`, `0x88 RPenergyT`, `0x8C RNenergyT`) là
  **read-to-clear** (datasheet Table-11 §5.5.1, kiểu R/C): mỗi lần đọc trả về
  *phần tăng thêm kể từ lần đọc trước* rồi tự xoá về 0. IC **không lưu** tổng tích
  luỹ. Vì vậy **bộ đếm trong RAM của firmware chính là chỉ số công-tơ** — không có
  bản sao nào khác.
- **Bền vững qua reboot:** bộ đếm RAM được ghi xuống NVS theo ngưỡng năng lượng
  (`APP_ENERGY_PERSIST_THRESHOLD_WH`, mặc định 50 Wh) **hoặc** theo chu kỳ backstop
  (`APP_ENERGY_PERSIST_PERIOD_S`, mặc định 600 s), tuỳ điều kiện nào đến trước; ghi
  theo cơ chế **2 slot + CRC32** nên mất điện giữa chừng không hỏng dữ liệu. Mọi
  đường reboot có kiểm soát (console `reboot`, `cfg-reset`, web portal apply, Modbus
  HR_REBOOT) và lúc vào **AP config portal** đều commit ngay trước khi khởi động lại.
  Mất điện đột ngột có thể mất tối đa phần năng lượng kể từ lần ghi cuối.
- **Reset:** `imp_kwh`/`exp_kwh`/`imp_kvarh`/`exp_kvarh` chỉ về 0 khi người dùng chủ
  động chọn **LCD → Settings → Energy → Reset Energy**. **Factory Reset KHÔNG xoá
  energy** — chỉ số đo là kết quả đo, không phải một tuỳ chọn cấu hình.
- Demand values in **Watts** (not kW) for precision
- **Demand tính theo tích phân thời gian** (`Σ P·dt / Σ dt`), không phải trung bình
  cộng số mẫu — cửa sổ mặc định 15 phút, đổi được; `dmd_max_w` reset riêng bằng
  **LCD → Settings → Energy → Reset Demand**.
- Cùng bộ số liệu này được ghi ra thẻ SD dạng CSV — xem
  [energy_logging.md](energy_logging.md)

---

### 3.3. `pm/<id>/io`

**QoS:** 1 (reliable delivery)  
**Retain:** **Yes** (last state always available)  
**Frequency:** Every publish interval + **ngay khi input đổi mức** + immediate after relay command

**Purpose:** Digital I/O state snapshot. Retained so late subscribers see current state.

#### Payload Structure

```json
{
  "in0": false,
  "in1": true,
  "out0": true,
  "out1": false
}
```

#### Field Details

| Field | Type | Description |
|-------|------|-------------|
| `in0` | boolean | Digital input 0 (true = high/active) |
| `in1` | boolean | Digital input 1 (true = high/active) |
| `out0` | boolean | Relay output 0 (true = closed/energized) |
| `out1` | boolean | Relay output 1 (true = closed/energized) |

**Notes:**
- Also published in `telemetry.main` as `relay1/2` and `input1/2` (same values)
- Separate topic useful for subscribers only interested in I/O state
- Publishes immediately after relay command as echo confirmation
- **Event-driven inputs:** `in0`/`in1` đổi mức → publish ngay, **không chờ hết chu kỳ
  publish**. PCF8574 kéo chân INT khi input đổi, firmware đọc lại rồi đẩy bản tin
  ở nhịp 250 ms kế tiếp ⇒ **độ trễ tối đa ~250 ms** (thay vì tới 60 s theo chu kỳ).
  Xung ngắn hơn một nhịp đọc vẫn có thể bị bỏ sót — nếu cần đếm xung chính xác thì
  dùng cảnh báo mức, không dùng topic này làm bộ đếm.
- Chỉ **input** là event-driven; `out0`/`out1` đã có echo ngay sau lệnh relay từ trước.

---

### 3.4. `pm/<id>/heartbeat`

**QoS:** 0 (best-effort)  
**Retain:** No  
**Frequency:** Every publish interval

**Purpose:** System health, version info, and network status for monitoring.

#### Payload Structure

```json
{
  "uptime_s": 12345.67,
  "ts": 12345,
  "tq": "U",
  "boot": 17,
  "heap": 180000,
  "fw_version": "1.0.0",
  "active_broker": "Main-Broker",
  "iface": "wifi",
  "ip": "192.168.1.100"
}
```

#### Field Details

| Field | Type | Unit | Description |
|-------|------|------|-------------|
| `uptime_s` | number | seconds | Time since boot (from `esp_timer_get_time()`) |
| `ts` | number | seconds | Wall clock, Unix epoch (UTC) — **chỉ tin khi `tq` = `"S"` hoặc `"E"`** |
| `tq` | string | — | Time quality: `"S"` synced / `"E"` estimate / `"U"` uptime-only |
| `boot` | number | — | Boot counter, +1 mỗi lần khởi động (lưu trong NVS) |
| `heap` | number | bytes | Free heap memory (from `esp_get_free_heap_size()`) |
| `fw_version` | string | — | Firmware version (from `esp_app_desc`) |
| `active_broker` | string | — | Broker name from config (`cfg.mqtt.name`) |
| `iface` | string | — | Active network interface: `"eth"`, `"wifi"`, or `"none"` |
| `ip` | string | — | Current IP address |

#### Time quality (`tq`) — đọc trước khi dùng `ts`

Thiết bị **chưa gắn RTC (DS1307)**, nên `ts` hiện tại **không phải ngày giờ thật**.
`tq` cho biết chính xác mức độ tin cậy:

| `tq` | Tên | Ý nghĩa | Subscriber nên làm gì |
|------|-----|---------|-----------------------|
| `"U"` | Uptime-only | Chưa có nguồn thời gian nào. `ts` đếm từ epoch 1970-01-01 theo uptime | **Không** dùng `ts` làm mốc thời gian. Dùng `boot` + `uptime_s` để xác định thứ tự và phiên chạy; nếu cần dấu thời gian thật, lấy giờ nhận của server |
| `"E"` | Estimate | Đã khôi phục mốc thời gian lưu trong NVS (sàn thời gian), đang trôi vì không có RTC | Dùng được cho xếp thứ tự / gom nhóm; **không** dùng cho tính tiền điện hay đối soát chính xác |
| `"S"` | Synced | Đã đồng bộ từ RTC hoặc NTP | Tin được |

Đây là **chủ ý thiết kế**: thiết bị không bao giờ bịa ra một ngày tháng trông có vẻ
hợp lệ. Khi DS1307 được gắn, driver gọi `time_source_set()` một lần là `tq` chuyển
sang `"S"` và **mọi consumer** (`ts` ở MQTT, cột `timestamp`/`tq` trong CSV thẻ SD,
Modbus IR 110–113) đều đúng ngay, không phải sửa payload.

**Notes:**
- `fw_version` currently reads from compile-time `esp_app_desc`
- Future: may read from NVS `ota_version` field after OTA updates
- `heap` useful for memory leak detection
- `uptime_s` wraps after ~136 years (safe for practical use)
- `boot` là cách duy nhất phân biệt hai phiên chạy khác nhau khi `tq = "U"`

---

### 3.5. `pm/<id>/status`

**QoS:** 1 (reliable delivery)  
**Retain:** **Yes** (last-known presence)  
**Content-Type:** Plain string (NOT JSON)

**Purpose:** Device online/offline presence signaling with Last Will Testament (LWT).

#### Payload Values

- `"online"` — Published when device successfully connects to broker
- `"offline"` — Published by broker via LWT when device disconnects unexpectedly

**Notes:**
- LWT (Last Will Testament) configured at connection time
- Retained flag ensures subscribers always see latest presence
- No JSON wrapping — payload is literal string `online` or `offline`

---

## 4. Subscribed Topics (Broker → Device)

### 4.1. `pm/<id>/cmd/out0`

**QoS:** 1  
**Purpose:** Control relay output 0

#### Payload Format

Plain string (NOT JSON):
- `"on"` or `"1"` or `"true"` → Close relay (energize)
- `"off"` or `"0"` or `"false"` → Open relay (de-energize)

**Validation:**
- Case-insensitive matching
- Any other value → **ignored**, warning logged
- Invalid UTF-8 or non-string payload → **ignored**

**Response:**
- On success: relay state updated + `io` topic republished immediately
- No explicit ACK message (state confirmation via `io` topic)

---

### 4.2. `pm/<id>/cmd/out1`

**QoS:** 1  
**Purpose:** Control relay output 1

Same format and behavior as `cmd/out0`.

---

## 5. Field Reference Dictionary

### Quick Lookup: Field Name → Meaning

| Field/Key | Full Name | Unit | Where Found |
|-----------|-----------|------|-------------|
| `v` | Phase voltages | V | telemetry (main/slaves) |
| `i` | Phase currents | A | telemetry (main/slaves) |
| `pf` | Power factor | — | telemetry (main per-phase, slaves total) |
| `in` | Neutral current | A | telemetry (main only) |
| `p_kw` | Active power | kW | telemetry (main/slaves) |
| `p_kw_ph` | Active power per phase | kW | telemetry (main/slaves) |
| `q_kvar` | Reactive power | kvar | telemetry (main/slaves) |
| `s_kva` | Apparent power | kVA | telemetry (main/slaves) |
| `pf_total` | Total power factor | — | telemetry (main only) |
| `freq` | Line frequency | Hz | telemetry (main/slaves) |
| `temp` | Chip temperature | °C | telemetry (main only) |
| `energy_kwh` | Active energy import | kWh | telemetry (main/slaves), energy |
| `relay1` / `relay2` | Relay outputs | boolean | telemetry (main only) |
| `input1` / `input2` | Digital inputs | boolean | telemetry (main only) |
| `warnings` | Warning flags, gộp nhóm | bitmask 8-bit | telemetry (main only) |
| `warn_bits` | Warning flags, từng pha | bitmask 16-bit | telemetry (main only) |
| `in0` / `in1` | Digital inputs | boolean | io |
| `out0` / `out1` | Relay outputs | boolean | io |
| `imp_kwh` | Import active energy | kWh | energy |
| `exp_kwh` | Export active energy | kWh | energy |
| `imp_kvarh` | Import reactive energy | kvarh | energy |
| `exp_kvarh` | Export reactive energy | kvarh | energy |
| `dmd_w` | Current demand | W | energy |
| `dmd_max_w` | Peak demand | W | energy |
| `uptime_s` | Uptime | seconds | heartbeat |
| `ts` | Wall clock (Unix epoch) | seconds | heartbeat |
| `tq` | Time quality (`S`/`E`/`U`) | string | heartbeat |
| `boot` | Boot counter | — | heartbeat |
| `heap` | Free heap | bytes | heartbeat |
| `fw_version` | Firmware version | string | heartbeat |
| `active_broker` | Broker name | string | heartbeat |
| `iface` | Network interface | string | heartbeat |
| `ip` | IP address | string | heartbeat |
| `name` | Device name | string | slaves array |
| `id` | Modbus address | number | slaves array |
| `type` | Device model | string | slaves array |
| `state` | Device state (on/off/inactive) | string | slaves array |
| `online` | Comms status (true chỉ khi state="on") | boolean | slaves array |

---

## 6. Integration Examples

### 6.1. Subscribe to All Topics (mosquitto_sub)

```bash
# Subscribe to all topics for one device
mosquitto_sub -h broker.local -t "pm/Power_Meter/#" -v

# Subscribe to telemetry from all devices
mosquitto_sub -h broker.local -t "pm/+/telemetry" -v

# Subscribe with credentials (if broker requires auth)
mosquitto_sub -h broker.local -u username -P password -t "pm/Power_Meter/#" -v
```

### 6.2. Control Relay (mosquitto_pub)

```bash
# Turn on relay 0
mosquitto_pub -h broker.local -t "pm/Power_Meter/cmd/out0" -m "on"

# Turn off relay 1
mosquitto_pub -h broker.local -t "pm/Power_Meter/cmd/out1" -m "off"

# With authentication
mosquitto_pub -h broker.local -u user -P pass -t "pm/Power_Meter/cmd/out0" -m "1"
```

### 6.3. Python Example (paho-mqtt)

```python
import paho.mqtt.client as mqtt
import json

def on_connect(client, userdata, flags, rc):
    print(f"Connected with result code {rc}")
    # Subscribe to telemetry
    client.subscribe("pm/Power_Meter/telemetry")

def on_message(client, userdata, msg):
    payload = json.loads(msg.payload)
    main = payload["main"]
    
    # Extract main device measurements
    print(f"Power: {main['p_kw']:.2f} kW")
    print(f"Voltage L1: {main['v'][0]:.1f} V")
    print(f"Current L1: {main['i'][0]:.2f} A")
    
    # Process slave devices if present
    if "slaves" in payload:
        for slave in payload["slaves"]:
            print(f"Slave {slave['name']}: {slave['p_kw']:.2f} kW (online={slave['online']})")

client = mqtt.Client()
client.on_connect = on_connect
client.on_message = on_message

client.connect("broker.local", 1883, 60)
client.loop_forever()
```

### 6.4. Node-RED Flow Snippet

```json
[
  {
    "id": "mqtt_in",
    "type": "mqtt in",
    "topic": "pm/Power_Meter/telemetry",
    "qos": "0",
    "broker": "broker_config",
    "name": "Power Meter Telemetry"
  },
  {
    "id": "json_parse",
    "type": "json",
    "name": "Parse JSON"
  },
  {
    "id": "extract_power",
    "type": "function",
    "func": "msg.payload = msg.payload.main.p_kw;\nreturn msg;",
    "name": "Extract Power (kW)"
  }
]
```

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 2.5 | 2026-09-20 | • `heartbeat`: thêm `ts` (epoch), `tq` (time quality `U`/`E`/`S`), `boot` (boot counter)<br>• Đính chính §3.2: energy KHÔNG nằm trong thanh ghi IC (thanh ghi là read-to-clear) — bộ đếm RAM + NVS 2-slot/CRC32 mới là chỉ số công-tơ<br>• Ghi rõ điểm commit NVS, chính sách reset, và Factory Reset không xoá energy<br>• Demand tính theo tích phân thời gian |
| 2.4 | 2026-09-19 | • `io`: 2 digital input thành event-driven (đổi mức → publish ngay, trễ ≤ 250 ms)<br>• Thêm `warn_bits` (bitmap alarm từng pha, 16-bit) cạnh `warnings`<br>• Mô tả đầy đủ từng bit alarm: ý nghĩa, phục vụ cho gì, nguồn thanh ghi IC |
| 2.3 | 2026-09-19 | - Alarm backend (ATM90E32AS native warning) nối vào `warnings`: bitmask latched 7 bit, reset bằng LCD Reset Latch |
| 2.2 | 2026-09-19 | • Thêm `p_kw_ph` (P từng pha, kW, 2 số lẻ) cho main và slaves<br>• Noise floor cho slaves (|PF| < 0.1, \|P\|/\|Q\|/\|S\| < 1)<br>• Giá trị làm tròn về 0 luôn là +0, không bao giờ `-0`<br>• No-load gate theo dòng (50 mA): pha rỗi → I/P/Q/S/PF = 0 (chống crosstalk CT) |
| 2.1 | 2026-09-18 | • Slaves: thêm `state` (on/off/inactive) — phân biệt device off với master inactive<br>• `online` giờ true chỉ khi `state="on"`<br>• Data authenticity: khi state ≠ on, mọi giá trị đo về 0 (không publish stale data) |
| 2.0 | 2026-09-14 | • Power units changed to kW/kvar/kVA<br>• Multi-device support (main + slaves)<br>• IO fields in telemetry<br>• Relay cmd simplified to plain string |
| 1.0 | 2024-xx-xx | Initial version (single device, W/var/VA units) |

---

## Related Documentation

- **MQTT Configuration Guide:** [mqtt_guide.md](mqtt_guide.md)
- **Energy accumulation & SD logging:** [energy_logging.md](energy_logging.md)
- **Console Commands:** [console_commands.md](console_commands.md) (section: mqtt-cfg)
- **Architecture Overview:** [architecture.md](architecture.md)
- **Modbus Register Map:** [modbus_slave_register_map.md](modbus_slave_register_map.md)

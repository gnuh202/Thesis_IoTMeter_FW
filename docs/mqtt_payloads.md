# MQTT Payload Reference (single source of truth)

> Đây là **tài liệu DUY NHẤT** mô tả toàn bộ topic + payload MQTT của thiết bị.
> Các doc khác (thiết kế, kiến trúc) chỉ trỏ về đây, KHÔNG lặp lại bảng — để tránh
> tài liệu rải rác, lệch nhau. Khi đổi payload trong code, sửa ở đây.

Nguồn code: [main/app/mqtt_manager.c](../main/app/mqtt_manager.c).

---

## 1. Định danh & tiền tố topic

- **Tiền tố**: `pm/<device_name>/...`
  - `<device_name>` lấy từ `config_system_t.device_name` (đặt qua console/NVS), đã
    **sanitize** cho hợp lệ topic (khoảng trắng, `/`, `+`, `#` → `_`). Ví dụ tên
    `Power Meter` → topic `pm/Power_Meter/...`.
- **MQTT Client ID** (khác `<device_name>`): tự sinh = `<device_name>-<3 byte cuối MAC>`,
  ví dụ `Power_Meter-3AF2C1`, để duy nhất trên broker.

---

## 2. Bảng tổng quan

| Topic | Hướng | QoS | Retain | Chu kỳ |
|---|---|---|---|---|
| `pm/<id>/telemetry` | publish | 0 | no  | mỗi `publish_period_ms` (mặc định 5s) |
| `pm/<id>/energy`    | publish | 1 | no  | mỗi chu kỳ |
| `pm/<id>/io`        | publish | 1 | yes | mỗi chu kỳ + echo sau lệnh relay |
| `pm/<id>/heartbeat` | publish | 0 | no  | mỗi chu kỳ |
| `pm/<id>/status`    | publish | 1 | yes | khi connect (`online`) + LWT (`offline`) |
| `pm/<id>/cmd/out0`  | subscribe | 1 | — | khi có lệnh |
| `pm/<id>/cmd/out1`  | subscribe | 1 | — | khi có lệnh |

---

## 3. Publish — thiết bị → broker

### 3.1 `telemetry` — số đo tức thời (QoS0, no retain)

| Field | Kiểu | Nghĩa | Đơn vị |
|---|---|---|---|
| `v` | number[3] | điện áp pha A/B/C | V |
| `i` | number[3] | dòng pha A/B/C | A |
| `pf` | number[3] | hệ số công suất pha A/B/C | — |
| `in` | number | dòng trung tính | A |
| `p` | number | công suất tác dụng tổng | W |
| `q` | number | công suất phản kháng tổng | var |
| `s` | number | công suất biểu kiến tổng | VA |
| `pf_total` | number | hệ số công suất tổng | — |
| `freq` | number | tần số | Hz |
| `temp` | number | nhiệt độ chip đo | °C |

```json
{"v":[220.1,219.8,221.0],"i":[1.20,1.18,1.25],"pf":[0.98,0.97,0.99],
 "in":0.03,"p":790.5,"q":60.2,"s":792.8,"pf_total":0.98,"freq":50.0,"temp":41.5}
```

### 3.2 `energy` — năng lượng tích lũy + demand (QoS1)

| Field | Kiểu | Nghĩa | Đơn vị |
|---|---|---|---|
| `imp_kwh` | number | điện năng tác dụng nhập | kWh |
| `exp_kwh` | number | điện năng tác dụng xuất | kWh |
| `imp_kvarh` | number | điện năng phản kháng nhập | kvarh |
| `exp_kvarh` | number | điện năng phản kháng xuất | kvarh |
| `dmd_w` | number | demand công suất hiện tại | W |
| `dmd_max_w` | number | demand đỉnh | W |

```json
{"imp_kwh":12.345,"exp_kwh":0.0,"imp_kvarh":1.20,"exp_kvarh":0.0,"dmd_w":810.0,"dmd_max_w":1250.0}
```

### 3.3 `io` — trạng thái số (QoS1, **retained**)

| Field | Kiểu | Nghĩa |
|---|---|---|
| `in0` | bool | ngõ vào số 0 |
| `in1` | bool | ngõ vào số 1 |
| `out0` | bool | relay ngõ ra 0 (true = đóng/on) |
| `out1` | bool | relay ngõ ra 1 |

```json
{"in0":false,"in1":true,"out0":true,"out1":false}
```

Retained: client subscribe muộn vẫn thấy trạng thái cuối. Được publish lại ngay
sau mỗi lệnh relay (echo xác nhận trạng thái thật).

### 3.4 `heartbeat` — sống + giám sát (QoS0)

| Field | Kiểu | Nghĩa |
|---|---|---|
| `uptime_s` | number | thời gian chạy kể từ boot (giây) |
| `heap` | number | RAM (heap) còn trống (byte) |
| `fw_version` | string | phiên bản firmware (từ app descriptor) |
| `active_broker` | string | tên profile broker đang kết nối |
| `iface` | string | interface data-path active: `eth` / `wifi` / `none` |
| `ip` | string | địa chỉ IP hiện tại |

```json
{"uptime_s":3600,"heap":142000,"fw_version":"1.0.0","active_broker":"Mosquitto local","iface":"eth","ip":"192.168.137.61"}
```

### 3.5 `status` — hiện diện (QoS1, **retained**, KHÔNG phải JSON)

Chuỗi thuần:
- `online` — publish khi kết nối thành công.
- `offline` — **LWT** (Last Will), broker tự phát khi thiết bị mất kết nối đột ngột.

Cùng một topic retained, nên subscriber luôn thấy trạng thái hiện diện cuối cùng.

---

## 4. Subscribe — broker → thiết bị (điều khiển relay)

| Topic | Payload | Hành động |
|---|---|---|
| `pm/<id>/cmd/out0` | `{"state":"on"}` / `{"state":"off"}` | đặt relay out0 |
| `pm/<id>/cmd/out1` | `{"state":"on"}` / `{"state":"off"}` | đặt relay out1 |

- Payload là **JSON**, field `state` kiểu **chuỗi**, chỉ nhận `"on"` hoặc `"off"`.
- **Validate chặt**: payload không phải JSON / thiếu `state` / `state` không phải
  chuỗi / giá trị khác `on`|`off` → **bỏ qua, không đụng relay**, ghi log cảnh báo.
- Sau lệnh hợp lệ: đặt relay vật lý rồi **publish lại `io`** (echo trạng thái xác nhận).

Ví dụ (mosquitto_pub, lưu ý escape nháy tùy shell):
```bash
# Linux / macOS / Git Bash
mosquitto_pub -h <broker> -t "pm/Power_Meter/cmd/out0" -m '{"state":"on"}'

# Windows CMD (phải escape nháy kép)
mosquitto_pub -h <broker> -t "pm/Power_Meter/cmd/out0" -m "{\"state\":\"on\"}"
```

> Cảnh báo an toàn: điều khiển relay vật lý qua broker → broker phải cấu hình ACL
> để chỉ client được phép mới publish được vào `cmd/*`. Firmware chỉ đảm bảo phần
> của nó (TLS khi bật + auth + validate payload).

---

## 5. Ghi chú

- Chu kỳ publish (`publish_period_ms`) lưu trong NVS, đổi qua `mqtt-cfg period --period <ms>`
  (xem [console_commands.md](console_commands.md)); mặc định lần đầu 5000 ms.
- Toàn bộ JSON build bằng cJSON. Khi thêm/sửa field, cập nhật **doc này** đồng thời với code.

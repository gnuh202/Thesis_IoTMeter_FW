# Modbus TCP — bản đồ thanh ghi theo QĐ EVN (ĐMTMN)

Máy chủ Modbus TCP của thiết bị, triển khai đúng bộ tín hiệu trong Quyết định
EVN 12/2024 về kết nối hệ thống ĐMTMN tự sản xuất tự tiêu thụ với hệ thống thu
thập, giám sát, điều khiển của EVN.

Đọc cùng [modbus_slave_register_map.md](modbus_slave_register_map.md) — đó là
bản đồ RTU **riêng biệt**, hai bản đồ không dùng chung địa chỉ.

---

## 1. Kết nối

| Mục | Giá trị |
|---|---|
| Giao thức | Modbus TCP |
| Cổng | 502 (`CONFIG_APP_MB_TCP_PORT`) |
| Unit ID | 0x0A (`CONFIG_APP_MB_TCP_UNIT_ID`); chấp nhận thêm 0x00 và 0xFF |
| Số kết nối đồng thời | 4 |
| Timeout nhàn rỗi | 120 s |
| Hàm hỗ trợ | FC01, FC02, FC03, FC04, FC05, FC06 |
| Kiểu dữ liệu | float32, 2 thanh ghi, word cao trước (big-endian / ABCD) |
| Địa chỉ | Số in trong bảng là **1-based**; địa chỉ trên dây = số in − 1 |

Địa chỉ gốc đổi được bằng `CONFIG_APP_MB_TCP_ADDR_BASE` (1 hoặc 0) nếu đầu thu
của ĐVĐL công bố địa chỉ PDU thô.

Bind `INADDR_ANY` nên phục vụ được trên cả Ethernet lẫn WiFi và sống qua
failover. Máy chủ **không** dừng khi mở AP config portal.

## 2. Input Registers — FC04 (3xxxx)

| Địa chỉ tài liệu | Trên dây | Tín hiệu | Đơn vị | Bắt buộc |
|---|---|---|---|---|
| 1 | 0 | Công suất tác dụng phát lên lưới | kW | **Có** |
| 3 | 2 | Công suất phản kháng phát lên lưới | kVAr | Khuyến khích |
| 5 | 4 | Điện áp pha Ua | V | Khuyến khích |
| 7 | 6 | Điện áp pha Ub | V | Khuyến khích |
| 9 | 8 | Điện áp pha Uc | V | Khuyến khích |
| 11 | 10 | Dòng điện pha Ia | A | Khuyến khích |
| 13 | 12 | Dòng điện pha Ib | A | Khuyến khích |
| 15 | 14 | Dòng điện pha Ic | A | Khuyến khích |
| 17 | 16 | Tần số | Hz | Khuyến khích |
| 1109 | 1108 | Hệ số công suất | — | Khuyến khích |

**Quy ước dấu:** firmware đo theo chiều tiêu thụ dương. Hai thanh ghi công suất
ở đây đã đảo dấu sang chiều **phát lên lưới dương**.

- Công suất tác dụng: kẹp tại 0 khi đang tiêu thụ (đại lượng một chiều).
- Công suất phản kháng: giữ dấu (sớm pha / trễ pha đều có ý nghĩa thật).

**Lưu ý địa chỉ 1109:** hệ số công suất nằm tách hẳn khỏi cụm 1–17. Đây là con
số quyết định in ra và là con số đầu thu của ĐVĐL sẽ hỏi, nên firmware lấy
**đúng nguyên văn**: ảnh thanh ghi đầu vào được cấp đủ tới 1109, khoảng trống
19–1107 tồn tại nhưng đọc ra 0 (bản đồ thưa, giống mọi thiết bị khác).

Vì FC04 chỉ cho đọc tối đa 125 thanh ghi một lần, đầu thu phải đọc hệ số công
suất bằng một lệnh riêng (start 1108, qty 2) chứ không gộp chung với cụm đầu.

Ảnh thanh ghi được làm mới mỗi 500 ms.

## 3. Coils — FC01 / FC05 (0xxxx)

| Địa chỉ tài liệu | Trên dây | Tín hiệu | Bắt buộc |
|---|---|---|---|
| 11 | 10 | Cho phép điều khiển công suất tác dụng | **Có** |
| 15 | 14 | Cho phép điều khiển công suất phản kháng | Khuyến khích |

Chỉ hai coil này ghi được; ghi địa chỉ khác trả exception 02. Đọc FC01 toàn dải
0–14 không lỗi (các bit còn lại dành riêng, đọc ra 0).

Đặt coil về 0 **không** xoá setpoint đã lưu.

## 4. Holding Registers — FC03 / FC06 (4xxxx)

| Địa chỉ tài liệu | Trên dây | Tín hiệu | Bắt buộc |
|---|---|---|---|
| 13 | 12 | SetPoint công suất tác dụng theo % | **Có** |
| 17 | 16 | SetPoint công suất phản kháng theo % | Khuyến khích |

Mỗi setpoint là **một thanh ghi 16-bit**, đúng với hàm ghi mà quyết định chỉ
định (FC06 — Write Single Register).

Giá trị được lưu và trả về **nguyên trạng, không quy đổi, không kẹp biên**. Lý
do: quyết định không nói phần trăm là đơn vị nguyên hay phần mười, nên mọi diễn
giải tại đây chỉ có thể làm mất thông tin. Thang đo cần chốt với ĐVĐL.

## 5. Discrete Inputs — FC02 (1xxxx)

Quyết định liệt kê FC02 trong bộ hàm bắt buộc nhưng không định nghĩa tín hiệu
nhị phân nào. Vùng 0–15 tồn tại, đọc ra 0, dành riêng.

## 6. Lưu setpoint qua mất kết nối

Mục A.2 yêu cầu giữ nguyên lệnh đã nhận khi mất liên lạc. Cả hai coil và cả hai
setpoint được ghi **NVS** (namespace `mbtcp`) ngay khi giá trị thay đổi, và nạp
lại lúc khởi động. Mất mạng, rút dây hay reboot đều không mất lệnh cuối cùng.

## 7. Phạm vi hiện tại

Thiết bị là **đồng hồ đo**, không phải bộ điều khiển inverter. Giai đoạn này:

- Giám sát (mục 2): đầy đủ, số liệu thật từ ATM90E32AS.
- Điều khiển (mục 3, 4): **nhận — lưu — trả về đúng giá trị**. Firmware chưa tác
  động xuống inverter.

Điểm nối cho giai đoạn sau là `modbus_tcp_get_control()` trong
[modbus_tcp_task.h](../main/app/modbus_tcp_task.h) — trả về coil + setpoint mới
nhất để đẩy xuống inverter qua Modbus RTU master đã có.

## 8. Giả định khi triển khai — cần chốt với ĐVĐL

Quyết định mô tả kênh GSĐK là kênh **một chiều**: cả bốn chức năng ở mục A.2
đều nói "công suất ĐMTMN **phát lên HTĐ quốc gia**", và chiều tiêu thụ đi bằng
đường khác (mục B — công tơ đo đếm gửi kWh/kVArh lũy kế và biểu đồ 30 phút).
Phạm vi đó hợp lý, nhưng ba điểm dưới đây quyết định không nói rõ nên firmware
đang tự diễn giải. Ghi lại để khi đấu nối thật có chỗ đối chiếu.

**1. Điểm đo.** Quyết định không định nghĩa vị trí CT. Với hệ tự sản tự tiêu,
hai vị trí cho hai con số khác hẳn nhau:

| Vị trí CT | IR 1 đọc được |
|---|---|
| Điểm đấu nối | công suất **dư** thực sự đẩy lên lưới (PV − tải) |
| Đầu ra inverter | **toàn bộ** sản lượng PV, kể cả phần nhà tự dùng |

Con số điều độ cần là cái thứ nhất. Phép kẹp P tại 0 ở mục 2 **chỉ đúng khi CT
đặt tại điểm đấu nối** — đây là ràng buộc lắp đặt, không phải ràng buộc phần mềm.

**2. Kẹp P tại 0 làm mất khả năng phân biệt.** Khi IR 1 = 0, đầu thu không biết
là "PV vừa đủ, không dư" hay "trời tối, đang mua điện". Với mục đích giám sát
dòng ngược thì không sao, nhưng cách đọc còn lại (giữ dấu âm khi tiêu thụ) cũng
đứng vững về câu chữ. Bản đồ RTU và MQTT vẫn giữ giá trị có dấu nên nội bộ
không mất thông tin.

**3. Q mang nhãn một chiều nhưng bản chất hai chiều.** Tên tín hiệu là "công
suất phản kháng **phát lên lưới**", nhưng chính quyết định lại cho SetPoint
Q-out theo % — mà điều khiển Q nghĩa là bắt inverter khi phát khi hút var để
giữ điện áp. Kẹp Q như kẹp P sẽ làm chế độ hút var vô hình và lệnh điều khiển Q
của chính ĐVĐL không kiểm chứng được. Vì vậy firmware **giữ dấu cho Q** — cách
đọc duy nhất tự nhất quán, dù ngược với chữ "phát lên lưới" trong tên. Hệ số
công suất ở 1109 cũng không được quy định dấu.

## 9. Những yêu cầu không thuộc Modbus TCP

| Yêu cầu | Nằm ở đâu |
|---|---|
| Lưu 5 phút / ≥7 ngày + gửi bù | CSV thẻ SD + MQTT (xem [energy_logging.md](energy_logging.md)). Modbus TCP là poll-only, không có cơ chế gửi bù |
| Mã hoá kênh truyền | Modbus TCP không có TLS. Dùng VPN (OpenVPN/IPSec) ở lớp mạng, đúng như quyết định cho phép |
| Đường truyền ≥64 kbps, trễ ≤125 ms | Hạ tầng; W5500 100M đáp ứng |
| Mốc thời gian tin cậy cho dữ liệu 5 phút | **Chưa có** — phụ thuộc RTC DS1307 chưa gắn, `time_source` hiện báo cờ `U` |

## 10. Ghi chú triển khai

Máy chủ này **không** dùng component `esp-modbus`. Bản 1.0.18 giữ một instance
slave duy nhất toàn tiến trình (`freemodbus/common/esp_modbus_slave.c:37` và FSM
toàn cục ở `freemodbus/modbus/mb.c:69`), nên gọi `mbc_slave_init()` lần thứ hai
sẽ phá instance RS485 slave đang chạy. Bộ hàm mà quyết định yêu cầu đủ nhỏ để
tự phục vụ trên socket lwIP, và cách này giữ nguyên toàn bộ lớp RTU đã kiểm thử.

Mã nguồn: [modbus_tcp_task.c](../main/app/modbus_tcp_task.c).

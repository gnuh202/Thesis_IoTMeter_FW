# Thiết kế Web Configuration Portal (ESP32-S3, ESP-IDF)

> Trạng thái: **BẢN THIẾT KẾ để review**. Chưa code. `[QUYẾT ĐỊNH]` = đã chốt, `[TODO-SAU]` = pha sau.

## 1. Mục tiêu

Trang cấu hình chạy trên trình duyệt, thay cho console `net-cfg`/`mqtt-cfg` khi cần cấu hình tại hiện trường mà không cắm dây serial. Nhập được cả những thứ console khó nhập — đặc biệt **CA cert dài** (textarea thay cho một dòng console).

## 2. Quyết định đã chốt

- `[QUYẾT ĐỊNH]` **Chỉ phục vụ trên SoftAP** khi config portal đang bật (`net-cfg ap on` hoặc auto-AP recovery). KHÔNG mở trên ETH/STA (data-path). Tắt AP → tắt web server.
- `[QUYẾT ĐỊNH]` **Có auth** (user/pass) trước khi vào form.
- `[QUYẾT ĐỊNH]` **Lưu xong reboot để áp** (không live-apply). Giữ đúng mô hình hiện tại.
- `[QUYẾT ĐỊNH]` **Có captive portal** — tự bật trang khi nối AP.

## 3. Ràng buộc quan trọng (đọc trước khi code)

1. **Web server chỉ sống cùng AP.** Server khởi động khi AP bật, dừng khi AP tắt. Không có socket HTTP nào mở trên interface data-path (ETH/STA) → giảm bề mặt tấn công. Đây là hệ quả trực tiếp của "chỉ trên AP".

2. **Không phá lõi.** Web server chạy task riêng của httpd; đo lường / Modbus / MQTT không phụ thuộc nó. Portal chỉ đọc/ghi NVS qua `config_store`, không đụng runtime của các module khác.

3. **Reboot mới áp — nhất quán với console.** Form lưu vào NVS rồi hiện nút "Reboot to apply". Không đụng `network_manager_restart()` (live-apply là `[TODO-SAU]`). Lý do: live-apply cần xử lý đang-kết-nối phức tạp, chưa cần cho luận văn.

4. **Auth trên HTTP (không TLS) chỉ là rào cơ bản.** AP là mạng cục bộ, không TLS. Auth ngăn người vô tình, không chống được kẻ nghe lén trong tầm sóng AP. Chấp nhận được vì: (a) AP chỉ bật khi cần cấu hình, (b) AP có mật khẩu WPA2. Ghi rõ để không hiểu nhầm mức bảo mật.

5. **Không lưu secret trong HTML trả về.** Form hiển thị mật khẩu WiFi/MQTT dạng `(đã đặt)` hoặc trống, KHÔNG in lại giá trị thật. Nhập mới thì mới ghi đè.

## 4. Cấu hình đưa lên web

Tái dùng `config_store` (đã có sẵn, không thêm struct):

**Network** (`config_network_t`):
- WiFi STA SSID / password
- (hiển thị) mode, eth_dhcp — `[TODO-SAU]` cho sửa static IP

**MQTT** (`config_mqtt_t`, 3 profiles):
- Chọn profile active (0..2)
- Mỗi profile: name, uri, port, username, password, tls_enable, use_custom_ca, **ca_cert (textarea)** ← đây là thứ console không nhập được
- enabled, keepalive, publish_period_ms

**System** (`config_system_t`):
- device_name (ảnh hưởng topic MQTT + client_id)
- hostname

## 5. Kiến trúc

| Thành phần | Trách nhiệm |
|---|---|
| `web_portal` (module mới, `main/app/`) | httpd server + route + auth + render form + parse POST |
| (dùng lại) `config_store` | đọc/ghi NVS |
| (dùng lại) `wifi_manager` | AP đã bật sẵn (start_ap/stop_ap) |
| (dùng lại) `network_manager` | gọi start/stop portal; báo web_portal khi AP lên/xuống |

**Vòng đời**: `network_manager_start_config_portal()` bật AP → gọi `web_portal_start()`. `stop_config_portal()` → `web_portal_stop()`. Web server chỉ tồn tại trong khoảng AP bật.

## 6. Route dự kiến

| Route | Method | Chức năng |
|---|---|---|
| `/` | GET | form cấu hình (sau khi auth) |
| `/login` | GET/POST | trang đăng nhập, cấp session cookie |
| `/save/network` | POST | lưu network vào NVS |
| `/save/mqtt` | POST | lưu MQTT profile vào NVS |
| `/save/system` | POST | lưu system vào NVS |
| `/reboot` | POST | reboot áp cấu hình |
| `/generate_204`, `/hotspot-detect...` | GET | captive portal redirect về `/` |

- **Auth**: session cookie đơn giản (token ngẫu nhiên trong RAM, hết hạn khi reboot/tắt AP). User/pass **dùng chung console login** (`CONFIG_APP_CONSOLE_AUTH_USERNAME` / `CONFIG_APP_CONSOLE_AUTH_PASSWORD`).
- **Captive portal**: DNS server nội bộ (component `dns_server` của IDF, hoặc tự viết mini) trả mọi truy vấn về IP AP `192.168.4.1`; các URL dò-mạng của OS được redirect về `/`.

## 7. Việc triển khai (thứ tự, mỗi bước build/test riêng)

1. **web_portal khung + gắn vòng đời AP** — server rỗng (1 trang "OK") start/stop theo config portal. Test: `net-cfg ap on` → mở `192.168.4.1` thấy trang.
2. **Auth + session** — login gate. Test: chưa login → redirect `/login`.
3. **Form đọc cấu hình** — render network/mqtt/system từ NVS (ẩn secret). Test: giá trị hiện đúng.
4. **POST lưu cấu hình** — parse form, validate, ghi NVS. Test: lưu → `mqtt-cfg show` thấy đổi.
5. **Reboot áp** — nút reboot. Test: lưu + reboot → cấu hình mới có hiệu lực.
6. **Captive portal** — DNS + redirect. Test: nối AP → điện thoại tự mở trang.
7. `[TODO-SAU]`: static IP, live-apply, HTTPS.

## 8. Kconfig dự kiến

```
APP_WEB_PORTAL_ENABLE       (bool, default y)
APP_WEB_PORTAL_PORT         (int, default 80)
APP_WEB_PORTAL_SESSION_MAX  (int, default 1 — số session đồng thời)
```

## 9. Rủi ro / lưu ý

- **RAM**: httpd + buffer form (nhất là ca_cert 2KB) — cần buffer POST đủ lớn. httpd của IDF cho cấu hình `max_req_hdr_len` / stack. Theo dõi heap khi bật.
- **Trùng httpd cũ**: dashboard cũ (`wifi_meter_server`) đã bị xóa, nên không đụng. Đây là httpd đầu tiên sau khi bỏ dashboard.
- **AP tắt giữa chừng**: nếu auto-AP recovery tắt AP (grace timeout) trong lúc đang điền form → mất kết nối. Chấp nhận; portal là thao tác ngắn.

## 10. Điểm đã chốt

1. **User/pass đăng nhập web = dùng chung console login** (`CONFIG_APP_CONSOLE_AUTH_USERNAME` / `CONFIG_APP_CONSOLE_AUTH_PASSWORD`). Một bộ credential cho cả console lẫn web, không phân tán. ✓
2. **HTML thuần nhúng firmware, không CDN** (thiết bị offline trên AP). Làm giao diện gọn gàng, CSS tách thành phần rõ ràng, cấu trúc trang **chừa sẵn chỗ mở rộng** (thêm tab/trang sau này không phải viết lại). ✓
3. **DNS captive portal = component `espressif/dns_server`** (managed component có sẵn). ✓

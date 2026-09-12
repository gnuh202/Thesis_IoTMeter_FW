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

5. **Không lưu secret trong HTML trả về.** Ô mật khẩu WiFi/MQTT luôn rỗng, KHÔNG in lại giá trị
   thật. Việc "đã có mật khẩu hay chưa" nằm ở `placeholder` — chữ mờ trong ô rỗng, không phải
   `value`, nên để nguyên ô là giữ mật khẩu cũ. Nhập mới thì mới ghi đè.

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
| `/` | GET | trang cấu hình, 1 form duy nhất (sau khi auth) |
| `/login` | GET/POST | trang đăng nhập, cấp session cookie |
| `/save` | POST | lưu toàn bộ cấu hình text rồi khởi động lại để áp dụng |
| `/api/cert?slot=ca\|cert\|key&profile=0..2` | POST | upload PEM cho 1 slot của 1 profile MQTT; thêm `&api=1` để nhận lại dòng trạng thái (không redirect) |
| `/api/cert/delete?slot=..&profile=..` | POST | xoá file của slot đó (`&api=1` tương tự) |
| `/api/cert` | GET | trạng thái tất cả slot: có/không + size + fingerprint ngắn |
| `/reboot` | POST | khởi động lại (giữ lại cho curl/script) |
| `/generate_204`, `/hotspot-detect...` | GET | captive portal redirect về `/` |

**Toàn bộ chữ trên trang là tiếng Anh** (`<html lang="en">`, tab `PowerMeter Setting`, tiêu đề
`Power Meter Setting`). Tài liệu này viết tiếng Việt nhưng khi trích tên nút/mục thì dùng đúng
chuỗi tiếng Anh đang hiển thị.

Trang `/` chia làm 2 mục theo *nơi dùng*, không theo domain NVS:

- **Device & network** — tên thiết bị, SSID/mật khẩu WiFi.
- **MQTT servers** — máy chủ đang dùng, chu kỳ gửi, rồi 3 khối gập/mở "Server 1..3". Mỗi
  khối chứa *toàn bộ* thông tin của máy chủ đó: label, địa chỉ, cổng, tài khoản, thời
  gian giữ kết nối, mức bảo mật, **và 3 file chứng chỉ của chính nó**.

Trang chỉ hiển thị thứ người dùng **làm được gì với nó**. Chế độ mạng (`AUTO/ETH_ONLY/...`) và
"IP lấy tự động" đã bỏ: đọc xong cũng không sửa được ở đây, và là từ ngữ kỹ thuật nội bộ. Câu
hướng dẫn "sửa rồi bấm Save" ở đầu trang cũng bỏ: nút nằm ngay cuối trang, không cần dặn. Trạng
thái bật/tắt MQTT là *state* nên nằm ngay cạnh tiêu đề "MQTT servers" dạng tag có chấm màu
(`.tag.on` = `Enabled` / `.tag.off` = `Disabled`), không phải một câu văn bên dưới.

Toàn bộ input text thuộc **một** `<form id="cfg">` nằm ở mục "Save changes"; các input được gắn
vào form qua thuộc tính HTML5 `form="cfg"` nên vẫn hiển thị bên trong khối máy chủ mà không tạo
form lồng nhau (HTML không cho phép). Nhờ vậy mỗi slot chứng chỉ vẫn là một `multipart/form-data`
riêng, còn **một** nút duy nhất cuối trang (`Save and restart`) phụ trách mọi ô text. Chỉ một
nút vì hầu như không field nào áp được live — lưu mà không reboot chỉ trông như đã có hiệu lực.

Nút tải file/xoá file **không reload trang**: một script nhỏ nhúng cuối `<body>` chặn `submit` của
form có `data-cert` và `click` của nút có `data-del`, gửi bằng `fetch()` kèm `&api=1`. Handler thấy
`api` thì trả về đúng một dòng trạng thái của slot đó (có/không + size + fingerprint) thay vì
redirect; script ghi dòng đó vào `<p class="st">` của riêng slot. Vì không điều hướng, mọi ô text
người dùng vừa nhập mà chưa lưu đều còn nguyên. Thuộc tính `action`/`method` của form vẫn giữ nên
browser tắt script vẫn chạy được theo đường redirect cũ, và curl vẫn nhận một dòng text.

Nút **Delete file** chỉ hiện ở slot đang thực sự có file. Server phát nút kèm attribute `hidden` khi
slot rỗng (không bỏ hẳn khỏi HTML) vì upload không reload trang: nếu server là nơi duy nhất tạo
được nút thì slot vừa nhận file đầu tiên sẽ không có nút cho tới khi F5. Script bỏ `hidden` sau
khi upload thành công và bật lại sau khi xoá thành công.

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

## 11. Giao diện & responsive

**Ngôn ngữ thị giác GitHub Primer, viết tay.** Không link được CDN (Primer, Tabler, Bootstrap,
Tailwind hay Flowbite): trang được phục vụ từ SoftAP của chính thiết bị, không có đường ra
internet — link CDN sẽ ra trang trắng không style. Nhúng bundle đã build thì phải kéo toolchain
Node vào giữa build ESP-IDF cho đúng một trang HTML. Nên `HTML_STYLE` viết lại đúng token của
Primer: `#0969da` accent, `#1f883d` nút chính (hover `#1a7f37`, nhấn `#197935`), `#d1d9e0` viền,
`#1f2328` chữ, `#59636e` phụ, `#6e7781` mờ, bán kính **6px**, chữ 14px/1.5 system font stack,
focus ring 3px `#0969da4d`.

**Ba lớp nền, hơi tối hơn Primer gốc.** Nền trang là `#eaeef2` (canvas.inset) thay vì trắng, card
`#fff`, panel bên trong card `#f6f8fa` — ba lớp phân biệt được bằng mắt mà vẫn là theme sáng.

**Hiệu ứng tương tác** theo Primer: mọi thứ bấm được có `transition` 80ms
`cubic-bezier(.33,1,.68,1)` cho background/border/shadow, `:active` lún `translateY(1px)` kèm
inset shadow (để trên màn hình cảm ứng — nơi không có hover — cú tap vẫn có phản hồi), `summary`
và `.slot` đổi nền/viền khi hover, mũi tên accordion `rotate(90deg)` thay vì đổi ký tự. Toàn bộ
tắt dưới `@media(prefers-reduced-motion:reduce)`.

Hai điểm cần giữ khi sửa CSS: `[hidden]{display:none!important}` (nút `.btn` là `inline-flex` nên
sẽ thắng `display:none` mặc định của attribute `hidden` — nút "Delete file" ẩn/hiện dựa vào attribute
này), và `.input{font-size:16px}` dưới 640px (Safari iOS tự zoom trang khi focus vào control nhỏ
hơn 16px), chỉ từ 640px mới hạ về 14px.

**Mobile-first, một cột là mặc định.** `.row` là grid `1fr`; chỉ từ `min-width:640px` mới thành
`1fr 1fr`. Điện thoại (mọi hãng) luôn nhận layout một cột — không phụ thuộc `auto-fit`/`minmax`.

**Nguyên nhân lỗi lệch cấu trúc trên PC (đã sửa).** Trước đây `send_input()` phát `<label>` và
`<input>` thành hai phần tử *ngang cấp* trong grid. Ở màn hình rộng, grid `auto-fit` mở ra 3–4 cột
nên mỗi label và mỗi input chiếm một ô riêng → label nằm trên input của field khác. Sửa bằng cách
bọc mỗi cặp label+control vào một `<div class="field">` (một ô grid), và số cột được cố định là
1 hoặc 2 thay vì `auto-fit`. Field có nhãn dài (địa chỉ máy chủ, bảo mật kết nối) dùng
`.field.wide` để span cả 2 cột.

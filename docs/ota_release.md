# OTA qua GitHub Releases: hướng dẫn đầy đủ

> **Single Source of Truth** cho việc cập nhật firmware từ xa.
> Source: [main/app/ota_manager.c](../main/app/ota_manager.c),
> [main/app/ota_manager.h](../main/app/ota_manager.h),
> [.github/workflows/release.yml](../.github/workflows/release.yml),
> [tools/make_manifest.py](../tools/make_manifest.py)

Tài liệu này đi từ đầu đến cuối: nguyên tắc thiết kế, cách đẩy code lên git, cách
tag và cắt release, CI sinh ra cái gì, thiết bị tải về và cài như thế nào, và
cách khôi phục khi hỏng.

---

## 1. Ba nguyên tắc

Toàn bộ module được dựng trên đúng ba nguyên tắc. Đọc kỹ phần này thì các phần
sau chỉ là thủ tục.

**1.1. App descriptor là nguồn version DUY NHẤT.**
`esp_app_get_description()->version` lấy từ ảnh firmware đang chạy. Không có bản
sao nào trong NVS. Lý do: một bản sao có thể lệch với ảnh đang thực thi, và một
version biết nói dối thì tệ hơn là không có version. Khi tag `v1.2.0`, ESP-IDF
chạy `git describe --tags --dirty` và ghi đúng chuỗi đó vào descriptor — MQTT,
LCD và Modbus đọc chung từ một chỗ nên không thể lệch nhau.

**1.2. Manifest chỉ nói "có release mới", KHÔNG quyết định cài gì.**
Manifest nằm trên mạng nên nó có thể khai bất kỳ version nào. Trước khi commit,
firmware đọc descriptor nhúng bên trong chính ảnh vừa tải (`esp_https_ota_get_img_desc`)
và so với ảnh đang chạy. Descriptor đi *bên trong* thứ được cài; manifest thì không.

**1.3. Ảnh mới phải tự chứng minh là chạy được.**
Với `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, bootloader đánh dấu ảnh mới là
`PENDING_VERIFY`. Nếu firmware không tự commit thì lần reset kế tiếp bootloader
quay về slot cũ. Điều kiện commit (xem `selftest_tick()`):

- uptime ≥ `CONFIG_APP_OTA_SELFTEST_DELAY_S` (mặc định 60 s), **và**
- `system_status_get(SYS_MODULE_ATM90) == SYS_STATUS_READY` — con chip đo, thứ
  duy nhất khiến sản phẩm này tồn tại, đang trả lời.

**Mạng KHÔNG nằm trong điều kiện này.** Nếu bắt buộc phải có link thì một sợi cáp
bị rút sẽ không phân biệt được với một bản build hỏng, và mỗi lần switch của trạm
chết là một ảnh hoàn toàn tốt bị rollback.

---

## 2. Cấu hình build (đã set sẵn)

| Mục | Giá trị | Ở đâu |
|---|---|---|
| Partition dual-OTA | `ota_0` 4 MB @0x20000, `ota_1` 4 MB @0x420000, `otadata` @0x19000 | [partitions.csv](../partitions.csv) |
| Rollback bootloader | `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` | `sdkconfig`, `sdkconfig.defaults` |
| CA bundle cho HTTPS | `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` + `DEFAULT_FULL` | `sdkconfig.defaults` |
| Bật/tắt OTA | `CONFIG_APP_OTA_ENABLE` (mặc định `y`) | `main/Kconfig.projbuild` |
| URL manifest | `CONFIG_APP_OTA_MANIFEST_URL` | `main/Kconfig.projbuild` |
| Timeout HTTP | `CONFIG_APP_OTA_HTTP_TIMEOUT_MS` = 20000 | như trên |
| Stack / priority worker | 8192 / **4** | như trên |
| Cửa sổ self-test | `CONFIG_APP_OTA_SELFTEST_DELAY_S` = 60 | như trên |

Ảnh hiện tại ~1,55 MB trên slot 4 MB → còn dư 62 %. Không cần đổi partition.

> **Vì sao priority 4?** Thấp hơn cả tầng comm (7) lẫn tầng internal (6). Một lần
> tải firmware không bao giờ được phép làm đói task đo, Modbus hay MQTT. Tải chậm
> hơn vài giây không ai chết; bỏ lỡ một chu kỳ đo thì có.

`CONFIG_APP_OTA_MANIFEST_URL` mặc định:

```
https://github.com/gnuh202/Thesis_IoTMeter_FW/releases/latest/download/manifest.json
```

Dùng `/releases/latest/download/` chứ **không** dùng `api.github.com`: GitHub
phân giải "latest" ngay trên server, không tốn quota. `api.github.com` giới hạn
60 request/giờ/IP — cả một fleet sau cùng một NAT sẽ ăn hết quota đó rất nhanh.

---

## 3. Đánh version

Quy ước: **semantic versioning**, tag có tiền tố `v`.

```
v<major>.<minor>.<patch>        ví dụ v1.0.0, v1.2.3
```

- **major** — đổi phá vỡ tương thích (map thanh ghi Modbus đổi, topic MQTT đổi).
- **minor** — thêm tính năng, vẫn tương thích ngược.
- **patch** — sửa lỗi.

So sánh chỉ xét `major.minor.patch` (`ota_manager_version_compare`). Hệ quả cần biết:

- Build giữa hai tag được `git describe` đặt tên `v1.2.0-5-gabc1234`; phần đuôi
  bị bỏ qua nên nó được coi **bằng** `v1.2.0` và sẽ không tự đề nghị cập nhật
  chính bản thân nó.
- Chuỗi không parse được (ví dụ `7ae0cca-dirty`, tức build chưa có tag) được xếp
  **thấp hơn mọi thứ**, nên máy đang chạy bản dev luôn thấy release mới nhất là
  bản nâng cấp. Đây đúng là hành vi mong muốn khi dev trỏ máy về production.

Kiểm tra version của build hiện tại:

```powershell
idf.py build
# hoặc đọc thẳng từ máy đang chạy, qua console:
#   ota status
```

---

## 4. Đẩy code lên GitHub

Repo `main` là nhánh công khai. Mọi việc mới đi theo **nhánh → PR trên web → merge**.

### 4.1. Làm việc trên nhánh

```bash
# đang ở nhánh tính năng, ví dụ fix/fixed-pga-4x
git status                      # xem những file đã đổi
git add main/ components/ docs/ sdkconfig sdkconfig.defaults
git commit -m "Add over-the-air firmware update from GitHub releases"
git push -u origin fix/fixed-pga-4x
```

### 4.2. Mở Pull Request

Máy này **không có `gh` CLI**, nên mở PR bằng trình duyệt:

1. Vào `https://github.com/gnuh202/Thesis_IoTMeter_FW`.
2. GitHub hiện banner *"Compare & pull request"* cho nhánh vừa push → bấm vào.
   Nếu không thấy: tab **Pull requests** → **New pull request** → base `main`,
   compare nhánh của bạn.
3. Viết tiêu đề và mô tả → **Create pull request**.
4. Xem lại diff trong tab **Files changed**.
5. **Merge pull request** → **Confirm merge**.

### 4.3. Cập nhật máy local sau khi merge

```bash
git checkout main
git pull origin main
```

> **Quan trọng:** chỉ tag trên `main` sau khi đã merge. Tag trên nhánh tính năng
> sẽ tạo ra một release được build từ commit không nằm trong lịch sử `main`.

---

## 5. Cắt một release

### 5.1. Tạo tag có chú thích

Dòng đầu (subject) của tag chính là release note mà CI sẽ đưa vào manifest và
hiển thị lên LCD, nên viết ngắn — thiết bị chỉ giữ **47 ký tự đầu**.

```bash
git checkout main
git pull origin main

git tag -a v1.0.0 -m "First production release"
git push origin v1.0.0
```

Tag sai thì xoá cả hai phía rồi tạo lại. Nếu release đã publish thì đẩy lại tag
cùng tên sẽ khiến CI **cập nhật release hiện có** — thay đủ 2 asset cùng tên,
nhưng **không đổi body** (hành vi của `softprops/action-gh-release`): body phải
sửa tay trên web, hoặc xoá release trước khi re-cut. Học từ lần re-cut `v1.0.0`.

```bash
git tag -d v1.0.0
git push origin :refs/tags/v1.0.0
```

### 5.2. CI chạy những gì

Đẩy tag `v*` sẽ kích hoạt [.github/workflows/release.yml](../.github/workflows/release.yml):

1. `actions/checkout@v4` với **`fetch-depth: 0`** — bắt buộc, clone nông không có
   tag thì `git describe` rơi về commit hash.
2. `git fetch --force origin refs/tags/<tag>:refs/tags/<tag>` — checkout trên
   tag-push event dựng ref tag trỏ thẳng vào **commit**, tag object (chứa
   message) không được kéo theo: `git describe` vẫn đúng nhưng release note rơi
   về subject của commit. Học từ chính lần phát hành `v1.0.0`.
3. `espressif/esp-idf-ci-action@v1` build với ESP-IDF v5.5.1, target esp32s3.
   Lệnh chạy có `git config --global --add safe.directory "$PWD"` vì container
   chạy bằng root, thiếu dòng này thì `git describe` im lặng thất bại.
4. Lấy release note từ subject của tag.
5. `tools/make_manifest.py` sinh `manifest.json`.
6. `softprops/action-gh-release@v2` tạo release và upload hai asset.

### 5.3. Chốt chặn của make_manifest.py

Script đọc `esp_app_desc_t` ngay trong file `.bin` (offset `0x20`, magic
`0xABCD5432`, trường `version` ở byte 16 của struct) và **từ chối publish** nếu:

- version trong ảnh không parse được thành `major.minor[.patch]` → gần như chắc
  chắn là tag chưa được fetch;
- version trong ảnh khác với tag.

Đây không phải kiểm tra thừa. Một release mà asset ghi `v1.1.0` còn manifest ghi
`v1.2.0` sẽ khiến cả fleet tin là có bản cập nhật đang chờ và **không bao giờ**
áp dụng được, vì ảnh chúng tải về liên tục thật ra không hề mới hơn.

Chạy thử tại chỗ trước khi tin vào CI:

```powershell
python tools/make_manifest.py `
  --bin build/luanvan_firmware.bin `
  --tag v1.0.0 `
  --repo gnuh202/Thesis_IoTMeter_FW `
  --notes "First production release" `
  --out manifest.json
```

### 5.4. Asset của release

| File | Vai trò |
|---|---|
| `luanvan_firmware.bin` | ảnh firmware, thiết bị tải trực tiếp |
| `manifest.json` | thứ thiết bị đọc trước để biết có gì mới |

`manifest.json`:

```json
{
  "version": "v1.0.0",
  "url": "https://github.com/gnuh202/Thesis_IoTMeter_FW/releases/download/v1.0.0/luanvan_firmware.bin",
  "size": 1614656,
  "sha256": "5ce667a0...",
  "notes": "First production release",
  "project": "luanvan_firmware",
  "idf_version": "v5.5.1"
}
```

Firmware chỉ đọc `version`, `url`, `notes`. `size`/`sha256`/`project`/`idf_version`
dành cho con người và cho công cụ khác; tính toàn vẹn của ảnh đã được ESP-IDF
kiểm bằng checksum + SHA-256 nhúng sẵn trong image trước khi flip boot partition.

### 5.5. Kiểm tra release đã lên đúng

```bash
curl -L https://github.com/gnuh202/Thesis_IoTMeter_FW/releases/latest/download/manifest.json
```

Phải trả về đúng JSON ở trên. Nếu ra HTML thì release chưa publish hoặc asset
chưa upload xong.

---

## 6. Cập nhật từ phía thiết bị

Có bốn đường vào, tất cả đều gọi chung một worker task và **đều tự reboot** khi
cài xong. Một hành vi duy nhất cho mọi cách kích hoạt: bản cập nhật qua MQTT
không có ai đứng đó bấm nút, còn một ảnh đã ghi mà chưa chạy là trạng thái duy
nhất mà thiết bị tự mâu thuẫn với chính nó về version nó đang chạy.

### 6.1. Màn hình LCD — `Settings → FW Update`

1. Vào `Settings → FW Update`, màn hình hiện `Checking...`.
2. Kết quả:
   - `1.2.0 is available` + dòng ghi chú → `OK: Install`, `<: Back`
   - `No fw available` → đang là bản mới nhất, `OK: Back`
   - lỗi (`No network`, `Bad manifest`, …) → `OK: Back`
3. Bấm OK → màn hình xác nhận `INSTALL?` → `<Confirm>` / `<Cancel>`.
4. Thanh tiến trình chạy tới 100 %, hiện `Installed` → `Rebooting...`.

### 6.2. Console (USB-Serial-JTAG)

```
ota check              # tải manifest, không chặn console
ota status             # version đang chạy, trạng thái, %, lỗi, release mới nhất
ota update             # cài bản từ lần check gần nhất
ota update --url https://.../luanvan_firmware.bin
ota confirm            # commit ngay, bỏ qua cửa sổ self-test
ota rollback           # quay về slot cũ và reboot
```

`ota status` in cả dòng `probation`, cho biết ảnh đang chạy đã commit hay chưa.

### 6.3. Web portal — mục **Firmware**

`GET /api/ota` trả về text từng dòng `key value`; `POST /api/ota` nhận
`action=check` hoặc `action=update` (kèm `url=` tuỳ chọn).

```bash
curl http://192.168.4.1/api/ota
curl -X POST http://192.168.4.1/api/ota -d "action=check"
curl -X POST http://192.168.4.1/api/ota -d "action=update"
```

Trang web tự poll `/api/ota` (1 s khi đang chạy, 10 s khi rảnh) nên một bản cập
nhật khởi động từ LCD hay MQTT cũng hiện lên ở đây.

### 6.4. MQTT

Subscribe lệnh trên `pm/<device>/cmd/ota`:

```json
{"action":"check"}
{"action":"update"}
{"action":"update","url":"https://.../luanvan_firmware.bin"}
```

Publish trạng thái trên `pm/<device>/ota` (QoS1, retained):

```json
{"state":"downloading","running":"v1.0.0","percent":45}
{"state":"check_done","running":"v1.0.0","latest":"v1.1.0","available":true,"notes":"..."}
{"state":"failed","running":"v1.0.0","error":"Transfer failed"}
```

Trạng thái được publish **theo thay đổi**, không theo đồng hồ, và tiến trình được
làm tròn 5 % — một lần tải ~1,5 MB chỉ tốn khoảng 20 message thay vì một message
mỗi 250 ms.

`state` nhận một trong: `idle`, `checking`, `check_done`, `downloading`,
`reboot_pending`, `failed`.

Version đang chạy cũng nằm trong heartbeat (`fw_version`) như trước.

### 6.5. Modbus

Thanh ghi input **IR 101** (`IR_FW_VERSION`) nay là `major<<8 | minor` lấy từ
descriptor, không còn hardcode `0x0100`. Build chưa có tag đọc ra `0x0000`
("không rõ") thay vì một con số bịa ra.

---

## 7. Điều gì xảy ra sau khi cài

```
tải xong → esp_https_ota_finish() kiểm ảnh, lật boot partition
         → hiện "Installed", chờ 3 s
         → esp_restart()
         → bootloader chạy slot mới, đánh dấu PENDING_VERIFY
         → ota_manager_init() thấy PENDING_VERIFY, bật timer self-test 5 s/lần
         → sau >= 60 s uptime VÀ ATM90 READY → esp_ota_mark_app_valid_cancel_rollback()
         → log "self-test passed; image committed, rollback cancelled"
```

Nếu ảnh mới **không** commit được (crash trước 60 s, hoặc chip đo không bao giờ
sẵn sàng), lần reset kế tiếp bootloader tự quay về ảnh cũ. Không cần ai can thiệp.

Trường hợp biên đã xử lý: nếu `esp_timer_create()` thất bại thì firmware commit
**ngay lập tức**, vì không có timer thì ảnh không bao giờ commit được và sẽ bị
rollback ở lần reset sau — thà commit sớm còn hơn bỏ rơi một thiết bị đang chạy tốt.

---

## 8. Khôi phục sự cố

| Triệu chứng | Nguyên nhân | Xử lý |
|---|---|---|
| LCD `No network` | chưa có IP | cắm Ethernet hoặc kiểm tra WiFi STA |
| LCD `Bad server reply` / `Download failed` | URL sai, release chưa publish, DNS chết | `curl` thử URL manifest từ PC cùng mạng |
| log `HTTP_CLIENT: Out of buffer` | GitHub trả 302 sang CDN; theo redirect nghĩa là phải **gửi** `GET <path ký ~1,4 KB> HTTP/1.1`, mà dòng request được dựng trong buffer **TX** mặc định 512 B | phải set **cả hai**: `buffer_size` *và* `buffer_size_tx` = 4096. `buffer_size` chỉ map sang `buffer_size_rx` — đó là lý do lần sửa đầu (chỉ `buffer_size`) không hết lỗi. Đã sửa sau v1.0.1; các bản ≤ v1.0.1 phải flash lại qua USB |
| LCD `Bad manifest` | JSON thiếu `version` hoặc `url` | chạy lại `tools/make_manifest.py` |
| LCD `Same version` | ảnh tải về không mới hơn ảnh đang chạy | tag lại cho đúng; đây là chốt chặn ở nguyên tắc 1.2 đang làm việc |
| LCD `Image invalid` | ảnh hỏng hoặc không phải app image hợp lệ | build lại, kiểm `sha256` trong manifest |
| Máy tự quay về bản cũ sau OTA | self-test không đạt | xem log: ATM90 có READY không, có crash trước 60 s không |
| Đã commit nhầm một bản xấu | — | `ota rollback` qua console |
| `ota rollback` báo lỗi | slot kia chưa có ảnh hợp lệ (máy mới flash lần đầu) | flash lại bằng USB |

Cứu hộ cuối cùng luôn là flash qua USB:

```powershell
idf.py -p COMx flash
```

---

## 9. Danh sách kiểm tra khi phát hành

```
[ ] Đã merge vào main và git pull
[ ] idf.py build sạch, không warning mới
[ ] Test trên bàn: LCD FW Update, ota status, /api/ota, MQTT cmd/ota
[ ] git tag -a vX.Y.Z -m "Ghi chú ngắn (<= 47 ký tự)"
[ ] git push origin vX.Y.Z
[ ] CI xanh, release xuất hiện với đủ 2 asset
[ ] curl -L .../releases/latest/download/manifest.json trả đúng version
[ ] Một thiết bị thật: check → install → reboot → chạy được
[ ] Sau ~60 s: log "self-test passed; image committed"
[ ] ota status báo probation: no
```

---

## 10. Kịch bản test phần cứng

Đánh số tiếp theo [energy_logging.md §10](energy_logging.md).

| # | Kịch bản | Kỳ vọng |
|---|---|---|
| 26 | `ota check` khi rút cáp mạng | `state failed`, `error No network` |
| 27 | `ota check` khi mạng tốt, chưa có release | `Bad server reply` (chưa publish) |
| 28 | `ota check` sau khi publish v1.0.1 (máy chạy v1.0.0) | `latest v1.0.1`, `available 1` |
| 29 | LCD `Settings → FW Update` ở trạng thái #28 | `1.0.1 is available` |
| 30 | LCD FW Update khi đã là bản mới nhất | `No fw available` |
| 31 | `ota update` đủ chu trình | % tăng, reboot, `ota status` hiện v1.0.1 |
| 32 | Ngay sau OTA, trước 60 s | `probation: yes (not committed)` |
| 33 | Sau 60 s với ATM90 READY | log commit, `probation: no` |
| 34 | OTA rồi rút nguồn trước 60 s | bật lại → chạy lại bản **cũ** |
| 35 | `ota update` khi ảnh trên server cùng version | `Same version`, không ghi flash |
| 36 | Rút cáp giữa lúc tải | `Transfer failed`, máy vẫn chạy bản cũ |
| 37 | MQTT publish `{"action":"check"}` lên `cmd/ota` | `pm/<id>/ota` báo `checking` rồi `check_done` |
| 38 | Web portal mục Firmware, bấm Check | status đổi, nút Install hiện khi có bản mới |
| 39 | Đọc Modbus IR 101 sau khi chạy v1.2.0 | `0x0102` |
| 40 | `ota rollback` trên máy đã OTA ít nhất một lần | reboot về slot cũ |

---

## 11. Những gì cố tình KHÔNG làm

- **Không mirror version vào NVS** — nguyên tắc 1.1.
- **Không tự động cập nhật theo lịch.** Mọi lần cập nhật đều do người hoặc hệ
  thống giám sát chủ động kích hoạt. Một thiết bị đo điện tự reboot lúc nửa đêm
  vì có release mới là hành vi không ai muốn.
- **Không yêu cầu mạng trong self-test** — nguyên tắc 1.3.
- **Không xoá hai trường NVS chết `ota_version` / `ota_fw_build`.** Bỏ chúng đi
  sẽ đổi `dto->total_size`, mà `config_manager.c` validate giá trị này — hệ quả
  là reset toàn bộ cấu hình đã lưu của mọi thiết bị đang chạy. Layout giữ nguyên,
  chỉ là màn hình Device Info thôi không đọc chúng nữa mà đọc thẳng descriptor.
- **Không có anti-rollback (`CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK`).** Nó khoá
  vĩnh viễn không cho hạ version; với một đồ án còn đang lặp nhanh thì cái giá
  khi lỡ tay lớn hơn nhiều so với lợi ích bảo mật.

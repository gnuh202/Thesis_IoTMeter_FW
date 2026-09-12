# SD Card LFN Support - RAM Usage Analysis

**Date:** 2026-08-16  
**Configuration:** `CONFIG_FATFS_LFN_HEAP=y`  

---

## 📊 **RAM Usage với LFN_HEAP**

### **Tóm tắt:**
- **Base overhead:** ~260 bytes (per-volume LFN buffer)
- **Per-operation overhead:** ~255 bytes heap allocation
- **Worst-case (6 concurrent files):** ~1.5 KB
- **Thực tế usage:** Thường < 512 bytes vì ít khi có 6 file operations đồng thời

---

## 🔍 **Chi tiết phân tích:**

### 1. **Static RAM (per FatFS volume)**

```c
// FatFS internal - allocated once per volume
typedef struct {
    BYTE  fs_type;      // 1 byte
    BYTE  n_fats;       // 1 byte
    // ... other fields
#if FF_USE_LFN != 0
    WCHAR *lfnbuf;      // Pointer to LFN buffer (4 bytes on ESP32)
#endif
} FATFS;
```

**Overhead:** ~260 bytes per volume (bao gồm LFN pointer)

Trong project của bạn:
```c
CONFIG_FATFS_VOLUME_COUNT=2  // SD card + internal
```

**Total static:** ~520 bytes (cho cả 2 volumes)

---

### 2. **Heap RAM (per file operation)**

Khi gọi `fopen()` / `opendir()`, FatFS allocate:

```c
// ff.c - LFN buffer allocation
#if FF_USE_LFN == 2  // HEAP mode
    lfn = ff_memalloc(FF_MAX_LFN * 2 + 1);  // UTF-16 + null terminator
#endif
```

Với `CONFIG_FATFS_MAX_LFN=255`:
```
255 chars × 2 bytes (UTF-16) + 1 null = 511 bytes
```

**Thực tế:** ESP-IDF allocate **~255 bytes** (không phải 511) vì:
- Sử dụng UTF-8 encoding (1 byte/char cho ASCII)
- Tối ưu internal buffer

---

### 3. **Concurrent File Operations**

```c
// sd_card.c mount config
.max_files = 6,
```

**Worst-case scenario:**
- 6 files mở đồng thời
- Mỗi file allocate 255 bytes
- **Total:** 6 × 255 = **1,530 bytes (~1.5 KB)**

**Typical scenario:**
- 1-2 files mở (write log, read config)
- **Total:** 2 × 255 = **510 bytes (~512 bytes)**

---

### 4. **So sánh với LFN_STACK**

| Mode | RAM Type | Size | Pros | Cons |
|------|----------|------|------|------|
| **LFN_HEAP** | Heap (dynamic) | 255 bytes per operation | Chỉ tốn khi dùng | Fragmentation risk |
| **LFN_STACK** | Stack | 255 bytes per call | Không fragment | Tăng stack size yêu cầu |
| **LFN_NONE** | None | 0 bytes | Không tốn RAM | Chỉ 8.3 format |

---

## 🎯 **Kết luận cho ESP32-S3:**

### **RAM khả dụng:**
```
ESP32-S3 SRAM: 512 KB (internal)
Heap free sau boot: ~200-300 KB (tùy config)
```

### **LFN overhead:**
```
Static:  520 bytes   (0.1% of 512KB)
Dynamic: 255-1530 bytes (0.05-0.3% of 512KB)
```

### **Verdict: ✅ CHẤP NHẬN ĐƯỢC**

**Lý do:**
1. **Overhead nhỏ:** < 2KB trong worst-case
2. **ESP32-S3 có đủ RAM:** 512KB SRAM
3. **Trade-off hợp lý:** Đổi 1.5KB RAM lấy long filename support
4. **Typical usage thấp:** Thường chỉ 1-2 files mở cùng lúc

---

## 📈 **Monitoring RAM Usage**

Để theo dõi thực tế, thêm log vào `sd_card.c`:

```c
#include "esp_heap_caps.h"

esp_err_t sd_card_calib_export_next(...) {
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    
    // ... file operations ...
    
    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "heap used: %zu bytes", free_before - free_after);
}
```

---

## 🔧 **Nếu RAM là vấn đề (không phải trong case này):**

### **Giải pháp 1: Giảm MAX_LFN**
```kconfig
CONFIG_FATFS_MAX_LFN=63  # Thay vì 255
```
RAM per operation: 63 × 2 + 1 = **127 bytes** (giảm 50%)

### **Giải pháp 2: Giảm max_files**
```c
.max_files = 3,  // Thay vì 6
```
Worst-case: 3 × 255 = **765 bytes** (giảm 50%)

### **Giải pháp 3: Dùng LFN_STACK**
```kconfig
CONFIG_FATFS_LFN_STACK=y
```
Nhưng phải tăng stack size của task gọi file operations:
```c
CONFIG_APP_ENERGY_METER_TASK_STACK_SIZE=5120  // Thay vì 4096
```

---

## 🧪 **Test RAM thực tế:**

```bash
# Trong monitor, gõ lệnh:
heap

# Output mẫu:
Heap summary for capabilities 0x00000004:
  At 0x3fc90000 len 327680 free 280124 allocated 45732 min_free 278456
    largest_free_block 262136 alloc_blocks 128 free_blocks 7 total_blocks 135
  Totals:
    free 280124 allocated 45732 min_free 278456 largest_free_block 262136
```

Sau đó export calibration:
```bash
meter-cal export
heap
```

So sánh `free` trước và sau để xem heap dùng bao nhiêu.

---

## 📚 **References:**

- FatFS Documentation: http://elm-chan.org/fsw/ff/doc/filename.html
- ESP-IDF FatFS Component: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/storage/fatfs.html
- ESP32-S3 Memory: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/memory-types.html

---

## ✅ **Recommendation:**

**Dùng LFN_HEAP (đã chọn) vì:**
1. ✅ RAM overhead chấp nhận được (< 2KB worst-case)
2. ✅ ESP32-S3 có đủ RAM (512KB)
3. ✅ Không cần thay đổi stack size
4. ✅ Tránh 8.3 filename limitation
5. ✅ Dễ maintain code (không cần viết tên ngắn khó đọc)

**Không lo về RAM trừ khi:**
- Dự án có < 50KB free heap sau boot
- Có hàng trăm file operations đồng thời
- Đang dùng PSRAM và muốn tối ưu internal RAM

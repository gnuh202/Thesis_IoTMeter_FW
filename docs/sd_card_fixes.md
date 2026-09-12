# SD Card Implementation Fixes

**Date:** 2026-08-16  
**Branch:** fix/config-manager-freeze  

---

## Issues Fixed

### 1. **Critical: FatFS Long Filename Support Disabled**

**Problem:**
- `CONFIG_FATFS_LFN_NONE=y` restricted filenames to 8.3 format only
- Attempting to create `calib_001.dat` (10+3 chars) failed with `errno=22` (EINVAL)
- Test files like `test.txt` (4+3) worked, but calibration files failed

**Root Cause:**
```c
// sd_card.c:279 - Filename violates 8.3 format
snprintf(filename, sizeof(filename), "calib_%03d.dat", next_num);
//       └─────┬────────┘  └┬┘
//            10 chars      3 chars  → FAIL (basename > 8)
```

**Fix:**
- Enabled `CONFIG_FATFS_LFN_HEAP=y` in sdkconfig
- Added `CONFIG_FATFS_MAX_LFN=255` for full filename support
- Added `CONFIG_APP_SD_ENABLE_LFN=y` in Application Config menu
- Updated mount config to use both FATs for reliability when LFN is enabled

**Trade-off:**
- Uses ~512 bytes heap per concurrent file operation
- Acceptable for this application (max 6 concurrent files)

---

### 2. **Critical: Extension Mismatch (.dat vs .bin)**

**Problem:**
```c
// Line 279: Export creates .dat files
snprintf(filename, sizeof(filename), "calib_%03d.dat", next_num);

// Line 368: Import scans for .bin files
if (sscanf(entry->d_name, "calib_%d.bin", &num) == 1) {
```

**Result:**
- Export created `calib_001.dat`, `calib_002.dat`, ...
- Import could never find any files
- Each export always created `calib_001.dat` (max_num always = 0)

**Fix:**
- Changed export to use `.bin` extension consistently
- Both export and import now use `calib_%03d.bin` pattern

---

### 3. **High: No Error Checking on fprintf()**

**Problem:**
```c
// Old append_line() - always returned ESP_OK
fprintf(f, "%s\n", line);
fclose(f);
return ESP_OK;  // ← No check!
```

**Risk:**
- SD card full → write fails silently → log appears complete but is truncated
- Corrupt file not detected

**Fix:**
```c
int result = fprintf(f, "%s\n", line);
esp_err_t ret = ESP_FAIL;

if (result > 0) {
    if (fflush(f) == 0) {
        ret = ESP_OK;
    } else {
        ESP_LOGW(TAG, "flush %s failed (errno=%d)", path, errno);
    }
} else {
    ESP_LOGW(TAG, "fprintf %s failed (errno=%d)", path, errno);
}
```

---

### 4. **High: No fsync() After Critical Writes**

**Problem:**
- After `fwrite()` + `fclose()`, data may still be in cache
- Power loss → calibration file lost or corrupt

**Fix:**
```c
size_t written = fwrite(data, 1, len, f);
if (written == len) {
    /* Flush and sync to ensure data is written to SD card */
    if (fflush(f) == 0 && fsync(fileno(f)) == 0) {
        ret = ESP_OK;
        // ...
    } else {
        ESP_LOGE(TAG, "flush/sync %s failed (errno=%d): %s", 
                 full_path, errno, strerror(errno));
    }
}
```

**Note:** `fsync()` requires `#include <unistd.h>` or equivalent (already via `<sys/stat.h>`)

---

### 5. **Medium: Debug Test Files in Production Code**

**Problem:**
- Lines 293-313 created `/sdcard/test.txt` and `/sdcard/CALIB/test.txt` on every export
- Wasted storage and cluttered SD card

**Fix:**
- Removed debug test file creation code completely
- Clean production-ready export function

---

### 6. **Low: Improved Error Messages**

**Before:**
```c
ESP_LOGE(TAG, "write %s failed (errno=%d)", full_path, errno);
```

**After:**
```c
ESP_LOGE(TAG, "write %s failed (expected=%zu, wrote=%zu, errno=%d): %s",
         full_path, len, written, errno, strerror(errno));
```

**Benefits:**
- Shows partial write size (helps diagnose disk-full vs I/O error)
- Includes `strerror()` for human-readable error description

---

## Configuration Changes

### sdkconfig
```diff
-CONFIG_FATFS_LFN_NONE=y
-# CONFIG_FATFS_LFN_HEAP is not set
+# CONFIG_FATFS_LFN_NONE is not set
+CONFIG_FATFS_LFN_HEAP=y
 # CONFIG_FATFS_LFN_STACK is not set
+CONFIG_FATFS_MAX_LFN=255

 CONFIG_APP_SD_CS_GPIO=8
 CONFIG_APP_SD_DET_GPIO=15
+CONFIG_APP_SD_ENABLE_LFN=y
```

### main/Kconfig.projbuild
Added new configuration option:
```kconfig
config APP_SD_ENABLE_LFN
    bool "Enable long filename support for SD card"
    default y
    help
        Enable FatFS long filename (LFN) support for SD card operations.
        When enabled, allows filenames longer than 8.3 format (e.g.,
        "calib_001.dat" instead of just "cal_001.bin"). Uses ~512 bytes
        of heap per concurrent file operation.
```

---

## Testing Checklist

- [ ] Mount SD card → directories created successfully
- [ ] Export calibration → `calib_001.bin` created
- [ ] Export again → `calib_002.bin` created (sequential numbering works)
- [ ] Import calibration → file found and loaded
- [ ] Remove SD during write → error logged, no crash
- [ ] SD card full → write fails with clear error message
- [ ] Log events → `EVENTS.LOG` written correctly
- [ ] Log energy → `ENERGY.CSV` written correctly
- [ ] Reboot after export → file still readable (fsync verified)

---

## Remaining Known Issues (Not Fixed)

### Race Condition on Mount Completion
```c
// sd_monitor_task() line 229
xSemaphoreGive(s_lock);
if (ret == ESP_OK) {
    sd_card_log_event("boot: card mounted");  // ← Not holding lock!
}
```

**Risk:** Card removed between `xSemaphoreGive()` and `sd_card_log_event()` → crash

**Recommended Fix:**
```c
if (ret == ESP_OK) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_mounted) {
        append_line(SD_EVENTS_FILE, "boot: card mounted");
    }
    xSemaphoreGive(s_lock);
}
```

---

## Files Modified

1. `components/sd_card/sd_card.c`
   - Fixed extension mismatch (.dat → .bin)
   - Removed debug test file code
   - Added `fprintf()` error checking
   - Added `fsync()` after calibration writes
   - Improved error messages

2. `main/Kconfig.projbuild`
   - Added `CONFIG_APP_SD_ENABLE_LFN` option

3. `sdkconfig`
   - Enabled `CONFIG_FATFS_LFN_HEAP=y`
   - Added `CONFIG_FATFS_MAX_LFN=255`
   - Added `CONFIG_APP_SD_ENABLE_LFN=y`

---

## Build Instructions

```bash
# In Windows environment with ESP-IDF configured
cd d:\WorkSpace\Luanvan\2. FIRMWARE\uart_echo
idf.py build
idf.py flash monitor
```

Or use VS Code ESP-IDF extension build/flash commands.

---

## References

- ESP-IDF FatFS Component: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/storage/fatfs.html
- FatFS Long Filename: http://elm-chan.org/fsw/ff/doc/filename.html
- Issue Log: [errno=22 Invalid argument on fopen()]

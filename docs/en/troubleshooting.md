# Troubleshooting

## 1. Log Viewing

### Serial Monitor

```bash
idf.py -p COM3 monitor
```

Exit: `Ctrl+]`

### Key Log Tags

| Tag | Module |
|-----|--------|
| `main` | Main entry, health monitoring |
| `camera` | Camera driver |
| `recorder` | Video recorder |
| `mjpeg` | MJPEG streaming service |
| `web` | Web server |
| `wifi` | WiFi management |
| `storage` | SD card storage |
| `uploader` | NAS upload |
| `status_led` | LED status |
| `time_sync` | Time synchronization |

### Health Monitor Output

Automatically outputs every 60 seconds in the following format:

```
I (60000) main: HEALTH: heap=45230 PSRAM=3145728 rec_hwm=1234 nas_hwm=2345
```

| Field | Meaning |
|-------|---------|
| `heap` | Internal SRAM free bytes |
| `PSRAM` | External PSRAM free bytes |
| `rec_hwm` | Recording task stack high water mark (minimum remaining) |
| `nas_hwm` | Upload task stack high water mark |

---

## 2. Key Thresholds

| Indicator | Threshold | Level | Log |
|-----------|-----------|-------|-----|
| Internal heap < 20 KB | 20000 bytes | `CRITICAL` | `E (xxx) main: CRITICAL: Free heap below 20KB` |
| PSRAM < 500 KB | 500000 bytes | `WARNING` | `W (xxx) main: WARNING: Free PSRAM below 500KB` |
| SD card free < 20% | 20% | `WARNING` | `W (xxx) storage: Storage low (xx.x%), starting cleanup` |
| Watchdog timeout | 30 seconds | `PANIC` | System auto restart |
| Consecutive upload failures | 10 times | `PAUSE` | Upload pauses for 5 minutes |

---

## 3. Common Issues

### WiFi Connection Failure

**Symptom**: LED keeps fast blinking, cannot connect to WiFi.

**Troubleshooting**:

1. Confirm SSID and password are correct (note case and spaces)
2. Confirm router is 2.4 GHz (ESP32-S3 does not support 5 GHz)
3. Check if router has MAC address filtering enabled
4. Check serial log output with `wifi` tag
5. Try moving device closer to router

**Solution**: Enter AP mode to reconfigure WiFi, or override via SD card `wifi.txt`.

---

### WiFi Authentication Repeated Failures (auth expired / assoc expired)

**Symptom**: Serial log continuously outputs `state: auth -> init (0x200)` or `state: assoc -> init (0x400)`, device can scan AP but cannot connect.

**Root Cause**: XIAO ESP32-S3 development board WiFi transmit power is low by default (Seeed official known issue, batches before August 2025), causing authentication frames to fail to reach the router properly.

**Log Signature**:
```
I (xxx) wifi:state: init -> auth (0xb0)
I (xxx) wifi:state: auth -> init (0x200)        ← Auth timeout
I (xxx) wifi:state: init -> auth (0xb0)
I (xxx) wifi:state: auth -> assoc (0x0)
I (xxx) wifi:state: assoc -> init (0x400)       ← Assoc timeout
```

**Fix**: Call in `wifi_start_sta()` after `esp_wifi_start()`:
```c
int8_t power_param = (int8_t)(15 / 0.25);  // 15 dBm
esp_wifi_set_max_tx_power(power_param);
```
Increase transmit power from default to 15 dBm (Seeed official recommended value). Reference: https://wiki.seeedstudio.com/cn/xiao_esp32s3_wifi_usage/

---

### Device Infinite Restart Loop (crash loop)

**Symptom**: Device restarts immediately after power on, infinite loop. Serial log crashes at same position every time.

**Log Signature**:
```
ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE)
file: "./main/wifi_manager.c" line 199
expression: esp_event_loop_create_default()
abort() was called at PC 0x4037df0f on core 0
Rebooting...
```

**Root Cause**: In `wifi_init()`, when `esp_event_loop_create_default()` is called, the default event loop has already been created by a previous WiFi scan, returning `ESP_ERR_INVALID_STATE`. `ESP_ERROR_CHECK` treats this as a fatal error and triggers abort.

**Fix**: Instead of using `ESP_ERROR_CHECK`, use fault-tolerant handling:
```c
esp_err_t ev_err = esp_event_loop_create_default();
if (ev_err != ESP_OK && ev_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ev_err));
    return ev_err;
}
```
---

### TF Card Not Recognized

**Symptom**: Serial log shows `Failed to mount SD card`, recording does not start.

**Troubleshooting**:

1. Confirm TF card is properly inserted in slot
2. Confirm TF card is FAT32 format (not exFAT or NTFS)
3. Try replacing with Class 10 or higher speed card
4. Check if card reads/writes normally on computer
5. Try reformatting (FAT32, allocation unit 64 KB)

**Solution**: Replace TF card or reformat. Supports hot-plug, automatically detects and recovers within 10 seconds after insertion.

---



### SD Card Initialization Fails with SPI High-Speed Mode



**Symptom**: SD card works fine with previous firmware but fails to initialize after upgrade. Serial log shows repeated failures:



```

E (xxx) sdmmc_sd: sdmmc_enable_hs_mode_and_check: send_csd returned 0x108

E (xxx) vfs_fat_sdmmc: sdmmc_card_init failed (0x108).

E (xxx) vfs_fat_sdmmc: esp_vfs_fat_sdspi_sdcard_init failed (0x108).

W (xxx) storage: SD card init attempt N/5 failed: ESP_ERR_INVALID_RESPONSE (0x108)

```



Device runs normally except SD card is unavailable (no recording, `/api/status` shows `sd_free_percent: 0` and abnormally large `sd_total_bytes`).



**Root Cause**: The firmware uses SPI mode (1-bit) for SD card communication. Setting the SPI clock to `SDMMC_FREQ_HIGHSPEED` (40MHz) causes the CMD6 high-speed mode switching command to fail in SPI mode. Not all SD cards support high-speed SPI negotiation.



**Log Signature**:

```

I (xxx) storage: Initializing SD card (SPI mode): CS=21 SCK=7 MOSI=9 MISO=8

I (xxx) sdspi_transaction: cmd=5, R1 response: command not supported

E (xxx) sdmmc_sd: sdmmc_enable_hs_mode_and_check: send_csd returned 0x108

```



**Fix**: In `storage_manager.c`, ensure the SPI clock frequency is set to `SDMMC_FREQ_DEFAULT` (20MHz), not `SDMMC_FREQ_HIGHSPEED` (40MHz). The 20MHz default provides reliable SPI operation with download speeds of approximately 300-600 KB/s.



```c

// Correct for SPI mode

sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();

// Use SDMMC_FREQ_DEFAULT (20MHz), NOT SDMMC_FREQ_HIGHSPEED (40MHz)

// High-speed mode CMD6 is not reliable in SPI mode

```



**Prevention**: When modifying SD card parameters, always test with the actual hardware. SPI mode has different speed capabilities than SDMMC mode (4-bit).



---

---

### Recording Not Starting

**Symptom**: LED is off but `/api/status` shows `recording: false`.

**Troubleshooting**:

1. Confirm SD card is mounted: `sd_available: true` in `/api/status`
2. Check if SD card free space is sufficient
3. Check serial log for errors with `recorder` tag
4. Confirm camera initialized successfully

**Solution**: Manually start recording via API: `POST /api/record?action=start`.

---

### Web Page Cannot Access

**Symptom**: Browser cannot open device IP.

**Troubleshooting**:

1. Confirm device and browser are on the same LAN
2. Check if device IP is correct (serial log or router DHCP list)
3. Confirm firewall is not blocking access
4. Try using `curl` to test: `curl http://<deviceIP>/api/status`
5. Check if serial log shows `Web server started` with `web` tag

**Solution**: If web service failed to start, restart device.

---

### NAS Upload Failure

**Symptom**: Recording completed but files not uploaded to NAS.

**Troubleshooting**:

1. Confirm NAS server address, port, path configuration is correct
2. Confirm username and password are correct
3. Confirm FTP/WebDAV service is started
4. Confirm device and NAS are on the same network
5. Check serial log for error messages with `uploader` tag
6. Check if consecutive failure count reached limit (pauses 5 minutes after 10 failures)

**Solution**: Reconfigure NAS parameters via API, or check NAS server-side logs.

---

### Video Stream Stuttering or Not Playing

**Symptom**: `/stream` page freezes or cannot load.

**Troubleshooting**:

1. Confirm concurrent clients ≤ 2 (returns 503 if exceeded)
2. Lower resolution (VGA is smoother than SVGA/XGA)
3. Lower JPEG quality (increase value)
4. Lower frame rate
5. Check WiFi signal strength (RSSI value in `/api/status`)

**Solution**: Lower resolution or frame rate in configuration page to reduce network bandwidth requirements.

---

### Device Frequent Restarts

**Symptom**: Device automatically restarts after running for a period.

**Troubleshooting**:

1. Check serial log for crash dump or panic information
2. Check if heap memory is below 20 KB (health monitor output)
3. Check if watchdog triggered timeout (no喂狗 for 30 seconds)
4. Confirm power supply is sufficient (USB 3.0 or 5V/1A+ adapter)
5. Check if PSRAM is below 500 KB

**Solution**: If due to insufficient memory, lower resolution or frame rate. If due to power issues, change power supply method.

---

## 4. Recovery Methods

### Method 1: BOOT Button Recovery

1. While device is powered on
2. Hold BOOT button (GPIO0) for 5 seconds
3. LED becomes double blink (error state)
4. Device automatically restarts after 2 seconds
5. After restart, enters AP mode (all configuration cleared)

### Method 2: API Factory Reset

```bash
curl -X POST http://<deviceIP>/api/reset \
  -H "X-Password: admin"
```

### Method 3: Erase Flash

Completely erase Flash via ESP-IDF tools (including NVS configuration):

```bash
idf.py -p COM3 erase-flash
```

After erasing, need to re-flash firmware:

```bash
idf.py -p COM3 flash monitor
```

> **Note**: Factory reset will clear all configuration (WiFi, NAS, password, etc.), restore to default value `admin`, device enters AP mode after restart. Recording files on TF card are not affected.
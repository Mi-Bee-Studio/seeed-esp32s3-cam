# 故障排除

## 1. 日志查看

### 串口监控

```bash
idf.py -p COM3 monitor
```

退出：`Ctrl+]`

### 关键日志标签

| 标签 | 模块 |
|------|------|
| `main` | 主入口、健康监控 |
| `camera` | 摄像头驱动 |
| `recorder` | 视频录像器 |
| `mjpeg` | MJPEG 流服务 |
| `web` | Web 服务器 |
| `wifi` | WiFi 管理 |
| `storage` | SD 卡存储 |
| `uploader` | NAS 上传 |
| `status_led` | LED 状态 |
| `time_sync` | 时间同步 |

### 健康监控输出

每 60 秒自动输出，格式如下：

```
I (60000) main: HEALTH: heap=45230 PSRAM=3145728 rec_hwm=1234 nas_hwm=2345
```

| 字段 | 含义 |
|------|------|
| `heap` | 内部 SRAM 空闲字节数 |
| `PSRAM` | 外部 PSRAM 空闲字节数 |
| `rec_hwm` | 录像任务栈高水位（剩余最小值） |
| `nas_hwm` | 上传任务栈高水位 |

---

## 2. 关键阈值

| 指标 | 阈值 | 级别 | 日志 |
|------|------|------|------|
| 内部堆 < 20 KB | 20000 bytes | `CRITICAL` | `E (xxx) main: CRITICAL: Free heap below 20KB` |
| PSRAM < 500 KB | 500000 bytes | `WARNING` | `W (xxx) main: WARNING: Free PSRAM below 500KB` |
| SD 卡剩余 < 20% | 20% | `WARNING` | `W (xxx) storage: Storage low (xx.x%), starting cleanup` |
| 看门狗超时 | 30 秒 | `PANIC` | 系统自动重启 |
| 连续上传失败 | 10 次 | `PAUSE` | 上传暂停 5 分钟 |

---

## 3. 常见问题

### WiFi 连接失败

**现象**：LED 一直快闪，无法连接 WiFi。

**排查**：

1. 确认 SSID 和密码正确（注意大小写和空格）
2. 确认路由器为 2.4 GHz（ESP32-S3 不支持 5 GHz）
3. 检查路由器是否开启了 MAC 地址过滤
4. 查看串口日志中 `wifi` 标签的输出
5. 尝试将设备靠近路由器

**解决**：进入 AP 模式重新配置 WiFi，或通过 SD 卡 `wifi.txt` 覆盖。

---

### WiFi 认证反复失败（auth expired / assoc expired）

**现象**：串口日志持续输出 `state: auth -> init (0x200)` 或 `state: assoc -> init (0x400)`，设备能扫描到 AP 但无法连接。

**根因**：XIAO ESP32-S3 开发板 WiFi 发射功率默认偏低（Seeed 官方已知问题，2025年8月前批次），导致认证帧无法正常到达路由器。

**日志特征**：
```
I (xxx) wifi:state: init -> auth (0xb0)
I (xxx) wifi:state: auth -> init (0x200)        ← 认证超时
I (xxx) wifi:state: init -> auth (0xb0)
I (xxx) wifi:state: auth -> assoc (0x0)
I (xxx) wifi:state: assoc -> init (0x400)       ← 关联超时
```

**修复**：在 `wifi_start_sta()` 中 `esp_wifi_start()` 之后调用：
```c
int8_t power_param = (int8_t)(15 / 0.25);  // 15 dBm
esp_wifi_set_max_tx_power(power_param);
```
将发射功率从默认值提升到 15 dBm（Seeed 官方推荐值）。参考：https://wiki.seeedstudio.com/cn/xiao_esp32s3_wifi_usage/

---

### 设备无限重启循环（crash loop）

**现象**：设备上电后立即重启，无限循环。串口日志每次启动都在同一位置 crash。

**日志特征**：
```
ESP_ERROR_CHECK failed: esp_err_t 0x103 (ESP_ERR_INVALID_STATE)
file: "./main/wifi_manager.c" line 199
expression: esp_event_loop_create_default()
abort() was called at PC 0x4037df0f on core 0
Rebooting...
```

**根因**：`wifi_init()` 中 `esp_event_loop_create_default()` 被调用时，默认事件循环已被之前的 WiFi 扫描创建过，返回 `ESP_ERR_INVALID_STATE`，`ESP_ERROR_CHECK` 将其视为致命错误触发 abort。

**修复**：不使用 `ESP_ERROR_CHECK`，改为容错处理：
```c
esp_err_t ev_err = esp_event_loop_create_default();
if (ev_err != ESP_OK && ev_err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ev_err));
    return ev_err;
}
```
---

### TF 卡无法识别

**现象**：串口日志显示 `Failed to mount SD card`，录像不启动。

**排查**：

1. 确认 TF 卡已正确插入卡槽
2. 确认 TF 卡为 FAT32 格式（非 exFAT 或 NTFS）
3. 尝试更换 Class 10 或更高速度的卡
4. 在电脑上检查卡是否正常读写
5. 尝试重新格式化（FAT32，分配单元 64 KB）

**解决**：更换 TF 卡或重新格式化。支持热插拔，插入后 10 秒内自动检测并恢复。

---

### SD 卡 SPI 高速模式初始化失败

**现象**：固件升级后 SD 卡无法初始化，串口日志显示重复失败：





E (xxx) sdmmc_sd: sdmmc_enable_hs_mode_and_check: send_csd returned 0x108

E (xxx) vfs_fat_sdmmc: sdmmc_card_init failed (0x108).

E (xxx) vfs_fat_sdmmc: esp_vfs_fat_sdspi_sdcard_init failed (0x108).

W (xxx) storage: SD card init attempt N/5 failed: ESP_ERR_INVALID_RESPONSE (0x108)





设备正常运行但 SD 卡不可用（无录像，`/api/status` 显示 `sd_free_percent: 0` 且异常大的 `sd_total_bytes`）。



**根因**：固件使用 SPI 模式（1 位）进行 SD 卡通信。将 SPI 时钟设置为 `SDMMC_FREQ_HIGHSPEED`（40MHz）时，SPI 模式下的 CMD6 高速模式切换命令会失败。并非所有 SD 卡都支持高速 SPI 协商。







I (xxx) storage: Initializing SD card (SPI mode): CS=21 SCK=7 MOSI=9 MISO=8

I (xxx) sdspi_transaction: cmd=5, R1 response: command not supported

E (xxx) sdmmc_sd: sdmmc_enable_hs_mode_and_check: send_csd returned 0x108










// Correct for SPI mode

sdspi_device_config_t dev_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();

// Use SDMMC_FREQ_DEFAULT (20MHz), NOT SDMMC_FREQ_HIGHSPEED (40MHz)

// High-speed mode CMD6 is not reliable in SPI mode





**预防**：修改 SD 卡参数时，务必在实际硬件上测试。SPI 模式的速度能力与 SDMMC 模式（4 位）不同。



---

---


---

### 录像未启动

**现象**：LED 熄灭但 `/api/status` 显示 `recording: false`。

**排查**：

1. 确认 SD 卡已挂载：`/api/status` 中 `sd_available: true`
2. 检查 SD 卡剩余空间是否充足
3. 查看串口日志中 `recorder` 标签是否有错误
4. 确认摄像头初始化成功

**解决**：手动通过 API 启动录像：`POST /api/record?action=start`。

---

### Web 页面无法访问

**现象**：浏览器无法打开设备 IP。

**排查**：

1. 确认设备与浏览器在同一局域网
2. 检查设备 IP 是否正确（串口日志或路由器 DHCP 列表）
3. 确认防火墙未阻止访问
4. 尝试使用 `curl` 测试：`curl http://<设备IP>/api/status`
5. 检查串口日志中 `web` 标签是否显示 `Web server started`

**解决**：如果 Web 服务启动失败，重启设备。

---

### NAS 上传失败

**现象**：录像完成但文件未上传到 NAS。

**排查**：

1. 确认 NAS 服务器地址、端口、路径配置正确
2. 确认用户名和密码正确
3. 确认 FTP/WebDAV 服务已启动
4. 确认设备与 NAS 在同一网络
5. 查看串口日志中 `uploader` 标签的错误信息
6. 检查连续失败计数是否达到上限（10 次后暂停 5 分钟）

**解决**：通过 API 重新配置 NAS 参数，或检查 NAS 服务器端日志。

---

### 视频流卡顿或无法播放

**现象**：`/stream` 页面画面卡住或无法加载。

**排查**：

1. 确认并发客户端数 ≤ 2（超过返回 503）
2. 降低分辨率（VGA 比 SVGA/XGA 流畅）
3. 降低 JPEG 质量（数值增大）
4. 降低帧率
5. 检查 WiFi 信号强度（`/api/status` 中 RSSI 值）

**解决**：在配置页面降低分辨率或帧率，减少网络带宽需求。

---

### 设备频繁重启

**现象**：设备运行一段时间后自动重启。

**排查**：

1. 查看串口日志中的 crash dump 或 panic 信息
2. 检查堆内存是否低于 20 KB（健康监控输出）
3. 检查看门狗是否触发超时（30 秒无喂狗）
4. 确认电源供电充足（USB 3.0 或 5V/1A+ 适配器）
5. 检查 PSRAM 是否低于 500 KB

**解决**：如因内存不足，降低分辨率或帧率。如因电源问题，更换供电方式。

---

## 4. 恢复方法

### 方法一：BOOT 按钮恢复

1. 设备上电状态下
2. 按住 BOOT 按钮（GPIO0）保持 5 秒
3. LED 变为双闪（错误状态）
4. 2 秒后设备自动重启
5. 重启后恢复为 AP 模式（所有配置清空）

### 方法二：API 恢复出厂

```bash
curl -X POST http://<设备IP>/api/reset \
  -H "X-Password: admin"
```

### 方法三：擦除 Flash

通过 ESP-IDF 工具完全擦除 Flash（包括 NVS 配置）：

```bash
idf.py -p COM3 erase-flash
```

擦除后需要重新烧录固件：

```bash
idf.py -p COM3 flash monitor
```

> **注意**：恢复出厂设置将清除所有配置（WiFi、NAS、密码等），恢复为默认值 `admin`，设备重启后进入 AP 模式。TF 卡上的录像文件不受影响。
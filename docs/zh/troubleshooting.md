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


### 下载的 AVI 文件无法播放



**症状**：从文件管理器下载的 AVI 文件无法在视频播放器（Windows Media Player、VLC 等）中打开，或显示"0 帧"/"文件损坏"错误。



**根因**：在此修复之前的固件版本中，`close_segment()` 函数的 AVI 头部回填偏移量错误：

- `dwTotalFrames` 被写入偏移量 40 而非 48（覆盖了 `dwFlags` 字段）

- `strh dwLength` 被写入偏移量 136 而非 140（覆盖了 `dwRate` 字段）

- 导致 `dwTotalFrames=0` 和 `dwLength=0`，播放器认为文件中没有视频数据



**修复**：修正了 `video_recorder.c:close_segment()` 中的 fseek 偏移量：

```c

// avih dwTotalFrames: RIFF(12) + LIST_hdrl(12) + avih_hdr(8) + 16 = 48

fseek(s_seg.fp, AVI_RIFF_HDR_SIZE + 12 + 8 + 16, SEEK_SET);

// strh dwLength: strh_data_start + 32

fseek(s_seg.fp, strh_data_pos + 32, SEEK_SET);

```



**注意**：修复前录制的文件无法修复——更新固件后需重新录制。



### 出现 0 字节录像文件



**症状**：文件管理器中显示大小为 0 字节的录像文件。



**根因**：分段完成回调函数未检查是否实际写入了帧数据。当分段文件被打开后立即关闭（例如 SD 卡异常），文件缓存中会注册 0 字节的条目。



**修复**：在调用分段回调前添加了 `completed_size > 0` 检查：

```c

if (s_segment_cb && completed_size > 0) {

    s_segment_cb(completed_file, completed_size);

}

```



**解决方法**：可以从文件管理器中安全删除 0 字节文件。固件更新后不会再产生新的 0 字节文件。

### AVI 文件报"编解码器暂不支持"

**症状**：下载的 AVI 文件在 VLC 中播放报错 "编解码器暂不支持: VLC 无法解码格式 `    ` (No description for this codec)"，文件属性显示视频尺寸异常（如 600x1572865）。

**根因**：`write_hdrl()` 函数在生成 AVI 文件的 BITMAPINFOHEADER（strf chunk）时，漏写了 `biSize` 字段（固定值 40）。strf chunk 声明了 40 字节数据但只写了 36 字节，导致所有后续字段偏移 4 字节：

| 偏移位置 | 应写入 | 实际写入 |
|----------|--------|----------|
| biSize (0) | 40 | 800 (=biWidth) |
| biWidth (4) | 800 | 600 (=biHeight) |
| biHeight (8) | 600 | 0x180001 (垃圾) |
| biCompression (20) | "MJPG" | 0x00000018 (垃圾) |

VLC 无法识别编解码器四字节码，因此报"Codec not supported"。

**诊断方法**：

1. 用 VLC 详细日志模式验证：`vlc -vvv --file-logging your_file.avi`
2. 查看 VLC 输出中的 `biCompression` 和 `biWidth/biHeight` 是否正常
3. 用十六进制编辑器检查文件偏移 172-211 位置的 strf 数据

**修复**：在 `video_recorder.c:write_hdrl()` 的 strf chunk 中添加缺失的 `biSize` 字段：

```c
/* strf chunk — BITMAPINFOHEADER */
memcpy(buf + pos, "strf", 4);                         pos += 4;
put_u32(buf + pos, 40);             /* chunk data size */ pos += 4;
put_u32(buf + pos, 40);             /* biSize         */ pos += 4;  // <-- 这行之前缺失
put_u32(buf + pos, w);              /* biWidth        */ pos += 4;
put_u32(buf + pos, h);              /* biHeight       */ pos += 4;
// ... 后续字段不变
```

**经验教训**：

- BITMAPINFOHEADER 的 `biSize` 字段不是可选的，所有播放器都依赖它确定头部大小
- AVI 文件结构问题应从 strf chunk 的每个字段逐一验证，而非仅检查 RIFF/LIST 层级
- VLC 详细日志 (`-vvv --file-logging`) 是诊断 AVI 文件结构问题的利器

---

### 停止录像后无法下载文件

**症状**：停止录像后立即下载文件，API 返回 409 "File is currently being recorded"。

**根因**：`video_recorder.c` 中 `s_current_file` 全局变量在录像任务退出后未被清空。下载接口通过 `recorder_get_current_file()` 检查当前文件名，始终返回上一次的文件路径，导致已关闭的文件被误判为正在录制。

**修复**：在录像任务退出时的清理代码中添加：

```c
// video_recorder.c: recording_task() 函数末尾
if (segment_open) {
    close_segment();
    // ... callback ...
}
s_current_file[0] = '\0';  // <-- 添加这一行
```

---

### 文件夹删除按钮无反应

**症状**：点击文件管理页面中文件夹级别的 🗑 删除按钮后无任何效果，控制台显示 `/api/files/batch` 返回 400 错误。

**根因**：`api_files_batch_handler()` 使用栈上的 `char buf[2048]` 接收请求体。当文件夹包含大量文件（如 60 个）时，JSON body 超过 2048 字节被截断，`cJSON_Parse` 失败返回 400。

**修复**：改为根据 `Content-Length` 动态分配堆内存：

```c
int content_len = req->content_len;
if (content_len <= 0 || content_len > 8192) {
    return json_error(req, "Invalid request body", HTTPD_400_BAD_REQUEST);
}
char *buf = malloc(content_len + 1);
if (!buf) return json_error(req, "Out of memory", HTTPD_500_INTERNAL_SERVER_ERROR);
int len = httpd_req_recv(req, buf, content_len);
// ... 使用 buf ...
free(buf);
```

**注意**：所有 return 路径（包括错误路径）都必须 `free(buf)` 避免内存泄漏。


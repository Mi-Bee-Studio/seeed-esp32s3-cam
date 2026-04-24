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

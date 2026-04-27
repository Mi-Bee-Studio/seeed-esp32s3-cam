# 硬件手册

## 1. 开发板

**XIAO ESP32-S3 Sense** — Seeed Studio 出品的超小型 ESP32-S3 开发板，板载摄像头接口和 TF 卡槽。

- 尺寸：21 × 17.5 mm
- USB Type-C 供电与烧录
- 板载 OV2640 摄像头模组（可替换 OV3660）
- 板载 TF 卡槽（Micro SD）
- 1 个用户 LED（GPIO21，低电平有效）
- 1 个 BOOT 按钮（GPIO0）

---

## 2. 芯片规格

| 参数 | 规格 |
|------|------|
| 芯片型号 | ESP32-S3-WROOM-1-N8R8 |
| CPU | Xtensa 双核 LX7，最高 240 MHz |
| Flash | 8 MB（内嵌） |
| PSRAM | 8 MB Octal（内嵌） |
| WiFi | 802.11 b/g/n，2.4 GHz |
| 蓝牙 | Bluetooth 5 (BLE) |
| USB | USB OTG（支持 CDC + JTAG） |
| 工作电压 | 3.3 V |
| 工作温度 | -40°C ~ 85°C |

### PSRAM 配置

本项目使用 Octal PSRAM 模式（8 线），在启动时初始化（`CONFIG_SPIRAM_BOOT_INIT=y`）。摄像头帧缓冲分配在 PSRAM，双缓冲模式（`fb_count = 2`），每帧大小取决于分辨率和 JPEG 质量。

- 保留内部 RAM：16 KB（`SPIRAM_MALLOC_ALWAYSINTERNAL`）
- 保留给 DMA/内设：32 KB（`SPIRAM_MALLOC_RESERVE_INTERNAL`）
- 允许外部栈：是（`SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY`）

---

## 3. 引脚定义

### 摄像头接口（DVP）

| 功能 | GPIO | 说明 |
|------|------|------|
| XCLK | 10 | 主时钟输出（20 MHz） |
| SIOD (SDA) | 40 | SCCB 数据线（I²C） |
| SIOC (SCL) | 39 | SCCB 时钟线（I²C） |
| D0 | 18 | 数据位 0 |
| D1 | 17 | 数据位 1 |
| D2 | 16 | 数据位 2 |
| D3 | 15 | 数据位 3 |
| D4 | 14 | 数据位 4 |
| D5 | 12 | 数据位 5 |
| D6 | 11 | 数据位 6 |
| D7 | 48 | 数据位 7 |
| VSYNC | 38 | 垂直同步 |
| HREF | 47 | 水平参考 |
| PCLK | 13 | 像素时钟 |
| PWDN | -1 | 未使用 |
| RESET | -1 | 未使用 |

### TF 卡接口（SDMMC）

| 功能 | GPIO | 说明 |
|------|------|------|
| CLK | 7 | SD 时钟 |
| CMD | 10 | SD 命令/响应（**与摄像头 XCLK 共享**） |
| D0 | 8 | SD 数据线 0 |

### 其他

| 功能 | GPIO | 说明 |
|------|------|------|
| 状态 LED | 21 | 低电平有效（Active-low） |
| BOOT 按钮 | 0 | 低电平有效，按住 5 秒恢复出厂 |

> **重要**：GPIO10 同时用于摄像头 XCLK 和 SD 卡 CMD。通过使用 1-line SDMMC 模式（仅 CLK + CMD + D0），在摄像头和 SD 卡之间共享 GPIO10。4-line SDMMC 模式不可用于本硬件。

---

## 4. 摄像头

### 支持型号

| 型号 | PID | 最大分辨率 | 说明 |
|------|-----|-----------|------|
| OV2640 | 0x2642 | 2MP (UXGA) | 默认配置，XIAO 板载 |
| OV3660 | 0x3660 | 3MP (QXGA) | 可替换模组 |

系统在初始化时通过 SCCB 读取传感器 PID 自动检测型号，无需手动配置。

### 分辨率配置

| 配置值 | 分辨率 | 像素 |
|--------|--------|------|
| 0 | VGA | 640 × 480 |
| 1 | SVGA | 800 × 600（默认） |
| 2 | XGA | 1024 × 768 |

### 采集参数

| 参数 | 范围 | 默认 | 说明 |
|------|------|------|------|
| FPS | 1-30 | — | 帧率目标 |
| JPEG 质量 | 1-63 | — | 数值越小质量越高 |
| 帧缓冲 | 2（固定） | — | 双缓冲，PSRAM 分配 |
| 抓取模式 | `CAMERA_GRAB_LATEST` | — | 始终获取最新帧 |
| XCLK 频率 | 20 MHz | — | 摄像头主时钟 |

---

## 5. TF 卡

### 硬件要求

| 参数 | 要求 |
|------|------|
| 卡类型 | Micro SD / SDHC / SDXC |
| 文件系统 | FAT32 |
| 速度等级 | Class 10 或更高 |
| 接口模式 | SDMMC 1-line |
| 分配单元 | 64 KB |
| 最大同时打开文件 | 8 |

### 存储结构

```
/sdcard/
├── recordings/              # 录像根目录
│   └── YYYY-MM/
│       └── DD/
│           ├── REC_YYYYMMDD_HHMMSS.avi
│           └── ...
└── config/                  # 配置覆盖目录
    ├── wifi.txt             # KEY=VALUE 格式
    └── nas.txt              # KEY=VALUE 格式
```

### 循环存储策略

- 每段录像完成后检查剩余空间
- 剩余 < 20%：自动删除最旧的录像文件
- 持续删除直到剩余 ≥ 30%
- 单次清理安全上限：100 个文件

---

## 6. LED 状态指示

状态 LED 连接在 GPIO21，**低电平有效**（输出 0 = 亮，输出 1 = 灭）。

| 状态 | LED 表现 | 触发条件 |
|------|----------|----------|
| `LED_STARTING` | 常亮 | 系统启动中 |
| `LED_AP_MODE` | 慢闪（1 秒周期） | WiFi AP 模式（等待配置） |
| `LED_WIFI_CONNECTING` | 快闪（200ms 周期） | WiFi STA 正在连接 |
| `LED_RUNNING` | 熄灭 | 正常运行中 |
| `LED_ERROR` | 双闪（闪两下，停 1 秒） | 错误状态（SD 卡故障等） |

### 双闪时序

```
ON 200ms → OFF 200ms → ON 200ms → OFF 1000ms → 循环
```

通过 FreeRTOS 软件定时器实现，定时器周期 200ms，使用状态机计数器控制双闪逻辑。

---

## 7. 电源

| 参数 | 规格 |
|------|------|
| 供电方式 | USB Type-C |
| 电压 | 5V |
| 最小电流 | 1A（推荐） |
| 工作电压 | 3.3V（板载 LDO） |

### 功耗参考

| 模式 | 估算功耗 |
|------|----------|
| 待机（WiFi 连接） | ~100 mA |
| 录像（SVGA 15fps） | ~200 mA |
| 录像 + 流媒体 + NAS 上传 | ~300-400 mA |
| WiFi AP 模式 | ~120 mA |

> **注意**：使用 USB 3.0 端口或独立 5V/1A 以上电源适配器供电，避免因 USB 端口供电不足导致不稳定。

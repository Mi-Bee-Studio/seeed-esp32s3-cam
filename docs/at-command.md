# MiBee Cam 家族 AT 指令契约（v1.0，2026-09-04）

> **定位**：四仓（ai-thinker / esp32s3-n16r8 / luatos / seeed）串口 AT 控制面的
> 统一契约，地位同 `docs/api-contract.md`（HTTP 面）。各仓实现可按板能力裁剪，
> 但**名称、语义、响应格式必须一致**；板级扩展指令在各仓 AGENTS.md 登记。
> **历史**：此前三套各自演化（ai-thinker/serial_config.c、n16r8/at_command.c、
> luatos/at_command.c 命名互不兼容，seeed 无 AT），2026-09-04 起统一。

## 1. 传输与约定

- **通道**：板载调试串口控制台。ai-thinker（CH340）/ luatos（CH343）/ n16r8（CH340）
  为 UART0 115200-8N1；seeed 为 USB-JTAG CDC 控制台（`/dev/ttyACM*`）。
- **行协议**：一行一条指令，`\r\n` 结尾，指令大写；参数区分大小写。
- **响应约定**：成功末行 `OK`；失败 `ERROR[: 原因]`；数据行 `+<名字>:<内容>`。
- **写串口会复位 CH340/CH343 板**（宿主机侧驱动行为，PIT-003）：生产/诊断
  场景用常驻采集器，AT 交互接受"开串口即重启"的现实。

## 2. 核心指令集（四板必备；⬜=按板裁剪）

| 指令 | 语义 | 备注 |
|---|---|---|
| `AT` | 链路测试 → `OK` | |
| `AT+HELP` | 列出本板支持的指令 | |
| `AT+GMR` | 固件版本 / 芯片 / 板型 / 传感器 | |
| `AT+WIFI?` | STA 状态：State / SSID / IP（**不回显密码**） | |
| `AT+WIFI=ssid,pass` | 写入凭据 → 保存 → 重启生效 | 唯一的 WiFi 凭据写入正道 |
| `AT+WIFISCAN` ⬜ | 扫描 AP（`wifi_scan` 能力板） | seeed/ai-thinker/n16r8 |
| `AT+IP?` | IP/掩码/网关 | 别名 `AT+CIFSR`（luatos/n16r8 历史名） |
| `AT+STATUS` | 汇总状态：uptime / heap / 温度(有则给) / WiFi / 摄像头 / 分辨率 / 画质 | 历史名 `AT+CONFIG`/`AT+INFO` 保留为别名 |
| `AT+CAMRES?` | 当前分辨率 + 板级可选列表 + 板级上限 | 列表与 `GET /api/camera` 的 `supported_resolutions` 同源 |
| `AT+CAMRES=n` | 设分辨率（越界 ERROR）；**生效方式随板**：ai-thinker 热重配 / n16r8 热重配 / seeed 保存+重启 / luatos 保存+重启 | |
| `AT+CAMQUAL?` | 当前画质 + `[min,max]` 边界 | 家族边界 10-63（PIT-021） |
| `AT+CAMQUAL=n` | 设画质，越界（<10 或 >63）ERROR | |
| `AT+REBOOT` | 重启（别名 `AT+RST`） | |
| `AT+RESTORE` | 恢复出厂配置 → 重启 | |
| `AT+CFGGET=field` ⬜ | 读白名单配置键 | **白名单不含任何 `*pass*`/`*password*`** |
| `AT+CFGSET=field,value` ⬜ | 写白名单配置键（含 `wifi_pass` 等敏感键，只写不读） | |
| `AT+SAVE` ⬜ | 持久化当前配置 | 各仓 config 写路径自带落盘时为幂等空操作 |

⬜ 裁剪规则：无该硬件/该子系统时 `AT+HELP` 不列出；指令返回
`ERROR: not supported on this board`。

## 3. 安全策略（红线）

- **任何读指令不得回显密码**（`wifi_pass` / `wifi_pass2` / `web_password` /
  `rtsp_pass` / `webhook_secret`）。凭据只经 `AT+WIFI=` / `AT+CFGSET=` 写入。
  历史教训：luatos 旧版 `AT+CFGGET=wifi_pass` 明文可读（2026-09-04 曾用它做
  板间凭据迁移，属一次性特权操作），统一后已从白名单剔除。
- 串口物理接触 = 完全控制（esptool 可刷任意固件），AT 层的脱敏是防肩窥/防日志
  泄漏，不是安全边界。

## 4. 历史别名（保留兼容，不新用）

| 别名 | 归一名 | 仓 |
|---|---|---|
| `AT+CWJAP?` / `AT+CWJAP=` | `AT+WIFI?` / `AT+WIFI=` | luatos |
| `AT+CWLAP` | `AT+WIFISCAN` | luatos |
| `AT+CWQAP` | —（清凭据专用，保留） | luatos |
| `AT+RST` | `AT+REBOOT` | luatos |
| `AT+CONFIG` / `AT+INFO` | `AT+STATUS` | ai-thinker / n16r8 |
| `AT+HEAP` / `AT+UPTIME` / `AT+TEMP` | （`AT+STATUS` 的分量，保留独立可用） | luatos |
| `AT+NAME?` / `AT+NAME=` | 设备名（等价 `AT+CFGGET/SET=device_name`） | luatos |
| `AT+DEVICE=` / `AT+TZ=` | （等价 CFGSET，保留） | ai-thinker |

## 5. 板级扩展指令（登记制）

| 仓 | 扩展 | 说明 |
|---|---|---|
| n16r8 | `AT+AIFACE?/=`,`AT+AIMOTION?/=`,`AT+AIQR?/=` | AI 管线开关（唯一带 AI 的板） |
| n16r8 | `AT+LED`/`AT+LED=`,`AT+RTSPPASS=` | 闪光灯 / RTSP 密码写 |
| n16r8 | `AT+CAMCAP` | 拍一帧报告尺寸 |
| luatos | `AT+STREAM?` | MJPEG 流状态 |
| seeed | —（首版无扩展） | |

## 6. 板级生效语义对照（与 api-contract §5 一致）

| 仓 | 分辨率变更 | 画质变更 |
|---|---|---|
| ai-thinker | 热重配（互斥锁+drain，OV2640 实测有效） | 热重配 |
| n16r8 | 热重配（camera_reinit 协调停 AI/广播再重建） | 热重配 |
| seeed | **保存+重启**（OV5640 运行时 set_framesize 无效，PIT-019） | 热应用（单寄存器写） |
| luatos | **保存+重启**（fb_count=1 DRAM 热重配竞态，PIT-012） | **保存+重启** |

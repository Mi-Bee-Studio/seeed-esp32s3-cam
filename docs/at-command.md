# MiBee Cam 家族 AT 指令契约（v1.1，2026-09-05）

> **定位**：四仓（ai-thinker / esp32s3-n16r8 / luatos / seeed）串口 AT 控制面的
> 统一契约，地位同 `docs/api-contract.md`（HTTP 面）。各仓实现可按板能力裁剪，
> 但**名称、语义、响应格式必须一致**；板级扩展指令在本文件 §5 登记。
> **历史**：此前三套各自演化（ai-thinker/serial_config.c、n16r8/at_command.c、
> luatos/at_command.c 命名互不兼容，seeed 无 AT），2026-09-04 起统一契约，
> 2026-09-05 v1.1 起四仓共享同一核心实现（见 §0）。
>
> **v1.1 变更（2026-09-05）**：共享核心纪律（§0）；`AT+WIFISCAN` 升为四板必备；
> 删除未登记的 `AT+RESET`；`AT+WIFI=` 生效方式随板登记（§2）；`AT+GMR` 必须
> 报真实固件版本；输入大小写不敏感；非 AT 行静默忽略（§1）。

## 0. 共享核心纪律（v1.1 起）

- 四仓使用**同一份核心实现**：`main/at_command.c` + `main/at_command.h`
  （解析器 / 指令表 / 响应框架 / HELP 自动生成 / CFGGET·CFGSET 白名单反射表），
  四仓文件 **md5 必须一致**，纪律同 SPA 四文件（api-contract §7）。
- 板差异只允许进入板级 port 层 `main/at_port.h/.c`：IO 后端（UART0-stdin vs
  seeed USB-JTAG CDC）、能力裁剪、生效语义钩子（热重配 vs 保存+重启 vs 热连）、
  §5 登记的板级扩展指令。
- 改核心 = 改四处：先改本契约，再改一仓核心文件，md5 校验后同步其余三仓。

## 1. 传输与约定

- **通道**：板载调试串口控制台。ai-thinker（CH340）/ luatos（CH343）/ n16r8
  （CH340）为 UART0 115200-8N1；seeed 为 USB-JTAG CDC 控制台（`/dev/ttyACM*`）。
- **行协议**：一行一条指令，`\r\n` 结尾；**输入指令大小写不敏感**（`at+wifi?`
  等价 `AT+WIFI?`），参数区分大小写。
- **响应约定**：成功末行 `OK`；失败 `ERROR[: 原因]`；数据行 `+<名字>:<内容>`。
- **噪声容忍**：非 `AT` 开头的行**静默忽略**（控制台与 ESP_LOG 共口时不制造
  错误刷屏）。
- **写串口会复位 CH340/CH343 板**（宿主机侧驱动行为，PIT-003）：生产/诊断
  场景用常驻采集器，AT 交互接受"开串口即重启"的现实。

## 2. 核心指令集（四板必备；⬜=按板裁剪）

| 指令 | 语义 | 备注 |
|---|---|---|
| `AT` | 链路测试 → `OK` | |
| `AT+HELP` | 列出本板支持的指令 | 核心自动生成 + port 层扩展 |
| `AT+GMR` | 固件版本 / 芯片 / 板型 / 传感器 | **必须报 `esp_app_desc` 实际版本**（v1.1 起禁止硬编码） |
| `AT+WIFI?` | STA 状态：State / SSID / IP（**不回显密码**） | |
| `AT+WIFI=ssid,pass` | 写入凭据 → 保存 → 生效；**生效方式随板**（§7）：luatos 热连（不重启）/ 其余板 1s 后重启 | 唯一的 WiFi 凭据写入正道 |
| `AT+WIFISCAN` | 扫描 AP（`wifi_scan` 能力，四板皆 true，v1.1 起必备） | 数据行 `+AP:<ssid>,<rssi>,<auth>`，按 RSSI 降序 |
| `AT+IP?` | IP/掩码/网关 | 别名 `AT+CIFSR`（luatos/n16r8 历史名） |
| `AT+STATUS` | 汇总状态：uptime / heap / 温度(有则给) / WiFi / 摄像头 / 分辨率 / 画质 | 历史名 `AT+CONFIG`/`AT+INFO` 保留为别名 |
| `AT+CAMRES?` | 当前分辨率 + 板级可选列表 + 板级上限 | 列表与 `GET /api/camera` 的 `supported_resolutions` 同源；**value 为家族统一 framesize_t 刻度**（api-contract §5） |
| `AT+CAMRES=n` | 设分辨率（越界 ERROR）；**生效方式随板**（§7） | |
| `AT+CAMQUAL?` | 当前画质 + `[min,max]` 边界 | 家族边界 10-63（PIT-021） |
| `AT+CAMQUAL=n` | 设画质，越界（<10 或 >63）ERROR | |
| `AT+REBOOT` | 重启（别名 `AT+RST`） | |
| `AT+RESTORE` | 恢复出厂配置 → 重启 | **不存在 `AT+RESET`**（v1.1 起从 ai-thinker/n16r8 删除：其曾映射为恢复出厂，跨板反射会误清配置） |
| `AT+CFGGET=field` ⬜ | 读白名单配置键，数据行 `+CFGGET:<field>=<value>` | **白名单不含任何 `*pass*`/`*password*`** |
| `AT+CFGSET=field,value` ⬜ | 写白名单配置键（含 `wifi_pass` 等敏感键，只写不读） | 白名单 = `docs/config-contract.md` 字段表的读侧子集 |
| `AT+SAVE` ⬜ | 持久化当前配置 | 各仓 config 写路径自带落盘时为幂等空操作 |

⬜ 裁剪规则：无该硬件/该子系统时 `AT+HELP` 不列出；指令返回
`ERROR: not supported on this board`。

## 3. 安全策略（红线）

- **任何读指令不得回显密码**（`wifi_pass` / `wifi_pass_2` / `web_password` /
  `rtsp_pass` / `webhook_secret` / `webdav_pass`）。凭据只经 `AT+WIFI=` /
  `AT+CFGSET=` 写入。历史教训：luatos 旧版 `AT+CFGGET=wifi_pass` 明文可读
  （2026-09-04 曾用它做板间凭据迁移，属一次性特权操作），统一后已从白名单剔除。
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
| n16r8 | `AT+WIFI2=ssid,pass` | 备用网络凭据（查询脱敏；`ssid,` 空串清除） |
| n16r8 | `AT+LED`/`AT+LED=`,`AT+RTSPPASS=` | 闪光灯 / RTSP 密码写 |
| n16r8 | `AT+CAMCAP` | 拍一帧报告尺寸 |
| luatos | `AT+STREAM?` | MJPEG 流状态 |
| seeed | —（首版无扩展） | |

## 6. 板级生效语义对照（与 api-contract §5 一致）

| 仓 | 分辨率变更 | 画质变更 | WiFi 凭据写入 |
|---|---|---|---|
| ai-thinker | 热重配（互斥锁+drain，OV2640 实测有效） | 热重配 | 保存+重启 |
| n16r8 | 热重配（camera_reinit 协调停 AI/广播再重建） | 热重配 | 保存+重启 |
| seeed | **保存+重启**（OV5640 运行时 set_framesize 无效，PIT-019） | 热应用（单寄存器写） | 保存+重启 |
| luatos | **保存+重启**（fb_count=1 DRAM 热重配竞态，PIT-012） | **保存+重启** | **热连**（保存后不停机切换 STA，v1.1 起登记为本板语义） |

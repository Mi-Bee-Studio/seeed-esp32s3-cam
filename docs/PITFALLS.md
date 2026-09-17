> **公开脱敏版说明**：本文件是家族坑库（PITFALLS）的对外发行版。
> 真实 SSID/网段/密码已按 PIT-027 约定替换为占位值（`HomeAP-*`、
> `192.0.2.0/24` RFC 5737 文档网段、`REDACTED-PW`）；设备以末段八位组
> 指称（如 `.119`）。技术事实、根因与修复步骤与内部版逐条一致。

# MiBee Cam 坑知识库（PITFALLS.md）

> **定位**：跨项目的"坑/事故/经验"唯一归档处。根 `AGENTS.md` 只留简短指针，
> 板级深水区细节仍在各子仓 `AGENTS.md`（它们对 README 保持权威）。
> **优先级**：本文档（跨项目坑）≈ 子仓 AGENTS.md（板级坑）＞ README。
>
> **用法（给 agent）**：动手前先按关键词查本文档（`accept (23)`、`rst:0xc`、
> `open-reset`、`TIME_WAIT`…）；每次踩坑/排障结束，按文末模板追加条目，
> 已有条目只补充"复发/新证据"，不重复开条。

---

## 1. 家族级坑（四个仓通用）

### PIT-001 EMFILE 重启循环 —— "WiFi 不稳定"的头号真凶 ⭐（2026-09-03，ai-thinker 实锤）

**症状**：用户视角"WiFi 信号一直不稳定很差"：页面时好时坏、HTTP 被重置、
ping 抖动大。**不是射频问题。**

**真因链**：SPA 流看门狗每 ~7s 重连 MJPEG（:81）→ 每个被踢连接在设备侧留
TIME_WAIT（默认 2×MSL = 120s）→ 默认 `LWIP_MAX_SOCKETS=10` 必爆 →
httpd `accept` 报错 23（EMFILE）→ health 探测 6×10s 失败 → 自愈逻辑重启 →
重启后 ~12s 再爆 → 循环。实测一段日志 21 次重启。

**日志签名（grep 这三连）**：
```
E httpd: httpd_accept_conn: error in accept (23)
W health_monitor: httpd :80 probe failed (n/6)
rst:0xc (SW_CPU_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
```

**修复（已三仓验证）**：`sdkconfig.defaults` 加
`CONFIG_LWIP_MAX_SOCKETS=16` + `CONFIG_LWIP_TCP_MSL=15000`
（TIME_WAIT 缩到 30s，~4-5 个驻留连接）。**改 defaults 后必须删 sdkconfig 重配**。
**2026-09-03 复发记录：四仓已全部合入该修复**（esp32s3-n16r8 走 PR #6 补齐；
配套 httpd 会话级 TCP_NODELAY + keepalive 也已四仓齐——本地脏工作区能编过、
干净树编不过的教训见 PIT-018）。

**诊断口诀**：见 `accept (23)` 先查 socket 表，别先怀疑天线。
流程：① `/api/status` 看 `uptime`——几十秒即在重启循环；② 采集器
`tools/overnight_reboots.log` 找上述三连签名；③ 修复顺序：先 lwIP 配置，
再考虑射频/省电（`wifi_power_save` 出厂 0=PS_NONE，一般无需动）。
重启循环期间测的 ping 抖动无意义，修复后再测基线。

### PIT-002 "探测失败→重启"自愈误杀家族（2026-09-02 seeed / 2026-09-03 ai-thinker / 2026-09-04 n16r8 收尾）

任何 `探测失败 → esp_restart()` 自愈逻辑，重启前**必须排除两类误杀**：
1. **网络不可达**（WiFi 掉线时本地探测必失败，EHOSTUNREACH/recv 113 ——
   ai-thinker "掉线 60s 被翻译成重启，越重启越掉线"）；
2. **EMFILE**（见 PIT-001，探测自身要 socket，表满时必失败）。
规则：WiFi 未连接时不计数；EMFILE 场景修根因而不是放宽重启阈值。
**n16r8 收尾实证（2026-09-04）**：该仓 socket/MSL 早已家族化（16/15000）仍
2-4.5 分钟循环重启——缺的正是探测分类：探针 socket()/connect() 的
EMFILE/ENOBUFS 被计为"httpd 死"。补上 seeed 的资源类失败不计数 + ai-thinker
的 WiFi 断线不计数后，误杀消失（仅剩一次真卡死的正确自愈）。**教训：
三件套（sockets/MSL + 探测资源分类 + WiFi 断线守卫）缺一不可，
单独修 sockets/MSL 不够。**

### PIT-003 串口 open-reset 陷阱（2026-09-03，跨项目）

本机上**任何一次 open() CH340/CH343 端口都会复位该板**（`ttyUSB0` ai-thinker、
`ttyACM1` luatos）：适配器驱动在 open 时断言复位线，pyserial 的
`rts=False, dtr=False` 构造参数拦不住（实测 uptime 52→16）。seeed 的
Espressif 原生 USB-JTAG（`ttyACM0`）免疫。
**规则**：观察这类板子必须走常驻采集器 `tools/overnight_log.py`
（已部署 seeed/luatos/ai-thinker），绝不临时开串口"看一眼"——
否则自造重启、污染诊断。采集器自身注意：CH340 上 pyserial open 后必须立即
`ser.rts=False; ser.dtr=False`，否则 EN 被按在复位态（0 字节输入）。

### PIT-004 禁止手改 `managed_components/`（2026-09-02 事故）

改了不进 git、`fullclean`/新克隆即丢、CI 与本地静默分叉。补丁正道：
`patches/` + 根 CMake 拷贝步骤（esp32s3-n16r8-cam 模板），或 vendor 进
`components/`（seeed-esp32s3-cam 模板）。

### PIT-005 共享 SPA 四文件纪律（2026-09-03 教训：SPA 切换丢功能）

统一 SPA（index.html / app.js / i18n.js / style.css）在四仓 **md5 必须一致**。
2026-09-03 ai-thinker 从 MPA 切 SPA 时丢了旧页面的双 WiFi 表单（用户直接
报"双 wifi 配置 web 上没有"）。规则：
- 改任何一个仓的 SPA，改完同步其余三仓并 md5 验证；
- 功能对板有差异时用**能力探测**降级（如 `wifi_ssid_2`、`rtsp_user` 字段
  仅当 `GET /api/config` 返回该键才显示/发送——**n16r8 的 POST 是白名单
  校验，没暴露的键绝不能发**，否则保存直接报错）；
- 改完必须重打包 SPIFFS 烧录才生效（见 PIT-008）。

### PIT-006 信设备不信文档（2026-09-02，seeed）

文档写 OV2640，板子实戴 OV5640（驱动自动识别）。传感器/配置一律以
boot log 或 `/api/status` 的 `camera` 字段为准；发现矛盾要上报，
不要默默改文档、更不要按文档"纠正"设备。

### PIT-007 陈旧 sdkconfig 静默覆盖 defaults（家族通用）

`sdkconfig` 是生成物且 gitignored；改了 `sdkconfig.defaults` 不删 sdkconfig，
新配置**静默不生效**（烧出去的还是旧值，极难察觉）。铁律：
改 defaults → `rm sdkconfig` → `set-target` → build → 烧录后用生成物反查
（`grep CONFIG_XXX sdkconfig`）确认。

### PIT-008 Web UI 与固件构建耦合（家族通用）

`main/web_ui/` 资产打包进 SPIFFS 分区烧进 flash——改 HTML/JS/CSS 后
**必须重新构建+烧录**才有生效，浏览器强刷（no-cache 头已有）不是问题所在。
注意各仓 CMake 的 spiffs `DEPENDS` 显式文件依赖（目录级依赖不随内容编辑触发）。

### PIT-009 工作区搬迁 → build/ 缓存绝对路径（2026-08）

`~/Projects/iot-cam` → `~/Projects/esp-cam` 后，旧 `build/` 报
"configured for project .../iot-cam/..."。每仓 `idf.py fullclean` 一次即好。

### PIT-010 宿主机（Arch）环境坑

- pacman 升内核不重启 → `ch341` 模块无法绑定，`/dev/ttyUSB*` 消失 → `sudo reboot`；
- 串口权限需 `uucp` 组；
- npm 默认源在本网络超时（当前无 JS 工具链，若引入注意换源）。

### PIT-011 浏览器"神秘 MJPEG 客户端"不是泄漏（2026-09-03，luatos 教训）

用户开着 SPA 页面时，img 自愈重连（被踢 ~7s 即回、重启后也抢回槽位），
日志里表现为持续的 connect/kick 循环——这是正常行为，不是资源泄漏，
不要当成 bug 去"修"。但它正是 PIT-001 TIME_WAIT 的流量来源。

### PIT-012 单 framebuffer 板的通用约束（luatos 模式，fb_count=1 无 PSRAM）

- MJPEG 双客户端会把堆压到百字节级 → 流任务自断；解法是硬上限
  `MAX_STREAM_CLIENTS=1`（LRU 踢旧观众）+ 堆水位纵深防御门槛；
- **开机初期高堆瞬时值会绕过堆门槛**（37K+ 时放进来第二路照样死）——
  门槛只能当纵深，不能当唯一闸门；
- 摄像头热重配（deinit+init）与并发取帧是致命竞态 → 该板改"保存+应答+1s
  后重启应用"；有 PSRAM fb_count=2 的姐妹板不受此限（板间不通用，别互抄）。

### PIT-013 事件/定时类隐性上限（2026-09-03，luatos）

功能叠加会悄悄越过编译期上限且**部分静默失效**：event_bus 订阅表 8→16
（9 个 WS 事件 + webhook 订阅超限，启动即 `subscription table full`）。
加订阅方前先数表容量；同类：httpd `max_uri_handlers`（30→40 曾丢静态兜底）、
lwIP socket 表（PIT-001）。**通配路由先注册会遮蔽精确端点**（`GET /*`
导致 `/api/camera`、`/ws` 运行时 404）——注册顺序：精确 → /ws → 静态兜底。

### PIT-014 时间显示用 esp_timer，别用 time(NULL)（2026-09-03，luatos）

SNTP 同步后 `time(NULL)` 从 epoch 起跳（日志打出 `Uptime: 1788399188`）。
uptime/时长一律 `esp_timer_get_time()`。

### PIT-015 "板子很慢"先查连的哪个 AP/信道/带宽，再查板子（2026-09-03，luatos 实锤）

**症状**：.148（luatos）网页"感觉挺慢"——ping 平均 317ms~1.7s（0% 丢包）、
`/api/status` 最长 8.8s。**同一时刻** .139（ai-thinker）同网推流 ping 仅 6ms。

**真因（三层叠加）**：
1. **连错了 AP**：慢的两块板（.133 seeed / .148 luatos）都连 HomeAP-1
   （ch11、**HT40** 40MHz），快的 .139 连 HomeAP-2（ch2、20MHz）——
   2.4G HT40 占双信道、灵敏度差 ~6dB，弱信号下 PER 恶化；
2. **luatos AMPDU 关闭**（当年绕驱动 stall 的权宜）：无聚合吞吐塌到 1-3Mbps，
   MJPEG 流吃满空中时间，ping/HTTP 全排队；
3. （对照排除）RSSI 关联瞬时值不可信：关联时 -56，稳态 API 报 -70。

**修复（luatos，已烧录验证）**：STA 强制 `WIFI_BW20` + AMPDU 重开（seeed 现行
配置；luatos AGENTS 的 Do-Not"重开需上板复验"已执行：`<ba-add>` 会话建立、
ping 5ms、落点 HomeAP-2 ch2 -59dBm）。**教训**：
- 板间延迟对比是定位利器（三板同网同负载，一快两慢 → 查共性与差异：连的 AP、
  信道/带宽、AMPDU、温度）；
- ESP32 日志 `connected with <ssid>, channel N, 40D|BW20` 一行就能读出全部链路参数；
- 修 AP/信道/带宽三层比动固件业务逻辑见效快得多。

### PIT-016 S3 板高温 = 独立故障源（2026-09-03，seeed 观察项）

seeed（.133）日志每 60s 报 `Chip temperature high (93.5°C)`（规格上限 85°C），
同时 ping 300ms 级 + 持续被 404 请求轰炸（`httpd_uri: 404` + `send : 104` +
`setsockopt : 22` 三连，疑似局域网某客户端反复请求不存在的 URI）。高温是散热/
负载问题固件不可根治，但它会放大一切延迟问题——**排障时先看温度再下结论**。
（未闭环：404 客户端身份、高温根因。）

### PIT-017 Web OTA 的正确用法（2026-09-03，seeed 实战验证）

家族 OTA 端点（seeed / ai-thinker / n16r8 有；**luatos 单分区无 OTA**）吃的是
**裸二进制流，不是 multipart**：
```bash
curl -X POST http://<ip>/api/ota/upload -H 'X-Password: <pwd>' \
     -H 'Content-Type: application/octet-stream' \
     --data-binary @build/mibee_cam.bin        # 固件 → next OTA 槽 → 自动重启
curl -X POST http://<ip>/api/ota/spiffs -H 'X-Password: <pwd>' \
     --data-binary @build/spiffs.bin           # UI → 整擦 SPIFFS → 自动重启
```
注意：镜像必须 ≤ OTA 槽尺寸（seeed 1.9MB）；上传中途失败 SPIFFS 即丢（只能串口救）；
成功后用 `/api/ota/info` 看 `running_partition` 切换 + **md5(设备 /app.js) ==
md5(仓库文件)** 验证 UI 真到位。弱网下大固件上传给足 curl --max-time。

### PIT-018 脏工作区掩盖干净树编译错误；提交时严防卷入未完成 WIP（2026-09-03，n16r8/ai-thinker 双案）

**症状**：本地 `idf.py build` 全绿，CI（干净检出）编译失败。两起：
① ai-thinker HEAD 的 `main.c` 调用 `wifi_start_sta_preferred()`，但实现只存在于
未提交的 `wifi_manager.c/h`——主干自 761ceb1 起对干净树就是坏的；
② n16r8 修 issue 时 `git add main/web_server.c` 把工作区里进行中的 OTA 端点代码
（引用未跟踪的 `ota_updater.h`）一起卷进提交，CI 立即红。

**规则**：
1. **本地构建通过 ≠ 分支可合**：本地工作区常叠着多套未提交改动，恰好补齐了符号。
   CI 的干净树才是真值；PR 必须等 build 绿。
2. 提交 issue 修复时**逐文件审视 `git diff`**：目标文件若混有无关 WIP（尤其引用
   untracked 文件的 include/handler），要么临时还原-提交-恢复（n16r8 拆分法），
   要么把 WIP 独立成完整提交（ai-thinker wifi_manager 法）。
3. 修 bug 前先看主干 CI 是否本来就红——红主干上的"我的 PR 弄坏了 CI"多半是
   暴露而非引入。

### PIT-019 OV5640 运行时 set_framesize 无效——"成功"但帧不变（2026-09-04，seeed 实锤）

**症状**：POST /api/camera 改分辨率（HD→SXGA/UXGA/FHD/QXGA 均复现），响应正常、
串口打出 `Resolution set to SXGA`，但 `/api/capture` 仍产出旧分辨率 JPEG（HD 改
SXGA 后仍是 1280x720），MJPEG 流 0 帧。
**真因**：esp32-camera 的 ov5640.c `set_framesize()` 只写窗口/ISP 寄存器（I2C 成功
即返回 0），不重启 DSP/JPEG 管线——寄存器写了、输出不变。OV2640 无此问题
（ai-thinker 运行时热改分辨率实测有效）。
**日志签名**：`camera: Resolution set to <X>` 之后帧尺寸不变；mjpeg 侧
`No frames for 10 tries, ending stream` 循环。
**修复（已烧录验证）**：seeed 分辨率变更走"保存+应答+1s 后重启"，由
`esp_camera_init()` 在启动时以新 framesize 初始化（唯一可靠路径）；运行时仅
`set_quality`（单寄存器写）保留热应用。`camera_set_resolution()` 已加 ⚠️ 注释。
**教训**：传感器驱动的 "return 0" ≠ 生效；改分辨率后必须验证**实际输出帧尺寸**
（`file` 看 capture JPEG），别信日志。

### PIT-020 seeed 停录像 = 杀流（帧源与录像任务耦合，2026-09-04 观察项）

**症状**：seeed 停止录像后（`Recording stopped`），MJPEG 流永久 0 帧，SPA 看门狗
每 ~30s 空转重连；串口循环 `mjpeg: No frames for 10 tries, ending stream` +
`fbcast: Subscriber registered: MJPEG`。2026-09-03 22:49 用户停录像后流死了 1h+，
期间一切"分辨率测试 0 帧"都是这具尸体（教训：测流前先确认帧源活着）。
**真因**：MJPEG/RTSP 的帧来自 `recording_task`（video_recorder.c），该任务仅在
RECORDER_RECORDING/PAUSED 态循环；`/api/record stop` 置 IDLE 后无人取帧发布到
fbcast。开机无条件 `recorder_start()`（main.c）掩盖了这一耦合——直到用户手动停录。
**修复（2026-09-04 已上板验证）**：`RECORDER_PREVIEW` 状态 + 引用计数
`recorder_preview_acquire/release`（MJPEG 客户端起/断挂钩；RTSP feed 任务按会话数
持引用）。有观众时 `recorder_stop` 降级预览（走既有 write_to_sd=false 关段路径），
`recorder_start` 从预览恢复不重启任务；暂停被观看时也按预览出帧。
**修复过程挖出两个潜伏 bug（同提交修掉，模式值得全家警惕）**：
1. "Open first segment" 在 write_to_sd=false 时也消耗 `segment_open` 标志 →
   预览→恢复录像跳过 open_segment 直接写已 close 的 FILE*（Guru
   LoadProhibited @ video_recorder.c fflush）——**状态标志的置位必须与资源
   实际获取同点**。
2. `sd_writer_task` 循环条件漏 PREVIEW 先行退场 → recorder 的 sentinel
   `xQueueSend(portMAX_DELAY)` 对无人排水的队列永久阻塞（TWDT 30s abort）——
   **配套任务的状态机必须镜像同款运行态集合**；屏障类发送一律有界。
验证：两轮停/启循环 + 125s 探针，0 断线 0 重启，recording 时间线精确。
**教训**：测流前 `GET /api/record` 确认状态；"流死了"先查帧源任务是否在跑。

### PIT-021 画质 q<10 撞 JPEG 帧缓冲预算 + 板级上限矩阵（2026-09-04，三仓联测）

**症状/真因**：esp32-camera 按 `宽×高/5` 分配 JPEG 帧缓冲（假设最高 1:5 压缩）；
q<10 在细节丰富的场景帧超预算 → 截断/坏帧。此前三仓校验 1-63 甚至无校验
（luatos POST 画质完全不查、ai-thinker 收 0-24 的 framesize 越界枚举），用户可选
到设备撑不住的配置。
**修复（已烧录验证）**：家族统一 `CAMERA_QUALITY_MIN=10/MAX=63`
（各仓 camera_driver.h），POST 越界 400；`GET /api/camera` 新增
`quality_min/quality_max`，SPA 滑杆按其钳制（四仓 md5 已同步）；NVS 旧值加载钳制。
**板级实测上限**（90s 推流+温度/堆采样，详见各仓 AGENTS.md 与契约 §5.1）：
- ai-thinker：UXGA（传感器上限；采集 ~1.7fps、推流 ~0.6fps，慢但零故障）
- seeed：UXGA（**FHD 97.5°C / QXGA 100.5°C 超 85°C 规格剔除**，PIT-016 延伸）
- luatos：VGA（SVGA 起 DRAM 堆螺旋，PIT-012 延伸）
- n16r8：**SVGA**（2026-09-04 复测翻案：首轮“仅 VGA”被 PIT-022 污染链毁掉；
  复测 SVGA 冷启动实拍 800×600 正常，XGA+ 冷启动取帧死并楔死整板——
  模组/DVP 组合极限，与 PSRAM 无关；详见其 AGENTS 与 PIT-022）。

**附录：分辨率三层上限架构（2026-09-04 下午家族统一，四仓已落地）**

"上限能不能只绑传感器？"——不能：同一颗 OV2640 在 ai-thinker 上 UXGA 稳定、
在 luatos 上 VGA 之外必死（DRAM），四板互为反例。落地架构：
`effective = min(传感器层, 板级层, 内存层)`，各层职责——
1. **sensor**（运行时自动）：查 esp32-camera 组件自带能力表
   `esp_camera_sensor_get_info(&sensor->id)->max_size`（OV2640→UXGA、OV3660→QXGA、
   OV5640→QSXGA…），**不手抄 PID 表**。换接传感器（含实戴与文档不符，如 seeed
   的 OV5640）候选表自适应；未知 PID 回退板级常数不放宽。
2. **board**（各板实测常数，唯一手工数字）：即上方矩阵。**禁止沿用姐妹板数值**；
   内存/热/DVP 都可能成为板级瓶颈，只有实测能定。
3. **memory**（运行时 fb 预算，只能收紧）：`宽×高/5 × fb_count + floor ≤
   可用 fb 内存域 + 当前 fb 足迹`（PSRAM 板查 SPIRAM、DRAM 板查内部 DMA 域）。
   防御内存退化态；luatos 稳态下该层独立复算出与板级常数一致的 VGA（floor=32K，
   按 PIT-012 实测校准），未来内存更宽裕的板型会自动放宽候选——届时仍须重测板级层。
- 可观测：`GET /api/camera` 新增 `res_cap_source`（sensor/board/memory）；
  AT+CAMRES 查询/报错同报来源；判定上限只看采集侧证据（见 n16r8 行）。
- 尺寸表纪律：各仓 camera_driver.c 内嵌 framesize→宽高表钉死 2.1.x 枚举序
  （`_Static_assert(FRAMESIZE_VGA==10)` 构建期防组件枚举漂移）。
- 顺带修复：n16r8 本地把 OV3660 PID 误记为 0x77（0x77 是 OV7725；组件检测
  0x3660）→ `camera_sensor_name()` 曾对实戴传感器返回 "unknown"，已改查组件表；
  同板 camera_init 曾硬拒 OV2640（换传感器即废），已改为传感器层自动收缩。

**n16r8 上限二次翻案（2026-09-05，SVGA→SXGA）**："模组/DVP 组合极限"结论两次
都错，真因是**配置复制缺行**：defaults 自称 VERBATIM from seeed 却漏抄
`SPIRAM_SPEED_80M`（实跑 40MHz）与 `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`。缺速→
XGA 时 cam_hal 16KB DMA 暂存缓冲在内部 DRAM malloc 失败（空闲块仅 15KB）=
"取帧死"；补齐后 XGA+ 仍 `NO-SOI/OVF` 帧损坏=第二层真因 XCLK 20MHz，降到
16MHz 后 XGA/HD/SXGA 全稳（SOF 实证），SXGA 投递 3.42fps（旧 0.4-0.8）。
**教训：①"verbatim"声明要逐行 diff 校验，缺行比错行更隐蔽；②"XX 组合极限"
归因前先读 cam_hal 的错误行（malloc failed≠DVP 时序）；③环境大变（自愈修复/
重置/换内存配置）后，旧实测结论值得重测。**

### PIT-022 NVS 键名 >15 字符令整次 config_save 失败 → "AT 关 AI 复活" → n16r8 误判 VGA-only（2026-09-04，n16r8）

**症状**：`AT+AIxxx=off` 当场生效（运行时），重启后 AI 全开；串口伴随
`E (…) config: Failed to write NVS key …`；连带 n16r8 分辨率实测全档被
"AI 强制 VGA"（camera_driver 加载钳制）污染，得出错误的"仅 VGA"结论。
**真因**：NVS 键名上限 **15 字符**。`"ai_motion_enable"`=16 →
`nvs_set_u8` 返回 `ESP_ERR_NVS_INVALID_NAME`；config_save() 全量写键
**遇错即返回**，其后所有键（ai_qr/rtsp/web_password…）永不落盘——
不止 AI，一切保存都悄悄失败。
**日志签名**：`config: Failed to write NVS key 'ai_motion_enable': …INVALID_NAME`；
`E config: Fai…`（截断样）；"关了又开"配置类问题 + NVS 写失败并现。
**修复**：键名缩至 ≤15（`ai_motion_en`）；帧尺寸校验由"单值等于"改
"区间上限"（原 `val != max` 锁死也挡住了复测）；AT+INFO 增加
`AI: face=motion=qr=` 行使持久化可核。已验证 n16r8。
**第二只狐狸**：改键表名后，`at_command.c`/`web_server.c` 里写键的
**字符串**（`config_set_bool_and_save("ai_motion_enable",…)`）仍是旧名 →
config_set 查表失败**静默丢弃**，运动检测再次"关不掉"。已把两处改齐，
并给 config_set 未知键加 WARN。教训：键名改表不改调用方 = 白改；
凡"字符串当键名"的接口，重命名必须全仓 grep。
**教训**：任何仓加 NVS 键先数字符；建议键表加
`_Static_assert(sizeof(key)<=16)`；"配置改了重启就没"先 grep NVS 写失败，
别急着怀疑 NVS 硬件/自愈逻辑。

### PIT-023 seeed WS 未掩码帧洪水 → httpd 打满 → TWDT 循环复位（19 次），连锁"SD 消失/录制自动恢复/85°C"（2026-09-04，seeed）

**症状**：网页反复掉线；SD 卡"突然没了"；用户停录制后过会儿"自己又开始录"；
reset_reason=6；芯片 85°C+。
**真因**：局域网某客户端向 /ws 持续 ~50Hz 发**未掩码帧**；ws_handler 收
协议违例仅摘自家常量后 `return ESP_OK` → httpd 保持会话 → 无限重入，
httpd 任务独占 CPU0 → IDLE0 饿死 → TWDT abort → 重启。重启后
①sdmmc_card_init 0x107 超时×5 且无重试路径 → SD "消失"；
②main 无条件 `recorder_start()` → 录制"自动恢复"；③CPU 满转助推 85°C。
**日志签名**：`httpd_ws_recv_frame: WS frame is not properly masked` 刷屏 +
`httpd_sock_err: error in recv : 128` + `task_wdt: … IDLE0 (CPU 0) / CPU 0: httpd`。
**修复**：协议违例/recv 失败一律 `return ESP_FAIL` 让 httpd 关会话；
WS connect 记录对端 IP（本次就因无 IP 记录查不到凶手）；sd_monitor 加
60s 周期 remount 重试；录制开机自启改为 `record_on_boot` 配置（默认 on）
且 SD 未就绪时不硬启。已验证 seeed。
**教训**：esp_http_server 的 WS handler，**任何协议违例都必须非 OK 返回**
（返回 ESP_OK = 告诉 httpd"连接很健康"）；"停止录像又自动开始"先查有没有
崩溃-重启循环，再怀疑业务逻辑；SPA 的 WS 是纯接收端（从不 send），
发坏帧的一定是别的客户端——连上就记 IP。

### PIT-024 luatos MJPEG 半开连接 send() 永久阻塞 → 客户端任务泄漏 → 堆耗尽 httpd 饿死 → 自愈误杀（2026-09-04，luatos）

**症状**：Web 时好时坏最终打不开；health 报 `Heap: ~20KB / Min Heap: 100`；
`mjpeg: Failed to create client task`；`httpd :80 probe failed (x/4)` → 自愈重启。
**真因**：无死客户端探测的 MJPEG 循环里，客户端异常消失（无 FIN）后
`send()` 阻塞在满窗口上永不返回 → 任务+栈+槽位永久泄漏
（17h 漏 24 个 ≈96KB，恰等于 HeapDelta）→ 堆见底 → httpd 无法 accept。
seeed 早有探测（recv MSG_DONTWAIT），**luatos 没同步** —— 家族修复不同步即坑。
**日志签名**：`grep -c "Stream client connected"` − `grep -c disconnected` 随时间增长；
`Min Heap` 持续下探；`httpd :80 probe failed` 周期性出现。
**修复**：家族同步死客户端探测 + accept 后 `SO_SNDTIMEO=10s` 兜底零窗口。
已同步四仓（探测 ai-thinker/n16r8 原有，SNDTIMEO 四仓本次齐补）。
**教训**：每个流式客户端任务都要有"死连接逃生门"；单仓修的家族性缺陷
必须当天同步评估其余仓（查同一函数在姐妹仓的形态），否则换个板子复发。

### PIT-025 n16r8 RTSP 会话线程创建抛 system_error 未捕获 → std::terminate 整机 abort（2026-09-04，n16r8）

**症状**：RTSP 客户端（NVR）一连上，板子整个 panic 重启（rst:0xc），
MJPEG 观众全断。
**真因**：资源紧张时 `RtspSession` 构造中 `std::thread` 创建抛
`std::system_error`，espp 库 accept 路径无 try/catch → 未捕获异常 →
`std::terminate` → abort。
**日志签名**：`abort() was called at PC …` 回溯含
`__cxa_throw → std::__throw_system_error → std::thread::_M_start_thread →
espp::Task::start → RtspSession::RtspSession → RtspServer::accept_task_function`。
**修复**：vendored `components/espp__rtsp/src/rtsp_server.cpp` 会话创建包
try/catch，失败记日志并拒绝该连接。已验证 n16r8。
**教训**：vendored C++ 组件的网络 accept 路径必查异常兜底——
资源耗尽时"拒绝一个连接"永远好过"整机崩溃"；abort 回溯第一帧就
addr2line，别凭 PC 猜。

### PIT-026 web_ui 新增文件不进固件（CMake file(GLOB) 只在 configure 求值）+ 采集器与 esptool 抢串口把板卡进 download 模式（2026-09-04，四仓）

**症状**：新加 favicon.svg 后 `idf.py build && flash` 一切"成功"，板上仍
404/回退 index.html；或烧录后板卡 `rst:0x15 (USB_UART_CHIP_RESET),
boot:0x0 (DOWNLOAD)… waiting for download` 卡死不启动。
**真因**：①各仓根 CMakeLists `file(GLOB MIBEE_UI_FILES main/web_ui/*)`
仅 configure 时展开——**新增**文件不触发重打包，spiffs.bin 悄悄缺文件且
flash 全程无告警（部分仓恰因别的触发重跑了 configure，四仓行为不一致、
极难排查）；②串口采集器持有 tty 时再 `idf.py flash`，esptool 与采集器
双开同一 tty，复位序列被搅乱 → 板进 ROM 下载模式。
**日志签名**：`build/spiffs.bin` 里 grep 不到新文件内容；串口见
`waiting for download` + 乱码。
**修复/纪律**：web_ui **新增**（非修改）文件后必须 `idf.py reconfigure`
再 build；烧录前先停对应串口采集器，烧完再拉起（本次 seeed 卡 download
即此因，重新单独烧录即恢复）。
**教训**："构建成功"≠"变更已入镜像"——对 GLOB 打包的目录，验证产物内容
（grep build 镜像）而不是只看退出码；任何占用串口的常驻进程都是烧录的天敌。

### PIT-027 真实凭据写进公开仓库（契约 v1.1 默认密码明文）→ 全家族历史重写清剿（2026-09-04，跨项目）

**症状**：用户发现家族统一 Web 默认密码（一个私人日期格式的真实口令）以明文
出现在 4 个 cam 仓的 `AGENTS.md`/`docs/api-contract.md`/C 源码字面量、mibee-docs
中英 API 文档、MiBeeSteward 测试用例、mibee-docs PR #5/#11 正文、MiBeeSteward
issue #332 正文。
**真因**：①四仓 `AGENTS.md` 实际**被 git 跟踪**（根 AGENTS.md 一直误记为
gitignored），把"本地知识库"当私有文件写入了凭据；②契约 v1.1（2026-09-03）
把默认密码当"产品公开信息"写进了契约文档与源码，未过"这是不是真实口令"这一关。
**日志签名**：`git log --all -S'REDACTED-PW'`；`git grep 'REDACTED-PW' $(git rev-list --all)`；
GitHub org 代码搜索 `gh api search/code q='"<串>" org:…'`。
**修复**（已验证于 4 cam 仓 + mibee-docs，全新克隆全 refs 0 命中）：
- 代码侧：Kconfig `MIBEE_CAM_DEFAULT_WEB_PASSWORD`（仓库默认占位 `changeme`），
  真实值只进各仓 gitignored `sdkconfig`（本地构建行为不变，四仓 build 验证过）；
- 历史侧：`git filter-repo --replace-text` + 全分支/tags 强推；
  mibee-docs 需临时放宽 main 分支保护（allow_force_pushes）推完立即恢复；
- GitHub 侧：PR/issue 正文用 API 编辑脱敏（编辑历史仍可回看，见下）。
**教训/操作要点**：
- 凭据永不入库；"文档说它是默认值"≠"可以公开"，真实口令一律 Kconfig+gitignored
  sdkconfig（真实值记录在根 AGENTS.md 本地机密清单）；
- filter-repo 三坑：①残留 `.git/filter-repo/already_ran` 超 1 天会交互确认
  （非交互 shell 直接 EOFError，删该目录重跑）；②每次运行都会**删掉 remote**，
  后续 push 前必须重新 `git remote add`；③**只重写本地分支**——多分支仓必须先把
  `origin/*` 全部物化为本地分支（`git branch -f x origin/x`）再重写，否则远程
  旧分支继续带毒（mibee-docs 10 分支踩过）；
- GitHub 残留面：release 二进制内嵌旧口令（等价泄露，靠轮换设备密码兜底）、
  dangling commit 按 SHA 仍可访问、PR refs（refs/pull/*）不随强推消失、
  PR/issue 正文编辑历史可回看——彻底清除需联系 GitHub Support GC；
- tag 强推会让 release 指向新提交，assets 不受影响（四仓已验证）。

**后续（2026-09-05 政策更新）**：对外公开默认密码统一为 `mibeecam2026`（Kconfig
默认值，可入文档，CI/发布固件自动带上；v6 与 v5.5.4 全新 configure 均已实测生效）。
本地自用值仍为 `REDACTED-PW`，仅存本机 gitignored `sdkconfig`。同时清掉了文档中
更早残留的 `admin` 默认密码示例（4 cam 仓 141+ 处、mibee-docs 140 处）——
"文档示例里的默认口令"也要跟着契约演进同步，否则又是另一种凭据漂移。

### PIT-028 IDLE1 "task not found" 洪水：运行期 esp_task_wdt_delete(IDLE1) 只删订阅条目不注销空闲钩子；"零静态引用"结论毁于编译器内联（2026-09-05，n16r8）

**症状**：`E (ms) task_wdt: esp_task_wdt_reset(707): task not found` 以 25~30Hz
（间歇 388Hz）刷屏，单次开机可持续 20 小时；板子其余功能正常（SXGA 流、API、
录像）。洪水与 `ai_pipe: AI task started` 仅差 ~11ms；`Removed IDLE1 from task
watchdog` 成功打印后毫秒级起爆。e19968a 移除 ai_pipeline 全部 TWDT 交互 +
espp__task 补丁后**依旧**，一度成为"静态不可能"悬案。

**真因**（链条，逐环实锤）：
1. ai_start_task 里 `esp_task_wdt_delete(xTaskGetIdleTaskHandleForCore(1))` 只删
   IDLE1 的**订阅条目**；IDF v6 task_wdt.c 官方退订是**两步**——
   `esp_deregister_freertos_idle_hook_for_cpu(idle_hook_cb, n)` +
   `esp_task_wdt_delete(...)`（task_wdt.c:286-287）。钩子被孤儿化后，IDLE1
   每轮空转照常调（被编译器**内联**进钩子的）esp_task_wdt_reset → 条目已删 →
   "task not found"（task_wdt.c:707 的 ESP_GOTO_ON_FALSE_ISR）。
2. 为何静态法证全灭：ELF 反汇编里对 `esp_task_wdt_reset` 符号零 call/零字面量
   引用（.debug 段除外）= 真，但调用走的是**内联副本**，外联符号成了无人引用的
   尸体——"零引用=零调用"在内联面前不成立。
3. 为何 wrap `esp_log`/`esp_log_write` 探针全沉默：IDF v6 该配置下
   `ESP_LOGE → ESP_EARLY_LOGE → esp_rom_printf`（esp_log.h:112/249，v1 模式
   ONE-PIECE 彩色格式串），ERROR 级日志根本不进 esp_log 族。挂对汇点
   `--wrap=esp_rom_printf` 后一发命中：`task=IDLE1 ra=0x…→
   esp_task_wdt_reset task_wdt.c:707`。

**日志签名**：`esp_task_wdt_reset(707): task not found` × 高频；起爆紧跟
`Removed IDLE1 from task watchdog (CPU-intensive AI task)`。

**修复**（n16r8 `680fb76`，上板验证洪水归零、SXGA 9.4fps 双订阅流正常）：
- `sdkconfig.defaults`：`CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=n`——构建期
  就不订阅 IDLE1（无条目+无钩子），这是官方姿势；gitignored `sdkconfig` 同步手改
  两行（含 `CONFIG_TASK_WDT_CHECK_IDLE_TASK_CPU1` 别名行）。
- ai_pipeline.cpp 删除整段运行期 delete(IDLE1) 代码。
- ai/seeed 的 `esp_task_wdt_delete(NULL)`（自退订）不受影响，未动。

**教训**：
- "CPU 密集任务让 core 空闲任务退出 TWDT"用 Kconfig（CHECK_IDLE_TASK_CPUx=n），
  不要运行期删 IDLE 条目——删不干净还留钩子。
- 内联会制造"符号零引用"假象；nm/objdump 零引用 ≠ 无人执行。运行期定性用
  `--wrap=<sink>` 链接期探针（打调用者 ra + pcTaskGetName），比 GDB-stub 轻。
- ESP-IDF v6 排日志去向先看 esp_log.h 的 `ESP_LOGE→ESP_EARLY_LOGE→
  esp_rom_printf` 映射：ERROR 级与 I/W 不同路（本配置下），挂探针要挂 rom 汇点。
- 诊断探针别 wrap ROM 的 `ets_printf`：启动早期（newlib 未就绪）被调到会挂死
  板子进 RTCWDT boot loop（本次实测翻车一次）。

### PIT-029 三层分辨率上限的 sensor 层用"初始化旗标"判活 → camera_init 中途钳制走了回退档，HD/SXGA/UXGA 全被压到回退值（2026-09-06，seeed 实锤 / ai-thinker 同型 / n16r8 正确姿势）

**症状**：seeed 配置 cam_framesize=13(HD)，实拍 JPEG SOF 却是 1024x768(XGA)；
boot 日志 `W camera: Requested res 13 exceeds effective max 12 (source: sensor), clamping`。
v0.4.0 时代就存在（配置 3=HD 实跑 XGA 的"标签 bug"其实是本坑）。
**真因**：`camera_init()` 在 `esp_camera_init()` 成功后、`s_initialized` 置位前调用
`camera_get_effective_max_res()` 做三层钳制；sensor 层以 `s_initialized` 为判活条件
→ 中途调用必然走"未初始化"回退档（seeed 回退 XGA=12）→ 恒 12 钳制。
ai-thinker 同型代码侥幸无恙只因其回退值恰为 BOARD_MAX。
**日志签名**：`exceeds effective max .* \(source: sensor\), clamping` 且上限值 =
该仓 sensor 层的回退常数（非组件表真值）。
**修复**：sensor 层判活改用 `esp_camera_sensor_get()` 句柄（句柄在
esp_camera_init 成功后即刻有效），不依赖自维护旗标。n16r8（agent 写的）天生
正确。已修 seeed（8474d66）+ ai-thinker（dd9dea8），四仓实机验证 HD 实拍 1280x720。
**教训**：三层上限是"启动中途也会调"的热路径，任何层判活都必须用运行时真源
（组件句柄/heap 查询），不能用"我先初始化完了"的自证旗标。

### PIT-030 ai-thinker 台架板 App 级串口 RX 硬件损坏：AT 全哑但 ROM/esptool 正常（2026-09-06，ai-thinker 单板）

**症状**：ttyUSB0 这块板：家族 AT 核心 TX 正常（boot 日志/+READY 都出），写指令
零反应（`AT`、`AT+REBOOT` 均无副作用）；烧旧固件时同样无 AT 响应。
**真因**：板级硬件个体问题——App 级 UART0 RX 通路坏；ROM 下载模式下 esptool
收发正常（auto-reset 电路 + ROM 同引脚），TX 正常，luatos/seeed 同核心正常。
**日志签名**：串口 TX 有日志、写任何字节无任何反应（连 REBOOT 都不触发）。
**修复**：无法软件修复。该板 AT 面不可用，部署/验证走 HTTP（Web OTA/curl）；
换板才能恢复串口面。
**教训**：AT 全哑先做三级判别：①TX 有无日志（无→查监听器/复位时序）
②写 AT+REBOOT 看副作用（无→RX 通路问题）③esptool 能否 sync（能→硬件 RX 好，
查 App 驱动；不能→整条串口坏）。勿先怀疑核心代码。

### PIT-031 串口工具三连坑：USB-JTAG CDC 的 readline 假异常/tcflush 假死 + 无参采集器默认口偷读 + 共口 ESP_LOG 拖慢 AT 应答（2026-09-06，seeed/跨项目）

**症状**：seeed（ttyACM0）上 at_console.py 全部指令 no-reply，手动裸读写却通；
luatos 偶发 `\r\r\n` 双回车。
**真因**：三因叠加：①pyserial `readline()` 在 USB-JTAG CDC 上逐字节读，每字节可
抛 "device reports readiness but no data" 假异常 → 一行永远凑不齐；②
`reset_input_buffer()`（tcflush）会让该 CDC 读写假死；③**无参运行的
overnight_log.py 默认口就是 /dev/ttyACM0**，与 AT 交互偷读竞争（pyserial 报
"multiple access" 字面为真）。另：AT 与 ESP_LOG 共口，SD 清理等日志洪峰可把
AT 应答推迟数秒。
**日志签名**：`device reports readiness to read but returned no data`；
`SerialException: device disconnected or multiple access on port`。
**修复**：at_console.py 改块读取自行分行 + 禁 tcflush + 开口预读 3s + OK/ERROR
终行耐心窗（ace0048）；排查时先 `lsof <port>` 灭掉偷读进程。
**教训**：USB-JTAG CDC（ttyACM* Espressif）不是普通串口——工具必须块读、禁
purge、单持有者；共口日志洪峰期做 AT 测试要先等洪峰过去。

### PIT-027 复发记录（2026-09-06）：本地默认密码再次回流公开仓 mibee-docs

PIT-027 清剿（filter-repo + 强推）之后，2026-09-05 新写的 espcam-api 镜像页
（commit 5c06356）把本地机密默认密码当"家族统一默认"再次写进中英文两页并已
推到 GitHub。**内容层已被两条路径分别修正**：远端 PR #16/#63ba6b3/#06e32e8
（脱敏 + 统一公开默认 mibeecam2026）与本地 v1.3 镜像刷新
（分支 `docs/contract-v1.3-sync` 3df8718，已推，待 PR 合并）；远端 PR #17 已加
合并前泄密扫描 CI 门禁（与本复发记录的建议一致）。**git 历史中该值仍在**
（5c06356 及其后续内容修正提交），是否再做一次 filter-repo + 强推由用户决断。
**教训**：机密清单（根 AGENTS.md）与公开文档的"默认密码"字段必须单一来源
（Kconfig 公开默认），任何人写文档提到默认密码时只允许出现公开值；CI 扫描
门禁（#17）防第三次回流。

### PIT-027 复发记录二（2026-09-06）：真实 SSID / 内网 IP 泄入 cam 仓公开文件；门禁落到四个固件仓

自建的泄密扫描器（mibee-docs security-check 移植版）首跑即在 **cam 仓本体**
命中 4 处已推送的真实泄密：seeed `docs/{en,zh}/api/config.md` 响应示例含真实
SSID `HomeAP-Alt` + 真实内网 IP `192.0.2.31`（WebDAV 示例 URL）；ai 与
seeed 的 AGENTS.md 事故记录里写进了真实家庭 SSID（`HomeAP-1`/`HomeAP-2`）。
内容层已脱敏（seeed e036fa0、ai ece19af）；**历史中仍在，filter-repo 由用户决断**。
**修复（机制层）**：`scripts/security-check.py` + `.github/workflows/
security-check.yml` 四仓字节一致部署（push/PR 触发），关键增强——只扫 git
跟踪文件（本地产物零噪音）、扫 `.c/.h/Kconfig.projbuild`（固件仓的源头是源码）、
日期形取值（YYYY-MM-DD，PIT-027 形态）显式可疑、"区间描述"豁免不再放行日期
密码、标识符/宏引用（`CONFIG_DEFAULT_AP_PASS`）豁免防误报。
**教训**：文档示例必须从保留段取值（SSID 用 `MyHomeWiFi`、IP 用 192.0.2.x），
真实环境值连"事故复盘"里都不能落库；门禁要部署在**产出内容的仓**，不只是文档仓。

### PIT-027 处置记录三（2026-09-06）：五仓全历史重写完成 + denylist 字面量出库

用户明确授权后执行（"解决git历史问题"）。**范围**：全历史扫描发现五仓全中——
seeed（SSID+内网 IP，最早 2026-05-09 的 9c79a54）、ai（AGENTS.md 事故记录里的
SSID）、luatos（2026-09-05 前曾误跟踪收集器日志，日志含 SSID+IP）、n16r8 与
mibee-docs（各自门禁 commit 的 denylist 字面量本身）+ mibee-docs 旧默认密码。
**前置修复**：扫描器 denylist 的真实值（SSID×2/网段×2/生产 IP）先改为
`MIBEE_SECURITY_DENYLIST` 环境变量注入（CI 走 repo secret `SECURITY_DENYLIST`，
五仓均已配置；cam 四仓 scripts/security-check.py 字节一致），否则重写完当场泄回。
**重写**：`git filter-repo --replace-text`（HomeAP-2 长串在前、裸 HomeAP-2 独立规则、
真实密码→`REDACTED-PW`、真实内网段→`192.0.2.0/24`、）+ 逐分支强推（mibee-docs main
临时放行 allow_force_pushes 后复原）。顺手清掉内容层漏网的裸 `HomeAP-2` 残留
（n16r8 AGENTS.md + camera_driver.h 注释、luatos AGENTS.md）。
**验证**：五仓普通克隆全历史复扫零命中；HEAD tree 哈希与重写前一致（n16r8/luatos
的 tree 变化即上述裸 HomeAP-2 清理，diff 逐行核对无意外）；CI 全绿。
**残留（GitHub 机制，本地不可清）**：① `refs/pull/*` 的 PR 存档引用仍指旧提交
（API 不可删）；② PR 页面 diff（如 mibee-docs #16/#17）显示净化前内容；③ 旧
commit SHA 的缓存视图。三者都只能走 GitHub Support 工单清除。**重写前的完整
历史备份（含机密）在 `~/Projects/esp-cam-backup-20260906-histpurge/`，本地敏感，
确认无误后可删**。其他机器上的旧克隆已与远端分叉，须重新克隆。
**工具坑**：zsh 不做无引号变量分词，批量 `git push $branches` 会把整串当一个
refspec（本批曾静默全败）；脚本推多 ref 用 `${=var}`。filter-repo 遇
`.git/filter-repo/already_ran`（上次清剿的标记）会交互询问，非交互管道要 `yes |`。

### PIT-027 处置记录四（2026-09-08）：门禁焊死——全分支 CI + pre-commit 钩子 + main 必需检查

**背景**：v1.4/v1.5 波次（ESPectre CSI/ONVIF 事件/GPL/gzip UI）整段以**未提交
工作树**存在——远端零提交、零 CI 暴露——期间 n16r8 AGENTS.md 再次写入裸
HomeAP-2（本地补扫抓获脱敏）。证明"push main 才触发"的门禁罩不住真实工作流。
**焊法（三层，五仓统一）**：① `security-check.yml` 触发域改为**任何分支 push +
任何 PR + 每日 cron 巡检**；② 五仓 main 分支保护把 `scan` 设为必需检查，cam
四仓 **enforce_admins=true**（管理员直推也拒，实测签名 `Required status
check "scan" is expected`）；mibee-docs 保留既有守门人审查政策上叠加 scan；
③ `.githooks/pre-commit`（`tools/setup-hooks.sh` 安装 core.hooksPath）提交前
扫 staged 文件——本批提交已实际被钩子拦扫过。
**代价（新工作流，全员/所有 agent 必须遵守）**：五仓 main 不再接受直推，一切
变更 = 分支 → push（CI 全分支触发）→ PR → scan 绿 → squash 合并（mibee-docs
另需审查批准）。
**残留盲区**：同 PR"改弱扫描器 + 泄密"的组合攻击只能靠 denylist 外置（repo
Secret）+ 跨仓 md5 纪律缓解；工作树阶段的回流只有本地钩子软防——提交前跑
`scripts/security-check.py` 应成为肌肉记忆。
**配套**：`tools/family_check.sh` v2 扩员——23 个单文件（新增 LICENSE/
.gitignore/csi_motion.h/compress_ui.py/setup-hooks.sh/门禁与钩子四件）+
espectre 整树哈希 + onvif_events 双板一致校验。

### PIT-032 gitignore 死规则压住过时的游离 AGENTS.md —— agent 指令污染源（2026-09-06，luatos/seeed）

**症状**：统一 .gitignore 删掉死规则 `AGENTS.md`（四仓顶层 AGENTS.md 早已
跟踪，该规则对跟踪文件无效）后，luatos/seeed 的 `main/AGENTS.md` 突然现形为
未跟踪文件——内容是数周前的目录级说明（"16 个端点/20 步启动"，pre-v1.3）。
**真因**：早期某次生成的目录级 AGENTS.md 从未入库，被全局 ignore 规则一路
压住；而 agent 工具会把任何名为 AGENTS.md 的文件当指令加载——过时副本等于
给未来的 agent 下假命令。
**修复**：两份游离副本已删除（luatos 5dbaaf5、seeed e036fa0）；统一版
.gitignore 四仓字节一致且不再含 AGENTS.md 规则。
**教训**：仓里出现"同目录树多处 AGENTS.md"时先 `git ls-files` 分辨跟踪态，
游离副本要么转正要么删除，绝不留双份；ignore 规则删掉后要立刻 `git status`
扫一遍现形的暗文件。

### PIT-033 SD 段轮换停顿 ~10s：每段全树孤儿扫描 + idx1 逐条 16B fwrite；轮询端点裸跑 f_getfree（2026-09-06，seeed 实测 / ai 镜像）

**症状**：seeed 每 300s 段轮换出现 ~10s 录像空窗（recorder "Segment complete"
→ 下一段 "Started" 差 10-11s）；/api/status、/metrics 每次轮询触发无缓存
f_getfree（自证注释 50-200ms/次的 SPI 停顿），SPA 秒级轮询下与录像写共享
总线；录像开启使推流吞吐掉 ~40%（4.06 vs 6.81fps @HD 2 客户端）。
**真因**：① `cleanup_orphan_zero_byte_files()` 在**每段**轮换后全递归
opendir/stat 扫整个 recordings 树（~288 文件/天 ×多天）；② idx1 索引逐条
16 字节 fwrite（2500-3000 条/段）；③ `storage_get_free_percent()` 绕过
`storage_get_info()` 已有的 60s 节流缓存 + 写/删增量计数器直接打 f_getfree。
ai 仓另有 health_monitor 每 10s 持锁 f_getfree——正是 AGENTS 记载"相机初始化
后会挂死"的那条路径（曾致看门狗复位）。
**日志签名**：`recorder: Segment complete` 与下一条 `recorder: Started` 相差
≥10s；`sd_cleanup_started free=25.x% threshold=20`（该行只是入口无条件日志，
free>low 时立即返回，勿误判为真清理）。
**修复**（seeed 4eb9abe 实测 / ai c60a837 构建验证待部署）：孤儿扫描节流到
≤1 次/小时（首转会跑一次）；idx1 攒 512 条/8KB PSRAM 缓冲批量 fwrite；
free_percent 改走 60s 节流缓存；SD 写队列 2→4；连续模式 fsync 10→30 帧。
**实测**：轮换空窗 **11.2s → 0.93s**（回调仅 +24ms），段长精确 300s，
frames_dropped=0。uploader 熔断本就存在（失败等 30s、连败 3 次暂停 5 分钟，
2026-09-02 事故产物）勿重复造。
**教训**：段边界是"顺手续务"的重灾区——任何全树扫描/元数据同步都不该挂
在轮换路径上；读侧遥测端点必须走缓存而不是裸 f_getfree。另：**性能 A/B 必须
同热状态同时段对照**——本次 fps 后段崩到 1.2-1.5 与固件无关（停录像同样烂），
是芯片温度 92→97.5°C 单调爬升 + 第二路真实消费者拉流的混杂；开机后 ~2 分钟
内的测量也不可用（启动全树缓存重建 + uploader 对死 NAS 重试都在跑）。

---

### PIT-034 ESPectre(CSI) SDK 嵌入 IDF v6.0.1 的适配链 + CSI 采集速率与链路匹配（2026-09-06，seeed 试点五轮 OTA 实测）

**症状**：vendored `components/espectre`（GPL-3.0-only，SDK 3.0.0，上游基于
IDF 5.5.x）在 v6.0.1 依次爆：① 配置期 `Failed to resolve component 'mqtt'`
（`esp_https_ota` 同）② 编译期 `mbedtls/sha256.h` 不存在→私有头无声明→
`WIFI_BW_HT20` 未声明 ③ 运行期 `Failed to enable CSI: ESP_FAIL`
④ CSI 使能后 `cal=0/1000` 永不推进、`packets=0`（内部生成器 tx=0 或
admitted 太低）。
**真因**：① v6 把 mqtt/esp_https_ota 移出核心组件；② mbed TLS 3.6 头移到
`mbedtls/private/` 且函数声明藏在新守卫宏 `MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS`
后面，一次性 `mbedtls_sha256()` 已删；带宽枚举改名 `WIFI_BW_HT20/40→WIFI_BW20/40`；
③ v6 新增 `CONFIG_ESP_WIFI_CSI_ENABLED`（默认关）把 CSI 从 esp_wifi 编译掉；
④ 三重叠加：(a) **v6.0.1 esp_wifi 驱动不填 `wifi_csi_info_t.payload/payload_len`**
（5.5.x 填），ESPectre 来源过滤器 `csi_frame_matches_traffic` 100% 拒绝
（diag 特征：`rej==cb`、cls=0）；(b) SDK 日志默认无 sink 全部丢弃，生成器
socket 卡死/发送失败一行都看不见；(c) 检测器时间栅格要 **70% 槽占用**
（7/10）才 `is_ready()`，系数按 100pps 拟合；拥塞网（ch11）回包衰减+成簇到达
（A-MPDU 聚合 → 同槽 excess 丢弃）实际 admitted 只有 ~8-20pps，100pps/20pps
栅格都永不就绪。
**日志签名**：`csi_motion: runtime fault: Failed to enable CSI: ESP_FAIL`；
心跳 `diag ... rej==cb`（来源过滤饿死）；`cal=0/N` 恒定 + `adm<70%×N`（占用不足）。
**修复**（全部在 seeed vendored 副本 + 集成模块，均有 MiBee patch 注释）：
① 组件 CMakeLists 删 `mqtt/esp_https_ota/improv` REQUIRES（可选组保持 OFF，
`idf_component.yml` 一开始就别 vendor——它的 improv git 依赖本网络拉不到）；
② sha256 改 context API + 组件 PRIVATE include 私有头目录 + 定义
`MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS`；③ `WIFI_BW_HT20/40→WIFI_BW20/40`
编译定义兼容；④ 版本戳打进 `runtime/espectre_sdk_version.h`（vendored 无 git
历史，`espectre_git_version.cmake` 会 FATAL）；⑤ `MIBEE_CSI_MOTION`
`select ESP_WIFI_CSI_ENABLED`（门关=固件与基线字节同尺寸）；⑥ 来源过滤旁路
（`csi_frame_matches_traffic→true`，等上游修 payload 再回退）；⑦ 装了
`espectre::set_log_sink`（W/E 级）SDK 报错才可见；⑧ `ESPECTRE_CSI_TARGET_PPS`
降到 10 匹配链路实际 admitted 率。
**实测**（五轮 Web OTA 迭代）：`calibration OK (thr=0.92)`、score 实时响应
（0.75/0.95 尖峰）、MJPEG 130s 并存零断连零重启、内部堆 52.4KB→30.4KB
（**-22KB**，远超 SDK 标称 1.8KB——运行时+流量任务+CSI 缓冲）、bin +76.5KB
（OTA 槽余 ~151KB）。**坑中坑**：改 `Kconfig.projbuild` 后 `idf.py build`
不重生成 sdkconfig，必须显式 `idf.py reconfigure`（select 不生效排查了半天）。
**教训**：vendor 基于 5.5.x 的 SDK 上 v6 先查四个差异面（组件迁移/mbedtls 3.6/
枚举改名/CSI 开关）；诊断顺序 = 先装日志 sink → 看
`diagnostics_sample()`（cb/cls/rej/acc/adm 五级速率定位断点）→ 别盲刷机；
pps 目标值不是灵敏度旋钮而是**链路预算**——先测 admitted 率再定栅格。

**四仓推广实录（2026-09-06 深夜，同日完成）**：组件改跨 IDF 版本安全
（BW 枚举/mbedtls 守卫包 `if(IDF_VERSION_MAJOR GREATER_EQUAL 6)`；mdns 移入
DIRECT 条件——ai 仓无 mdns 组件）。四板全部构建+上板+校准通过，但各挖出新坑：

- **n16r8（OTA 后两 bug 已修）**：① ESPectre CSI 策略应用会**异步重连 STA**，
  断连事件驱动的重连与之竞态，`wifi_manager.c` 里 `ESP_ERROR_CHECK
  (esp_wifi_set_config)` 状态错误直接 abort 重启（实测 line 226）——已改温和
  处理；② 感知任务在核 1（broadcaster+AI 都是 prio 5）上静默停摆——任务挪
  核 0 + 栈 6144→8192 后稳定。修复版 5min+ 连续心跳、校准 OK、4.99fps 并存
  零断连（ch2 好网 cb 高达 145pps）。**结论：完美可用**。
- **ai-thinker（初代 ESP32，两轮修复后判决=仅感知可用）**：CSI 硬件路径完全
  成立（自动选 **LLTF20** 档，S3 是 HT20；esp-csi#247 的 settle 规避上游已带），
  校准 OK；但 ① CSI 运行时早期启动把 lwIP 池打爆 → `:81` 监听 socket
  ENOBUFS(errno 105) → 推流全灭——`csi_motion_init()` 挪到 MJPEG 启动后修复；
  ② 修复后暴露真天花板：**初代 ESP32 任务栈/WiFi 缓冲只能住内部 RAM**（无
  SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY，WiFi-lwIP 迁 PSRAM 不生效），CSI 一口
  吃掉 ~100KB 内部堆（S3 只 -22KB），free internal 只剩 ~5KB → MJPEG 客户端
  任务（4KB 栈）创建失败。**感知✓ 推流✗，不能并存**。
- **luatos（无 PSRAM，判决=仅感知可用）**：v5.5.4 跨版本构建/运行全通（版本
  守卫生效）、USB 烧录、校准 OK(thr=0.43)；但 free_heap 26.8KB、
  **min_heap=108B**（启动期距堆死 108 字节），推流并存崩（60s 6 断连）。
  **感知✓ 推流✗，堆预算不过关**。
- **seeed**：71min+ 连续运行、38 次 MOTION（含 score=1.00 饱和命中）。
- **物理运动验证**：seeed 38 次 / n16r8 2 次 / luatos 2 次 / ai 0 次（运行窗
  短）——CSI 检测的是真人走动，非流量伪迹。
- **通用**：csi_motion 任务参数按板分叉（n16r8 核 0；其余核 1）；四仓
  `CONFIG_ESPECTRE_CSI_TARGET_PPS=10`（ai -70dBm 弱链路下占用率间歇达标，
  校准慢但能完成）。
- **救活实验双阴性（2026-09-06 深夜追加；2026-09-07 补充定案）**：① ai 上
  `SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`（S3 家族配方）在初代 ESP32 是**毒药**——
  全系统任务创建失败（csi/onvif/motion 皆挂、httpd 探针奔自愈重启），已回滚
  并在 defaults 留禁用注释。回滚后"同尺寸二进制仍连续启动失败"的**台架硬件
  归因是错的**——真凶是 **22:26 被中断的增量构建污染了 build 目录**（后续
  增量构建产出隐性损坏的固件：开机相机后所有任务创建失败；`fullclean` 干净
  重建后同一配置完整启动、CSI 85 秒完成校准 thr=0.47）。**教训升级：idf.py
  build 被中断（OOM/手动 kill）后，下一次产物不可信，必须 fullclean。**
  ② luatos 关 WS+mDNS（Kconfig 门）**省不出运行时堆**（这些是惰性分配：
  实测 20KB 反低于 26.8KB 基线、min_heap 1380B，34.8KB 接客线无望）——不裁
  WiFi RX 缓冲这类伤推流的深水区，无解；已恢复产品配置。两板判决维持
  ⚠️仅感知（ai 干净构建复测：开门 CSI✓ + 推流仍 138 断连/0 帧——内部堆
  23KB 天花板真实；free_heap−free_psram 减法受采样错位影响不可靠，以
  /metrics node_memory_MemFree 为准）。
- **luatos 阈值退化观察（2026-09-07）**：其校准收敛出 thr=0.00 → MOTION
  数秒级翻转（一夜 1108 次）——弱链路低占用率下 Lightweight 校准可能退化，
  调参候选：换不拥塞信道后重校准/提高 pps/换 High-Accuracy 档。

---

### PIT-035 四板常驻 USB 的"偶发连不上"排查 + 台架整改（2026-09-07，全台架）

**症状**：用户视角四板 USB 偶发连不上；另有 ai 板跨上电复位持续启动失败。
**排查结论（按命中概率排序）**：
1. **采集器占口 = "Device or resource busy"（最常见，设计行为）**：四口
   常年被 overnight_log.py 持有（CH340/CH343 裸开即复位的陷阱防护）。手动
   连任何口必失败——正确姿势=读 `tools/overnight_serial.log`。
2. **USB autosuspend 全开（真凶之一）**：四个串口设备 `power/control=on`
   声明 98-138mA 但 autosup=on，空闲挂起后偶发打开卡顿/首包丢。修复需
   root：udev 规则 `ACTION=="add", SUBSYSTEM=="usb-serial", ATTR{power/control}="on"`
   或 `usbcore.autosuspend=-1` 内核参数。
3. **两只同型号 CH340 无序列号 → by-id 碰撞**：仅一条 by-id 软链；若同时
   重插，ttyUSB0/1 可能对调（采集器按旧名会读到错的板）。**已整改**：
   采集器+soak 守护改绑 `/dev/serial/by-path/pci-0000:00:14.0-usb-0:X`
   （物理口稳定；n16r8=0:3.2、ai=0:4、luatos=0:3.3、seeed=0:1）。
4. **hub 供电健康（排除）**：VIA 2109 自供电 hub（bMaxPower=0），整夜零
   断连/零过流；hub 上另挂 RTL8153 千兆网卡（宿主机有线网），480Mbps 下
   串口流量可忽略。早先"hub 电流冲击"怀疑对当前台架不成立。
5. **内核模块匹配（排除当下）**：运行内核 == 已安装（7.2.2），pacman
   陷阱未激活；历史教训仍在册（内核更新不重启 → ch341 绑不上）。
6. **宿主 OOM 次生灾害**：并行 ≥3 个 idf 构建会把 8G 内存打爆（2026-09-06
   21:46 Xorg 被 OOM 杀）——构建必须串行或限并发。
**日志签名**：`fuser /dev/ttyUSB0` 显示 PID=采集器（busy 类）；
`cat /sys/bus/usb/devices/*/power/control` 全 `on`（autosuspend 类）。
**教训**：多板台架先绑 by-path 再谈稳定；"连不上"先 `fuser` 再 `journalctl -k`，
最后才怀疑硬件。附：pkill -f 的模式若出现在自己命令行里会自杀（本轮实踩，
后续 kill 用显式 PID）。

---

### PIT-036 IDF v6 Websocket 推送静默失效 + req 悬垂崩溃（2026-09-07，seeed/v6 板）

**症状**：浏览器 `/ws` 握手 101 成功，但**任何 WS 事件都永远收不到**
（motion/recording/wifi_state/csi_status 全灭）；前端表现为"看不到 CSI/运动
推送内容"。修复第一版（握手回调入册后仍存 `httpd_req_t*` 广播）首发即
`Guru Meditation Core 1 panic (LoadProhibited)` 重启。
**日志签名**：整个日志历史**零** `WS client added`（客户端列表恒空）；
崩溃版签名 `httpd_ws: httpd_ws_check_req: Argument is null` → 紧接 Guru。
**根因（两层，都在 IDF v6.0.1 esp_http_server）**：
1. **v6 起 WebSocket 握手请求不再调用 uri handler**（`httpd_uri.c` 握手分支
   显式注释 "do not call the uri->handler"，直接 return OK）——v5.x 时代
   "握手后以 GET 进 handler 做客户端入册"的模式在 v6 永不执行 →
   `ws_broadcast` 里 `s_client_count==0` 早退，**推送整体静默失效**。
   v6 迁移后 seeed 的 WS 事件从未到达过任何浏览器（2026-09-0x 引入，
   2026-09-07 才发现）。
2. **握手期捕获的 `httpd_req_t*` 在回调返回后即失效**——存下来跨任务
   `httpd_ws_send_frame` = 悬垂指针 → LoadProhibited。v5 恰好不踩（内部
   结构寿命不同），属"在 v5 上碰巧能跑"的反模式。
**修复（seeed `main/ws_server.c` 重写，三件套）**：
1. 入册挂 `ws_post_handshake_cb`（Kconfig `CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT=y`，
   已入 sdkconfig.defaults + gitignored sdkconfig）；v5 的 GET 分支保留，
   `add_client` 按 fd 幂等去重（双路径安全）。
2. 客户端跟踪**只存 fd**，广播/收割统一 `httpd_ws_send_frame_async(server, fd, frame)`
   （内部 `httpd_sess_get` 同步校验，死 fd 返回 ESP_ERR_INVALID_ARG 不崩，
   顺带完成死连接摘除）。
3. 独立 `s_send_mutex` 串行化所有 socket 写（csi_motion 1Hz 心跳 × 30s 收割
   PING 不再可能交错写同一 socket）。
**验证**：标准库裸 WS 探针（`/tmp/ws_probe.py`）10s 实收 10 帧 `csi_status`
（1Hz，score 实时跳变），探针断开后设备不重启、无 Guru。
**范围**：仅 seeed（v6 + websocket:true）。luatos（v5.5.4）GET 分支照常、
不受影响；n16r8/ai 无 WS。**luatos 若升 IDF v6 必须套用同款重写。**
**遗留小瑕疵**：post_handshake 回调里 `getpeername` 得 `ip=0.0.0.0`（v6
回调时机下 peer 地址未填），溯源降级为仅 fd；待后续核对 v6 时机再修。

---

### PIT-037 ai Web OTA 必崩：mjpeg listen 任务悬垂句柄 × vTaskDelete（2026-09-07，ai-thinker）

**症状**：`POST /api/ota/upload` 固件字节全部收完（弱链路 1.3MB×3 次均传完）
后 100% 崩溃重启，curl 侧表现为 HTTP 000；修复送不进去（鸡生蛋：修 OTA 的
补丁只能经 OTA/USB 送达）。
**日志签名**：`ota_updater: Quiescing system for OTA flash write...` 紧跟
`Guru Meditation Error: Core X panic'ed (LoadProhibited)`；addr2line 落在
`uxListRemove ← mjpeg_streamer_stop (mjpeg_streamer.c:434) ← ota_quiesce_system`。
**真因**：listen 任务自行退出时（`vTaskDelete(NULL)`）**不清全局句柄**
`s_listen_task`；弱链路下 listen socket 早已出错退出 → 句柄悬垂 →
`mjpeg_streamer_stop()` 固定等 200ms 后盲删悬垂句柄 → FreeRTOS 链表操作
踩已释放 TCB。
**修复（ai `main/mjpeg_streamer.c`）**：① listen 任务退出路径先
`s_listen_task = NULL` 再自删；② stop() 改为轮询等待任务自清（≤1s），
仅句柄仍非 NULL（任务真卡死、TCB 仍有效）才强制删除。**注意修复本身
经 USB 烧录送达**（旧固件上 OTA 不可用）。同款"存任务句柄 + 盲删"模式
值得在其他仓 stop 路径复扫。
**教训**：任务自删前必须清全局句柄；stop 侧永远不要对可能已自退的任务
盲 vTaskDelete——轮询自清 + 超时强删才是安全姿势。

**教训**：任务自删前必须清全局句柄；stop 侧永远不要对可能已自退的任务
盲 vTaskDelete——轮询自清 + 超时强删才是安全姿势。

---

### PIT-038 ai/luatos CSI 模式下 Web UI 不可服务的分层根因与修复（2026-09-08）

**症状**：两板开 CSI 后 Web UI 大资产（app.js 62KB）0-4KB 处死 / 131B/s
爬行 / 浏览器只出静态壳（app.js 未执行，net-dot 无类）；小 JSON 时好时坏；
luatos 周期性 httpd 全瘫到自愈重启。
**根因（四层叠加，逐层剥洋葱）**：
1. **外部锤击（放大器）**：192.0.2.30（有线、开 SSH、RTT 0.9ms 的
   NVR 类盒子）以 1-2s 间隔自动重连两板 :81 流（ai 6283 次/4 天、
   luatos 同款）。弱板服务不了流 → 查看端重试 → 空口/堆被 churn 穿
   → 更服务不了 → **自维持失败循环**。健康板（seeed -48dBm/n16r8）能
   稳定供流故无此循环。溯源靠流 accept 路径新加的 `inet_ntop` 对端日志。
2. **lwIP 发送缓冲搁浅（漏）**：中途夭折的传输把整只 `TCP_SND_BUF`
   钉进重传黑洞（curl 超时走人，设备对着黑洞重传到 MAXRTX），实测一条
   死连接搁浅 >10KB → luatos heap 掉到 min 200B → `send:113/104` 风暴。
3. **httpd 槽位饥饿（浏览器专属）**：默认 `max_open_sockets=7`（含 3
   内部保留=客户端仅 4 槽）+ luatos `send_wait_timeout=30`：浏览器首屏
   6 并发 + WS + 健康自探测必超限 → i18n.js/app.js 随机夭折 → SPA 永久
   静态壳（curl 单连接测永远发现不了）。
4. **射频环境（残留，固件无解）**：luatos 关联 ch2 网格节点呈 ~10s 周期
   射频失聪振荡（cb=0.0 间歇归零，PS 已证 NONE、motion/锤击逐一排除）；
   ai 弱链路 -61~-79dBm + 漫游尝试。两板好窗内一切正常（luatos 曾
   7/8 全量、ai 17KB/0.16s），坏窗内连 status 都死。
**修复（全部已部署）**：
- 家族级 gzip：`tools/compress_ui.py` 产物 26%（app.js 62K→17K）+
  静态 handler `.gz` 协商（seeed 原有休眠支持直接激活；ai/luatos/n16r8
  补齐；`.gitignore` 四仓加 `main/web_ui/*.gz`）。
- ai/luatos 流服务：accept 记对端 IP + **同 IP 3s 重连护栏**（不建槽
  不分配）+ **CSI 门控下不起 :81**（判决=仅感知，流本就不可用；端口
  关闭让查看端秒收 RST，风暴止步）。
- ai/luatos lwIP 防搁浅：`TCP_SND_BUF_DEFAULT=5760`（4×MSS）+
  `MAXRTX/SYNMAXRTX=6`（死连接 ~1 分钟拆除）。
- ai/luatos httpd：`max_open_sockets=10`、`send_wait_timeout=5`。
- SPA 自愈加载（index.html，四仓 md5 同步）：脚本按依赖顺序加载、
  失败 2s 重试×15——页面可穿透死窗自动收敛，健康板零影响。
**附带战果**：ai Web OTA 在新镜像上三轮全通（PIT-037 修复实锤验证）。
**事故记录（自己的坑）**：ai 的流禁用第一版拦错了函数——`mjpeg_streamer_init()`
只建互斥锁，真起监听的是 `mjpeg_stream_server_start(81)`（两处：WiFi 连上
回调 + AP 路径）。拦了 init 没拦 start → 监听任务带着 NULL 互斥锁跑 →
`assert xQueueSemaphoreTake queue.c:1709` 崩溃循环（.30 每次连接触发）。
**教训：动"禁用某服务"前先分清 init/start/ensure 三类调用；改完必须看
启动串口日志确认签名行出现。**
**遗留（用户侧动作）**：
1. 处置 192.0.2.30 的自动重连查看端（NVR？）——治本；
2. luatos 的 ~10s 射频失聪振荡疑其关联的网格 AP 节点（ch2）不稳：
   挪板/锁节点/换信道后重测；ai 同理（-79dBm 漫游目标）。
3. httpd 会话缓冲在紧堆板建议后续评估 PSRAM 化（ai 有 PSRAM）。

### PIT-038 补遗二（2026-09-08 下午）：政策反转（摄像头优先）+ 锤击护栏 v2 + 测试端也须退避

**政策（用户拍板）**：摄像头能力优先、CSI 其次——本轮全量 CSI 只是"能否并存、
解决有人才拍摄"的试验。ai/luatos 互斥判决定稿 → 两板回退 CSI-off 生产固件
（`CONFIG_MIBEE_CSI_MOTION=n` 重建；ai 经 Web OTA 翻 ota_0，luatos 经 USB）。
验证：两板 `csi_motion` 能力位消失、相机管线正常（ai capture 0.5-0.8s/堆
3.9MB；luatos capture 正常/free 42.5KB）、:81 流服务恢复。seeed/n16r8 保持
CSI 常开（与推流完美并存，承载"有人才拍摄"试验）。soak 期望矩阵同步
（推流四板=True，CSI 仅 seeed/n16r8 要求存活）。
**护栏 v2（单 peer 互洗漏洞）**：v1 只追踪"最近放行的那个 IP"——锤子与真实
观众交替接入时每个新 IP 都重置追踪对象，锤子借观众的接入穿透，单槽位板真实
观众 1-2s 被踢一次（实测探针+锤子乒乓、30s 0 帧而护栏零拦截）；v1 还有
"放行即武装 10s 冷却"，会把被踢观众的 ~7s SPA 自愈重连也锁出去。v2
（ai/luatos `mjpeg_streamer.c` 同款）：4 项每-IP 独立退避表（环替换）+
**按接入间隔判定**——同 IP 两次接入 <5s 视为锤击 → 503 + 10s 翻倍封顶
300s，正常观众（≥7s 重连）不受影响。新日志签名：`Hammer guard:
rejected N from <IP> (backoff Xs)`。实测两板 .30 均被钉至 300s/会话。
**补遗三（2026-09-08 晚，MiBeeNvr#711 对账发现）**：护栏与对端退避梯子会
**互锁**——窗口内任何再撞续期 300s 窗口，而对端（NVR PR #712）梯子封顶
60s < 300s ⇒ 永久 503（实测拒计数 20901 仍爬）。修复：503 带
`Retry-After: <冷却秒数>` 头（ai 已上板实测，luatos 源码就绪待下次烧录），
客户端照做等过窗口即可重新入内；顺手修 Content-Length 23→22。
**教训（测试端自己当了锤子）**：`tools/mjpeg_probe.py` 的 0.3s 重连环同样
是 <5s 间隔——被设备新护栏 503 钉死（94 rejects/30s），且钉死窗口内每次
再撞自动续期 +300s，出现"等过窗口还连不上"（实为又撞了续期）。测试端已改
6s 退避（四仓 md5 一致同步）。**规则：任何自动重连的客户端都应带 ≥5s
退避，否则它就是锤子。**
**附带发现（OTA 断链竞态）**：弱链路下 curl 可能已收到 200 状态行、随即
吃 RST（设备写完镜像抢先重启）——curl 报 56 但镜像已完整写入并翻转。判据：
`/api/ota/info` 的 `running_partition` 翻转 + uptime 归零；勿盲目重传。

### PIT-038 补遗四（2026-09-08 晚）：自愈加载器杀死 SPA boot（四板全量回归，已修复）

**症状**：PIT-038 加固上线的自愈加载器（index.html 动态注入 i18n.js/app.js）
部署后，四板 Web UI"能打开但全是静态壳"——`Caps` 恒空、WS 不连、CSI pill
永不显示、统计条不填充、能力驱动面板全隐藏；控制台零报错（静默不执行）。
**真因**：动态 `<script>` 注入不阻塞解析，app.js 到达时 `DOMContentLoaded`
早已触发——`document.addEventListener('DOMContentLoaded', boot)` 注册在
事件发生之后，**回调永不执行**。自愈加载器上线前的静态 `<script src>` 写法
阻塞解析、必先于 DCL 执行，故无此问题。上一轮"行隐藏之谜"（手动调
loadConfig 才显示）即此 bug 的早期信号，当时误判为缓存竞态放过了。
**修复**：app.js boot 改标准双路引导——`readyState==='loading'` 才挂 DCL
监听，否则立即执行（四仓 md5 同步 + gzip 重压 + SPIFFS 重刷：seeed/n16r8/
ai 走 OTA spiffs，luatos 走 USB 单分区写 0x392000）。实测四板 Caps 填充、
seeed WS OPEN + CSI pill 实时显示、n16r8/ai 控件渲染设备值。
**教训：把同步 `<script>` 改成动态注入时，任何依赖 DOMContentLoaded 的
入口都必须改双路引导；四仓共享 SPA 改动后浏览器验收必须查"Caps 是否
填充/WS 是否连"，不能只看静态壳渲染。**

### PIT-039 ai 板"web+NVR 双失效"四层根因：AMPDU 黑洞 + 护栏锁死 + 内部 RAM 碎片化 + 漫游扫描（2026-09-08 深夜，ai-thinker）

**症状**：用户报"web 和 NVR 都没法用"。实测四象限：SPA 大资产（app.js 62KB/17KB gz）
0-9KB 处搁浅 20-30s 或 RST，而 /api/status 30-50ms 秒回、i18n.js 25KB 好窗 0.17s；
:81 推流全天 0 fps（soak fps=0 连续 20 轮）；NVR(192.0.2.30) 被护栏累计拒 2 万+ 次；
每次开机 4s 内 `motion_detect: Failed to create motion detection task`，流客户端任务
~50% 创建失败；httpd(8KB 栈) 开机创建是竞态（败→健康自愈 SW_CPU_RESET 循环）。

**根因（四层叠加 + 两个坑中坑，全部 2026-09-08 深夜定位并修复）**：
1. **AMPDU 聚合重传黑洞（头号）**：弱链（-69dBm HT20）+ AMPDU TX/RX 开启 →
   聚合帧反复重传，任何 >~6KB 的批量流填满一个 TCP_SND_BUF(5760) 后零进展
   （搁浅点反复恰好 6072B=8×MSS=一个发送窗口），小帧照常穿过、ping 正常、
   RTT 却从 30ms 飙到 1.8-2.5s。搁浅会话在设备侧表现为 `httpd_sock_err:
   send/recv 113 (EHOSTUNREACH)` 僵尸 + 每 10s 重试。修复：n16r8 配方
   `CONFIG_ESP_WIFI_AMPDU_TX/RX_ENABLED=n`（sdkconfig.defaults 落档）。
2. **护栏 v2 续期锁死**：退避窗口内**任何**再撞都续期+翻倍（封顶 300s）→
   重连间隔 <300s 的合法客户端（NVR 15s 梯子）永久 503。修复：只有**新的
   <5s 违规**才续期；在窗但守规矩的重连被拒不续期，窗口自然过期即重新入内
   （ai 已上板 + luatos 源码同步待烧）。实测修复后 NVR 数分钟内重新占流。
3. **内部 RAM 碎片化 + 运行期任务栈**：MALLOC_CAP_INTERNAL 常态 ~27KB 且无
   4KB 连续块（`/api/status` 口径 free_heap-free_psram≈10KB 更低）。修复组合：
   流 worker/listen 任务持久化 + xTaskCreateStatic（listen→长度 1 队列递 fd，
   零运行期任务创建）；motion 任务 xTaskCreateStatic；motion 30KB ΣΔ 网格
   `.bss`→PSRAM 运行期分配（DRAM 最大连续区 +30KB）；`SPIRAM_MALLOC_ALWAYSINTERNAL`
   16384→3072（4-16KB 冷缓冲让去 PSRAM）；httpd 启动 5×2s 重试。修后 internal
   开机 59KB，httpd/motion/stream 任务 100% 创建成功。
   **坑中坑 A（httpd 栈红线）**：`config.stack_size` 不能低于 8192——
   `handler_static` 栈上局部 ~5.3KB（buf[4096]+filepath[560]+gzpath[576]），
   6144 实测栈溢出：静态文件 0B/6.77s 精确复现 RST + 连环 SW_CPU_RESET。
   **坑中坑 B（CSI-off stub 漏函数）**：csi_motion.cpp `#else` stub 只写了
   init，漏 `csi_motion_get_status`（web_server.c 无条件调用）——门关时链接
   必炸；今晨"CSI-off 构建"靠 build/ 里陈旧 sdkconfig.h（CONFIG 仍 =1）糊过。
   已补 stub。**坑中坑 C（stray give）**：mjpeg listen 任务 `s_client_count++`
   后跟一个无配对 Take 的 `xSemaphoreGive(s_mutex)`——可在他任务持锁时放锁，
   互斥锁失效。已在重写中移除（luatos 版本无此问题）。
4. **全信道漫游扫描**：`channel=0`（全信道）扫对端每 60s 离信道 ~2.9s，
   在途 TCP 批量流的重传黑洞共犯。修复：连续无益扫描指数退避（60s→…→900s
   封顶；实测 "unprofitable x4 — backing off to 900s"），链路较上次扫描恶化
   ≥6dB 或真漫游成功即重置。

**验证（2026-09-08 23:47 固件，USB 交付——httpd 挂死时 OTA 通道不可用）**：
app.js gz 17520B×3 次 0.23-0.33s（修复前 0-9KB/12-25s）；/api/capture 21KB
0.27s（修复前 19-30s）；mjpeg 探针 30s 128 帧 4.2fps@76KB/s（全天 0 fps）；
soak cycle98 fps 5.06；NVR 持续占流 4min+；开机 httpd/motion/stream 任务
全起；无 SW_CPU_RESET（修复前每 2-4 分钟一次）。

**教训**：① 弱链路板"小请求通、大流量死、搁浅点≈整发送窗口"先关 AMPDU
（n16r8 配方，家族弱链板默认）；② 紧堆板常驻任务用静态栈，运行期才分配
4KB+ 连续块等于掷骰子；③ 退避类护栏的续期条件必须区分"在窗"与"新违规"，
否则任何间隔短于封顶值的合法重连器都被永久锁死；④ 改 sdkconfig 后必须
全量重链（stale sdkconfig.h 能让门禁形同虚设）；⑤ `#if` 门控模块的 `#else`
stub 要覆盖头文件全部公开函数，链接器是最后的手。

**延伸实锤（2026-09-09，luatos）**：ai 的四层配方 24 小时内在 luatos 复现
判决——症状变体为"分钟级 TX 全楔死"（ping 与 TCP 同生共死，连 226B 小响应
都断，区别于 ai 的"小帧通大流量死"）。判别链：AMPDU 开@GT 隔墙网 20 请求
8/20 → AMPDU 开@HomeAP-2（1 米、n16r8 同位完美）仍 8/20（失聪跟板走，网络
无罪）→ **AMPDU 关@HomeAP-2 20/20 零失败**、NVR 流会话 16-30s→2min+、
PIT-040 失聪计数零触发。luatos 2026-09-03 的"驱动 stall 史"即此坑前身。
教训 ① 升级：弱链/紧堆板 AMPDU 黑洞有两种表型（ai=搁浅型、luatos=全楔型），
判别法=换网对照 + ping/TCP 同死观测，**别按信号强度排查，按板子排查**。

### PIT-043 改 SPA 忘跑 compress_ui.py → 设备静默服务旧 UI（2026-09-09，家族）

**症状**：luatos 双 WiFi 上板后，前端配置页只显示一个 WiFi（备用 SSID/密码
字段缺失）。`GET /api/config` 明明返回 `wifi_ssid_2`，三块姐妹板 UI 正常。
**真因**：`main/web_ui/*.gz` 是 gitignored 构建产物 + **纯手工生成**。昨晚
23:00 改了 app.js 后只重压了 seeed/n16r8 两仓，ai/luatos 漏跑——今晨刷机把
22:08 的旧 gz 打进 SPIFFS，gzip 静态服务优先吐旧内容。family_check 对此
失明（gz 不入 git，不在 23 文件集）。
**日志签名**：`curl 设备/app.js | md5sum` ≠ 仓内 `main/web_ui/app.js` 的
md5（PIT-017 的 UI 验证步骤本可拦住，这次刷机后没做）。
**修复**：① 四仓重新生成 gz（luatos 重刷后 UI 恢复，`/app.js` md5 与源一致）；
② **压缩步骤焊进构建**：根 CMakeLists `add_custom_command` 跑
`tools/compress_ui.py`，gz 与源文件同入 spiffs `DEPENDS`（Ninja 保序），
v5.5.4（luatos）/v6.0.1（ai）双侧验证 `[1/7] gzip → [6/7] spiffsgen`。
四仓提交：luatos 04846f2 / ai 8cdaf77 / seeed 4744334 / n16r8 6d55293。
**教训**：凡是「源文件 + 手工生成的伴生文件 + 构建打包伴生文件」的结构，
生成步骤必须进构建系统；UI 变更交付后必验 `/app.js` md5 == 仓内源。

### PIT-042 错误 WiFi 密码伪装成射频硬件故障——3.5h 全员误诊实录（2026-09-09，luatos）

**症状**：luatos 双 WiFi 移植交付后，"NVR 能录但 web 打不开"恶化为完全掉线；
3.5h 内 ~200 次关联尝试零成功，两网皆死：HomeAP-2（1m）reason=2/205/201（认证段），
GT（5m 隔墙）reason=15（EAPOL 四次握手）；三块姐妹板同 AP 全部正常。自愈链
每 9:07 重启一轮，固件零崩溃——一切证据都指向"板端射频前端/PA 渐进劣化"。

**真因**：**主槽（slot0）密码在当天上午的槽位演练中被写坏**（wrong PMK）。
错误密码在本路由器家族上的表型是 reason=2/15 混合——**"认证段失败≠密码问题"
的经验法则在此被证伪**。09:47:30 最后一次成功关联后 5 分钟内凭据被覆盖写入，
从那一刻起两网全部失败；当天下午换了 4 版固件（移植版/20dBm/11B/改MAC）+
回刷移植前基线全部同样失败——因为所有版本读的是**同一份坏掉的 NVS**。

**日志签名**：
- 确定性 100% 失败（~200 连试零成功、每次死在同一阶段）——纯射频噪声做不到，
  错 PMK / 拉黑 / 死硬件才会；
- `auth -> assoc → assoc -> run → 2~3s → run -> init (0xf00/0x2c0)` + reason=15
  = 错误 PMK 的完整形态（管理帧全过、密钥交换必死）；
- 同信噪比下姐妹板（ai 甚至 -64dBm）正常 = AP 侧无罪；
- AT 查槽位显示 `pass:****`（非空）——**掩码让坏密码和好密码不可分辨**。

**修复**：v4 抢救固件做"密码交叉验证"——连 HomeAP-2 时借用 slot1 的原装密码
（密码不离开设备、不打印），1 秒连上；GOT_IP 时自动把好密码回填进坏槽
（`slot0 pass REPAIRED from other slot`），NVS 修复持久化。回填后撤掉全部
抢救补丁、重刷干净移植固件：boot pick → 1 秒 connected → .148 原 IP 回网。

**教训**：
1. **"改完就坏"报告的第一嫌疑人永远是当晚写过的持久化状态（NVS/配置），
   其次才是代码**。A/B 刷回旧固件只隔离了代码变量，隔离不了数据变量——
   旧固件读同一份 NVS，坏配置下 A/B 结果"完全相同"反而是配置问题的暗示
   （两版代码不可能以相同方式同时坏）。
2. 密码正确性无法从掩码/日志判定，唯一可靠测试=交叉验证（借另一槽/另一板
   的已知好凭据试连）。家族双网同密码时，槽间互借是零成本判决实验。
3. reason 码 folklore 要让位于对照实验：本例 reason=2（"应为 AP 无响应"）
   实际由错 PMK 触发。判据组合（确定性 × 阶段 × 姐妹板对照）> 单个 reason 码。
4. 串口采集器按 4096B 块写日志，块内行**没有行首时间戳**——`^\[时间` 锚定的
   grep 全部低估（本例断连直方图一度只看到 6 条，真实 155 条）。统计前先做
   chunk-aware 解析（续行继承块时间戳）。
5. `pkill -f <pattern>` 会匹配到**自己的命令行**导致静默自杀，长链命令莫名
   无输出时先查这个；规避=`[.]` 类正则破坏自匹配。

**遗留**：当晚 luatos 在推流下堆仍会缓慢衰减（-92KB/3h，Min 132B 后掉流）——
与本案无关的独立老问题，堆治理另案跟进。

### PIT-041 双 WiFi 移植的三个上下文陷阱（2026-09-09，luatos→家族）

**背景**：n16r8 双 WiFi 配方移植到 luatos/ai（契约 AT v1.2，AT+WIFI2 三板
登记）。luatos 上板过程 09:18-09:50 连踩三坑，全部已修已验证。
**坑 1（静默楔死）**：重构版把 `esp_wifi_set_config` 挪到 `esp_wifi_start()`
之后 + DHCP 盲区定时器回调直接在 esp_timer 任务里调
`wifi_start_sta`/`esp_wifi_stop`——开机关联掉线后设备卡死在
"已关联无 IP、无重试无日志"（AT 口活着，health 只报 Connected/Connecting）。
**修复**：连接路径恢复移植前逐字节原版（config 在 start 前）；开机择优改
**独立扫描会话**（起栈→扫→停，不触碰凭据）；盲区回调只做
`esp_wifi_disconnect()`，切换交给事件路径计败驱动。
**坑 2（悬空旗标吞掉真实掉线）**：`s_expected_disconnect` 在对**未启动的
radio** 调 `esp_wifi_stop()` 时被置位（不产生断开事件→永不消费）→ 下一次
**真实**掉线被当自致断开吞掉，不重试。修复：双判据
`旗标 || reason==WIFI_REASON_ASSOC_LEAVE(8)`（自致断开的稳定签名），
且旗标只在 stop 返回 OK（radio 在跑）时置位。
**坑 3（切换风暴后 connect 201）**：频繁 stop/切网后 FAST 扫描缓存过期 →
connect 报 201 NO_AP_FOUND。修复：`sta.scan_method=WIFI_ALL_CHANNEL_SCAN`
（慢 ~2s，弱链板正确性优先）。
**附带（PIT-040 门补洞）**：web 迟迟不起（STA 卡 CONNECTING 的楔死态，
服务全延迟启动）超 5min 也走失聪升级链（force_reassoc×3 → 重启）——
否则健康门在楔死态永久静默，而旧固件的"误杀重启"反而是唯一自愈。
**日志签名**：`expected disconnect (self-initiated, reason=N) — not counting`
出现在真实掉线时刻；`boot pick: 'A' -XdBm vs 'B' -XdBm → …`；`connect
failures — switching to X network (switch N/6)`。
**教训**：①移植 WiFi 逻辑时连接路径的 API 顺序（config 在 start 前后）不是
等价细节；②esp_timer/小栈回调里禁止调重 API（stop/start/全套配置），只做
轻动作让事件路径接力；③"自致断开"识别必须用 reason 码兜底旗标；④楔死态
的兜底必须独立于"服务已就绪"的假设。
**移植判据参考**：boot pick ≥8dB 换网/平局保持 last_net；2 败切网；6 次上限
转 AP；实测 24s 内从掉线到换网拿 IP。
**环境注记**：2026-09-09 ~08:53 起台架射频对 luatos 转劣（两网关联均 2-3s
掉，三姐妹板同 AP 正常推流，旧固件同症状）——非移植引入，soak 持续观察。



### PIT-040 luatos health 自愈探针把"射频失聪窗"误译成整机重启（2026-09-09，luatos）

**症状**：luatos 2026-09-08 晚"NVR 采集正常但 web 打不开"，且一夜连环重启
（21:46/22:49/…）。NVR 反而正常——单条长连 MJPEG 靠 TCP 重传+6s 自动重连
扛过坏窗；web 每次加载要新建多条连接拉多文件，对 ~10s 失聪振荡零容忍。
**真因链**（PIT-038 遗留#2 射频振荡的放大器）：失聪窗里 localhost :80 探针
一起超时（httpd 会话被搁浅发送占满/tcpip 拥塞，堆瞬时 3~6KB）+ 每 boot
假失败种子（health Step 7 先于 web_server 启动，首轮探测必失败白送 1/4）
→ 4/4 → esp_restart。重启治不了射频，反而打断 NVR 流，越重启越乱。
**日志签名**：`httpd :80 probe failed (N/4)` 连发后重启横幅，伴
`httpd_sock_err: error in send : 11/104`、`ping_sock: create ping task failed`。
**修复（已上板 2026-09-09 00:57）**：①探针在 httpd 未起
（`web_server_get_handle()==NULL`）时不计（boot 假种子根除）；②探测失败先
ping 网关旁证（esp_ping 2×16B/300ms/800ms，任务栈显式 2048——默认 ≈3KB
在失聪窗 3~6KB 堆上建不起来）：链路活→才累计 httpd 罪名（4 次重启，原
行为）；链路失聪→不计，连续 3 次（≈90s+ 持续失聪）`wifi_manager_force_
reassoc()` 强制重联（disconnect reason=8 → 10s 自动重连/主网 3 败切备用网，
成功重连即清零，不会误切）；③升级阀：连续 3 次强制重联换不来一次探测
成功 → 回落重启兜底（防"httpd 真瘫被误判失聪"永久重联）。
**实测**：00:43-00:57 坏窗全链路实录——deaf 1/3→3/3 → `forcing STA
re-association` → 10s 重连 → NVR 流回槽，**零重启**（旧固件同窗必重启）；
新 boot 全程无假种子行。
**教训**：①自愈探针必须区分"服务死"与"链路聋"——WiFi 状态机 connected
≠链路可用，ping 网关是判别器；②boot 顺序里晚于探针启动的服务，探针必须
gate 在其 ready 标志上；③失聪判定只做轻恢复（重联），重启只留给"链路
活着仍瘫"或"重联也无救"的组合。附记：esptool 烧录校验通过但 RTS
hard-reset 偶尔不生效（镜像已写入、板子不重启继续跑旧镜像）——判据
uptime 未归零，兜底用 AT+REBOOT 共口注入（采集器持口时可写）或重新上电。


### PIT-034 补遗二（2026-09-07 深夜，待观察）：seeed ESPectre 内部线程重建 abort

v1.4 固件上 seeed 出现 2 次自发性 abort（22:46/22:49，间隔 3.5min，此后停）：
`E pthread: Failed to create task!` → `abort()`。回溯落在
`std::thread ← espp::Task ← ESPectre`（SDK 内部采样线程死亡后重建失败；
堆充足 6.6MB，疑内部 RAM 碎片或线程表瞬时耗尽）。**不在 ws_server/
csi_status 改动路径上**（回栈无家族代码帧）。日志史仅此 2 次，先挂 soak
观察复发规律再深挖；若复发，查 espectre runtime 的 sampler 生命周期。

### PIT-034 补遗（2026-09-07 晚）：CSI 开启下 ai/luatos 连 Web UI 都不可服务

四板全量部署 v1.4（capabilities `csi_motion` + SPA CSI 指示 + luatos WS 桥）
时实测：**ai 与 luatos 在 CSI 门控开启 + 有活跃流客户端时，Web UI 的
大资产（app.js ~62KB）传输在 0~4KB 处必死**（`httpd_sock_err: send 104/113`，
len=0 或截断）；index.html（~4KB）时好时坏。堆证据：luatos
`heap 8964 < 34816` 拒新流客户端、min_heap 128B、ESPectre TrafficGen ping
ENOMEM；ai `mjpeg_streamer: Failed to create client task`（15:00 起某查看端
以 1-2s 间隔持续重连拉流，6283 次/4 天）。
**结论**："ai/luatos = 仅感知"判决的外延——CSI 开启时这两板不仅推流不可
用，**Web UI 本身也不可用**（小 API 调用可活）；n16r8/seeed（PSRAM 充裕）
UI+CSI 并存无碍。luatos 的 WS csi_status 桥本身工作正常（裸探针 8s 9 帧），
只是 UI 加载不出来。外部流查看端（用户的播放器/NVR 自动重连）会把这两板
钉死在堆死边缘——测试时要先断开查看端。

---

### PIT-040 CSI 替代 ΣΔ 运动拍照：warm-up 同帧陷阱 + 门开链接坑 + 保存后画廊空（2026-09-09，ai-thinker）

**症状**：CSI 运动拍照链首夜实测三连：①暗场"闪光照片"13.5KB 纯暗帧，且
判定到落盘仅 468ms（闪光预热 3 帧 @2fps 理论 ≥1.5s）；②`CONFIG_MIBEE_CSI_MOTION=y`
构建链接炸（undefined reference to `csi_motion_get_status`，与 PIT-039 坑中坑
B 同型——门开分支又漏了，上次是门关 stub 漏）；③任一照片保存后 `/api/files`
画廊空到重启。

**真因**：① `frame_broker_get_copy()` **恒立即返回当前帧**——"丢弃 3 帧预热"
丢的是同一帧的 3 个引用、抓的也是闪光点亮**前**的旧帧（ΣΔ 时代即存在，只到
暗场自动闪光场景才暴露字节级证据）；② ai 仓 `csi_motion.cpp` 门开分支从未
配过快照（PIT-038 时只做了日志试点）；③ 保存路径整体丢弃列表缓存
（`storage_invalidate_list_cache` 语义），而 GPIO14 共享使相机运行期
`opendir` 不可靠（AGENTS 既有记载）→ 缓存无法重建。

**日志签名**：`Motion detected! (auto-flash) (scene DARK)` 后紧接
`storage: Saved photo ... (13522 bytes)`（无 `Dark scene — flash photo` /
预热间隔）；对照修复后 36.9-46.3KB。链接期：`undefined reference to csi_motion_get_status`。

**修复**（全部已在 ai-thinker 上板验证，2026-09-09）：
1. broker 增发布序号：`broker_frame_t.gen` + `frame_broker_current_gen()` +
   `frame_broker_get_copy_after(gen_floor,...)`——闪光前记 `gen0`，等
   `gen0+3` 后的第 4 帧才抓（帧驱动预热语义为真）；配套 `frame_broker_boost(ms)`
   把拍照窗口空闲帧率 2→5fps（闪光点亮时长 ~0.8s，2fps 兜底 ~2s，超时 3s）。
2. `csi_motion.cpp` 门开分支移植 seeed 快照（portMUX 单写者，状态转移回调
   **也落快照**——只靠 ~1Hz 周期更新会漏采 <1s 的 MOTION 片段）。
3. 保存→pending 数组（portMUX）→ `storage_get_photo_list_json()` 持锁并入
   缓存：新照片免重启可见且带字节数（GPIO14 局限下的唯一实时可见途径）。
4. 架构层：`CONFIG_MIBEE_CSI_MOTION=y` 时 ΣΔ 逐帧解码与其 ~60KB **内部**
   工作缓冲整体退役（`motion_detect.c` CSI 分支：250ms 快照轮询 + 三级暗场
   判定：缓存<120s 直用 / 录制中单帧 luma 兜底 / 按需锁定曝光探针——NVR
   常驻观看不再饿死暗场缓存）；main.c 解除 PIT-038 的 CSI/:81 互斥门
   （净内部 RAM 为正，流/CSI/拍照三线并存实测稳定）。

**验证**：修复前 08:12=13.5KB（暗帧）；修复后 08:57=36.9KB、09:08=44.3KB、
09:11=46.3KB、09:52=44.4KB（自然暗场 4%），与手动闪光对照（41-46KB）吻合；
6h soak 见当日 report（soak/csi_photo*）。

**教训**：①"丢弃 N 帧再抓"类逻辑必须以**发布序号/时间戳**为界，broker 的
get_copy 语义是"最新"不是"新"；② 门控模块两分支的公共函数清单要对着公共
头文件逐个核对（两分支都会漏，且各自只在对面形态炸）；③ 关键链路证据不能
只靠串口——本次采集器连丢三行 motion 日志差点误诊为"任务死锁"，是
/api/files 盘上清单+事件计数器还原了真相；④ GPIO14 板上照片**内容**运行期
读不出（fread 200+0B），验证成片要用字节数（列表 API）而非下载。

**附**：当日 09:57-10:00 三次 `rst:0x1 POWERON_RESET`（硬件域：电源跌落/EN/
人为断电），均不与闪光/负载事件相关、无崩溃签名，每次自愈——固件无关，
留意供电/是否有人在动设备。

**坑中坑 ⑤（同日 16 时补录）：SPIFFS 的 app.js.gz 旧版**。用户报"前端看不到
CSI"二次复发——设备上**明文 app.js 是新的、gz 是旧的**（v1.6 前版本，CSI 胶囊
只认 WS 心跳无轮询回退；ai 无 WS → "无事件则 UI 全静默"，正是旧 app.js 注释
写的情形）。验证陷阱：curl 不带 `Accept-Encoding: gzip` 核对的是明文（md5 与
仓库一致，看似无害）——**浏览器永远带 gzip 头拿的是另一份文件；核对 Web UI
资产必须 `-H "Accept-Encoding: gzip"` 解压比对**。修复：`compress_ui.py` 重生成
gz → 重建 spiffs.bin → `/api/ota/spiffs` 上传。流程红线 compress_ui.py docstring
早有（"Run after editing any web_ui file"），09-08 的 v1.6 四板部署漏跑了 ai 的
gz 重生成。

### PIT-044 第二台 N16R8 单元（CH343）无法 USB 自动进 download：DTR=摁复位 / RTS=杀USB（2026-09-09，n16r8）

**症状**：对第二台 N16R8 单元（CH343 桥，by-id `1a86_USB_Serial_5ABA098589`，ttyACM1，OV5640）执行
`idf.py -p /dev/ttyACM1 flash`：esptool 卡 `Connecting.......` 后
`SerialException: device reports readiness to read but returned no data`。换 `--before no-reset`、
手工 DTR/RTS 进模式时序、ioctl 层拦截"清 RTS"全部无效。旁观察：AT 控制台"完全静默"，
但 DTR 拉高后芯片立刻正常启动（`rst:0x1 (POWERON), boot:0x8`）。

**真因**：该单元 CH343 控制线接法特殊（与 luatos 的 CH343、老单元的 CH340 都不同）：
**DTR=低 → EN 被持续摁在复位**（电平保持，不是脉冲）；DTR=高 → 释放并正常启动；
**RTS=低 → CH343 的 USB 立即掉线重枚举**（疑板级电源/复位耦合）；**IO0 未接任何自动下载
电路** → 任何 DTR/RTS 组合都进不了 download 模式。esptool 经典复位时序在此板上等价于
"摁住复位 + 杀 USB"。另：芯片任何复位（含 AT+REBOOT）都连带 USB 重枚举一次。

**日志签名**：
```
esptool: Connecting....... → device reports readiness to read but returned no data
pyserial: s.rts = False  → 同款 SerialException（秒杀 USB）
pyserial: s.dtr = True   → 1s 内 ESP-ROM:esp32s3-20210327 ... rst:0x1 (POWERON),boot:0x8
```

**修复（2026-09-11 回填，issue #11）**：板载第二个 USB-C 实测为 **ESP32-S3 原生
USB-OTG**（枚举 `Espressif_USB_JTAG…80:B5:4E:C2:BE:5C`），原生口
`esptool --before default-reset write-flash @flash_args` **一次成功**（~45s 全镜像
+校验）——**这块板刷机永远走原生口，CH343 口只当串口控制台**；BOOT+RST 手动路径
未再需要。诊断口诀：
**串口静默 ≠ download 模式**——先做 DTR 保持实验（False 8s / True 8s 看有无 banner）
分清"被摁在复位 / 正在启动 / ROM 在等"三态；注意本固件控制台要到 boot 后 ~9s
（fbroadcast 首条）才有输出，几秒的静默读窗口全是误导。

**教训**：同型号板不同批次，USB 桥与控制线可完全不同（老单元 CH340/ttyUSB1 vs
新单元 CH343/ttyACM1）；同是 CH343 也有两种接法（luatos 开口即复位 vs 本板 DTR
反相保持 + RTS 杀 USB）。esptool 连不上先做控制线三态实验，别怀疑工具链；
换板先 `ls /dev/serial/by-id/` 认序列号，别认 tty 名。

### PIT-045 泄密扫描器扩展名盲区：sdkconfig.defaults 注释里的真实 AP 标识逃过门禁（2026-09-09，luatos）

**症状**：luatos PR #7/#8 CI `scan` 连败，报 `AGENTS.md:109: [泄露标识] HomeAP-2`
（AMPDU 判别实验记录把真实 AP 名写进了文档）。修复时复查发现
`sdkconfig.defaults` 63/67 行注释里**同款标识未被 CI 命中**——不是没泄，是没扫到。

**真因**：`scripts/security-check.py` 只扫 `SCAN_EXTS`/`SCAN_FILENAMES` 白名单，
`.defaults` 扩展名不在内 → tracked 配置注释成为泄密盲区。本次是 AGENTS.md（.md
在清单内）先败露才带出 defaults 的同款。

**修复**：4 处全改"HomeAP 系主/副节点"泛称（luatos `540b6c7`/`4e7e2d8`，两分支同修）。
**遗留**：把 `sdkconfig.defaults`（及 `.defaults` 系）加进 SCAN_FILENAMES 需动
md5 锁定的门禁本体，应走四仓同步 PR 由守门人审查，勿单仓私改。

**教训**：写实验记录/配置注释时真实 SSID、网段一律泛称——denylist 只兜底，不挡
"新发明的写法"；本地提交前用根工作区 AGENTS.md 机密清单导出
`MIBEE_SECURITY_DENYLIST` 跑一遍 `scripts/security-check.py`（pre-commit 钩子
未配 env 时 A/B/C 规则为空，等于没扫最关键的三条）。


### PIT-046 CSI 无人室误报风暴：Lightweight settle 阈值单边崩塌 + 自家流量污染（2026-09-09，seeed→家族）

**症状**：seeed 置于无人房间仍持续报 CSI MOTION（每小时 200-400 次状态翻转，
soak 三天累计 4628 次；同网 n16r8 仅 397）。SPA 胶囊/WS motion 事件/ONVIF
MotionAlarm 全部跟着扇出。

**真因**（三层叠加，按因果序）：
1. **Lightweight 检测器的 settle 机制只降不升**（`observe_settled_level_`：
   12 块×20 评估取块最大值中位数−2.7 logit，仅下调）。无人室的安静块喂给它
   "会话更安静"的证据，几小时内把校准阈值（0.38-0.92）单调压到 ~0.02——
   环境本底噪声高于阈值，`metric > thr` 反复成立。luatos PIT-034 补遗的
   thr=0.00 一夜 1108 次是同机制极端形态。
2. **自家流量污染 CSI 序列**：PIT-034 修复⑥旁路来源过滤（v6 驱动不填
   payload）后，板上 MJPEG 推流/API/WS 流量的 CSI 全进检测器。diag 实锤
   `tx=9.9 cb=54.7 rej=0`——突发流量与 ping 竞争占时间栅格槽，turbulence
   统计被时序抖动污染 → metric 0.00↔1.00 满幅方波。
3. **CSI 是 device-free sensing，无人房间 ≠ 射频静止**：邻室走动/风扇/
   窗帘/邻居设备/AP 信道切换（本次 ch11→ch7）都改写多径。阈值健康时是
   无害微扰，阈值 0.02 时全是"运动"。

**日志签名**：`csi_motion: motion=... thr=0.02`（恒定低位）；`score` 在
0.00/1.00 二值跳；心跳 `diag ... rej=0.0` + `cb≫tx`；`calibration OK
(thr=0.38)` 后数小时 thr 轨迹单调下滑（串口日志 grep `thr=` 按小时分桶）。

**诊断口诀**：误报先查三样——/api/status 的 `csi.thr` 水位（<0.10 即崩塌）、
`csi.flip_rate`（>60/h 即风暴）、`csi.cb_pps vs tx_pps` 比值（≫2 即污染）。

**修复**（契约 v1.7 调参面 + 自愈环，2026-09-09，seeed 实测验证）：
1. **阈值锁定根治开关**：`csi_threshold` 键（0=自动；0.05-1.0=手动锁定，
   SDK `set_threshold` 的 manual override **同时禁用 settle 下调**）——
   seeed 实测锁 0.30 即时生效、恢复 0 触发重校准后 thr=0.704。
2. **自愈环**（`csi_auto_heal` 默认开）：thr<0.10 或翻转>60/h 持续 5min →
   重校准（30min 冷却）；冷却窗内二次退化 → 锁定 0.15；link_channel 变化 →
   重校准（10min 冷却）。手动锁定期间让位用户。
3. **可观测面**：快照/WS 心跳增补 profile/thr_locked/calibrating/flip_rate/
   tx·cb·adm pps；SPA CSI 调参卡（阈值滑杆/hits/检测档/重校准按钮/spark
   曲线）；AT+CSI?/AT+CSICAL；`POST /api/csi/calibrate` 端点。
4. 四仓同步：seeed/n16r8 完整实现（n16r8 无 WS，唯一通道 /api/status）；
   ai/luatos 键族存储无害（CSI-off 板）。ai 板 2026-09-09 反转 CSI 生产门开
   （PIT-040），调参面直接适用。

**教训**：自适应阈值检测器的"单向漂移"是系统性缺陷——安静环境喂出的
"更安静"证据会追到噪声底以下；一切自适应阈值必须有下限钳制或翻转率
哨兵。CSI 误报≠有人在，先看阈值水位再怀疑物理世界。

### PIT-047 AI IDE 状态目录误入库：.zcode 无 ignore 规则 + .omo 早于规则被强加（2026-09-10，seeed/luatos→五仓）

**症状**：seeed 仓 `git ls-files` 出现 `.zcode/plans/plan-sess_*.md`（ZCode 会话
计划文档）、luatos 仓出现 `.omo/evidence/task-*.log`×20（oh-my-openagent 任务
证据）——均已推到 GitHub 并**进了各自 `origin/main` 历史**；luatos 已发布 tag
v0.3.0 也包含 c54dad8（seeed 的 v0.1.0~v0.4.0 全部干净；初判曾把 tag 归错仓，
处置时已纠正——教训：`git tag --contains` 两个仓连跑时输出要逐行对回命令）。

**真因**：两个独立漏洞叠加——① 家族 .gitignore 有 `.omo/`/`.codegraph/` 但
**没有 `.zcode/`**，seeed 的计划文档毫无拦截（2026-09-04 随 4683546 入库）；
② luatos 的 evidence log 是 2026-06-17 与 `.omo/` 规则**同一天**被显式
`git add -f`（或先 stage 后补规则）绕过（c54dad8）——**ignore 只拦未跟踪文件，
已跟踪状态在 ls-files 里一直续命**。泄密闸（PIT-027）只扫密钥/网段/标识，
对 AI 工具状态文件天然失明（本次已复查确认内容无泄密）。

**日志签名**：`git ls-files | grep -E '\.(zcode|omo)'` 非空；GitHub 仓库页出现
`plans/`/`evidence/` 路径。

**修复**（2026-09-10）：五仓 .gitignore 增补 AI IDE 黑名单段（`.zcode/ .claude/
.cursor/ .cursorrules .continue/ .aider* .windsurf/ .kiro/ .zed/ .copilot*
.gemini/ .goose/ .roo/ .clinerules CLAUDE.md GEMINI.md`；四 cam 仓字节一致
md5 4348→新值过 family_check，mibee-docs 原先**没有 .gitignore**，一并新建走
PR #25）；luatos/seeed 分支 HEAD `git rm -r --cached` 清出并推入进行中 PR
（luatos #9 / seeed #13，历史清理后最终头 `887d63e`/`6c62dcb`）。

**处置记录（2026-09-11，全历史清除完成）**：用户拍板后执行。luatos：filter-repo
`--invert-paths --path .omo` 一次重写 7 分支（90 提交）+ 续跑补重写 tag v0.3.0
（`0a71473`→`33df310`，tagger/message 保留；v0.1.0/v0.2.0/v0.2.2 不受影响）；
seeed：`--path .zcode` 重写 7 分支（209 提交，tag 本就干净无需动）。main 保护经
API 临时开窗（enforce_admins/force-push 放开、scan 检查保留），force-push 全部
分支+tag 后立即恢复并 GET 复核与原状一致。两仓 `origin/main` 及全部分支历史对
目标路径 0 命中；本地 reflog expire + gc 后旧提交（c54dad8/4683546）对象已不
存在。重写前完整备份：`~/Projects/esp-cam-backup-20260911-aipurge/`（两仓 git
bundle + 远端快照 + 保护规则 JSON + PIT-027 期 filter-repo 旧状态；本地自留，
确认无误后可删）。**残留**：旧提交在 GitHub 侧仍可经 SHA 直读（API 200）——
refs/pull/* 与缓存残留，已无 ref 指向；彻底抹除需 Support 工单（PIT-027 先例）。

**教训**：AI 工具状态目录（.zcode/.omo/.codegraph 等）会随"顺手 add -f /
先 stage 后补规则"混进仓库，且跟踪状态续命肉眼难察——新工具进场先补
.gitignore 再干活；周期性 `git ls-files | grep -E '^\.(zcode|omo|codegraph|claude|cursor)'`
自查。

---

### PIT-048 n16r8 内部堆地板定性 + HEAP_TASK_TRACKING 诊断构建自毁（2026-09-11，n16r8→家族）

**症状**：n16r8 稳态内部空闲堆仅 7~27KB（`free_heap−free_psram`），最低触
6.8KB；espectre TrafficGen ping 频发 `errno=12`（生产 4.6h 实测 258 次）；偶发
`pthread/xTaskCreate` 失败。两台单元（旧板/新板 CH343）地板一致 → **基线行为，
非新板回归**。为定位开 `CONFIG_HEAP_TASK_TRACKING=y` 诊断构建后：OTA 落板即
陷入 5.5s/次的 rst:0xc 连环（panic 文本被 USB 重枚举盲区吞掉，实为 **Core 0
INT-WDT panic 循环**），幸存启动 RTSP 任务创建失败（`E rtsp: Failed to create
video feed task`），17min 后再入 panic 风暴，Web OTA 恢复失败（上传连接被打断）。

**归因**（heap-task-tracking 实测表，2026-09-11 issue #11 专项）：内部堆总量
~320KB 五区；占用大头 = **wifi 任务 133KB + tcpip 任务 15→98KB 动态（NVR/流
负载驱动）+ ipc/静态区 ~90KB + 任务栈 ~40KB**；main 名下 1.67KB~1.67MB 主要
在 PSRAM（camera fb/DMA）。大 DRAM 区（231KB）被 ~1000 个小块切成碎片，
`largest_free_block` 低至 1.5KB——`SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 令所有
≤16KB 分配走内部堆是碎片驱动因素（PSRAM 空着 5.8MB+）。

**日志签名**：`ping send failed (errno=12` 持续刷；诊断构建特征
`Failed to create video feed task` + 密集 `rst:0xc` 每 ~6s；`Guru Meditation:
Core 0 panic'ed (Interrupt wdt timeout on CPU0)`（串口抓到时）。

**修复/处置**：① 生产固件经原生 USB-OTG 口 `idf.py flash` 整镜像恢复（OTA 在
panic 风暴中不可用，符合 USB 例外条款；恢复后 RTSP/护栏正常，panic 零复发）；
② 诊断钩子以 `#if CONFIG_HEAP_TASK_TRACKING` 包裹合入（n16r8 `d9d5a36`，生产
构建零开销；开启方法见提交信息）；③ 缓解方向（未实施，家族决策）：调低
`SPIRAM_MALLOC_ALWAYSINTERNAL`、lwIP pbuf/窗口调优、任务栈审计。

**教训**：在内部堆地板 <30KB 的板上开 HEAP_TASK_TRACKING 是自毁——每块
alloc 元数据（~16-32B×1000 块≈16-32KB）恰好吃掉最后余量，启动期任务创建
风暴下直接 INT-WDT panic 循环，且 OTA 救不回来（httpd 也死）。诊断构建刷入
前先确认有 USB 恢复通道在手；观察这类板子的串口优先走原生 USB-JTAG 口
（免 open-reset、panic 文本不易被枚举盲区吞）。errno=12 ping 失败在该板为
已知基线劣化（CSI 靠环境流量分类仍工作，pkts≈10/interval），不是新故障。

**补遗（2026-09-12 晨，缓解实验数据 + OTA 坏窗截断签名）**：
① `SPIRAM_MALLOC_ALWAYSINTERNAL` 16384→2048 单变量实验（仅本地 gitignored
sdkconfig，USB 刷入）：净空窗 errno=12 从 ~3.7 次/min 降到 **0**，内部空闲
25.1→25.9KB，内部 fps 15.6→15.8 无回退；同窗 A/B 对照交付吞吐两配置无差别
（射频坏窗主导）——**数据支持调低，待其它板复测+长 soak 后家族决策**。
② 射频坏窗里 Web OTA 会**静默截断**：设备日志只有 `OTA upload: N bytes to
partition 'x'` 而无 `OTA upload complete` = 传输中途断流（curl 侧空响应，
易误判成功）；上传速率 ~19KB/s 的窗别走 OTA，直接 USB。

---

### PIT-049 SPA 长开标签页自锤 :81 锤击护栏：90s 强制换流 × 3s 看门狗 → 300s 退避终身锁（2026-09-12，家族）

**症状**：无头浏览器标签页整夜开着设备 SPA，设备日志显示同一来源 IP 以
~80-90s 节奏 accept :81 流、穿插 `Hammer guard: rejected`，累计被拒 351 次
仍不停；伴随 errno=12 ping 失败率从 ~0.06/min 恶化到 ~33/min（churn 驱动
pbuf 分配压力）。设备本身零重启、稳定。

**机理**：app.js 看门狗（3s 采样停帧即重连）与 90s 无条件换流（`img.src`
换新流）互相踩踏：换流瞬间旧连接关闭→新 accept 与上次同 IP accept 间隔可
<5s → PIT-038 补遗二护栏判锤 → 503 + 指数退避；SPA 遭 503 后看门狗很快
再试 → 退避翻到 300s 封顶 → 标签页进入永久"重试→被拒"循环，把设备钉在
socket churn 上（对 CSI 运动"有人才拍摄"场景是持续干扰源）。

**日志签名**：`Hammer guard: rejected N from <IP> (backoff 300s)` 的 N 随
时间单调增长 + 同 IP `Stream accept` ~90s 节奏；来源 IP 常是测试机自身。

**修复**：暂无——两侧都是 md5-locked 共享件（SPA 五件套 + mjpeg 护栏）。
候选：SPA 端 90s 换流加抖动/遭 503 后暂停换流；护栏端 300s 封顶后放行一次
探测连接。归家族同步会话（与 seeed `feat/day-night-auto` 的 SPA/契约漂移
一并裁决）。**临时纪律：测试结束关标签页、杀无头浏览器**（本次残留 chrome
标签页一夜制造假 churn，2026-09-12 晨清理）。

---

### PIT-050 SPA 三个 AI 开关零回读：UI 假"全关" × 设备 AI-VGA 锁真生效（2026-09-13，家族）

**症状**：用户报告 Web 切不了分辨率。设备 `ai_*_en=true` 时 AI-VGA 锁拒绝
非 VGA（`400 Disable AI to use non-VGA resolution`），但 UI 三个 AI 开关显示
全关——自相矛盾。

**根因**：`index.html` 开关默认关 + `app.js` 全文无一处 `setToggle('ai-*')`
（loadConfig 拿到 `ai_*_en` 键弃置不用）。设备端锁读 NVS 真值。

**修复**：`loadConfig()` 对存在的 `ai_*_en` 键回读 setToggle（键缺失=无 AI
板保持默认），联动由既有 `loadCamera()→updateAIVGACoupling()` 收尾。三仓
字节一致：n16r8 `f68e3d2` / ai `47c18bd` / luatos `dae4487`（issue #14；
seeed 漂移分支待同步会话带上）。

**教训**：SPA 引入新联动锁（AI-VGA）时必须审计开关的**回读路径**——只加
锁不回读，用户看到的就是"没开却锁死"。同类审计点：任何 setToggle 目标
都应有且仅有一个数据源。

---

### PIT-051 【判决已收回】第二台 n16r8"射频 TX 硬件故障"——实为内部堆地板饿死新建分配（2026-09-13 复测推翻+确诊，n16r8）

**原判（2026-09-10/11，已收回）**：曾判"射频 TX 通路硬件损坏"，依据：WiFi 关联
+DHCP 成功但 30-60s 后对任何主机无 ARP 回复；AP 回落模式 2m 内扫不到信标；
串口见过 `wifi: lmac stop hw txq`；且"重启/USB 整镜像重刷/换 AP/擦 phy_init/
擦 NVS 全无效"。**用户质疑"之前一直正常、没动过硬件"后复测，全部推翻。**

**收回证据（2026-09-13 当场实测）**：
1. 该板 09-12 07:29 开机起连续 **28h 健康推流**（16.3fps、seq 160 万帧、日志尾部
   errno=12 洪水可见）——"烧坏"的射频不会自愈再干 28h；
2. ping 100% 丢包、`/api/status` 15s 超时的**同一时刻**，串口实况 =
   `frame_broadcaster ~15.8fps subs=2`（两路 MJPEG 正在推）+ ARP 表确认它就关联在
   原 IP + `curl /` 拿到 HTTP 200（5s 慢但通）——**已建立的流不耗新分配所以活着；
   ICMP 应答/TCP accept/新 socket 全要新分配所以死**；espectre ping errno=12 洪水
   同屏（每秒 1-3 条）；
3. `AT+REBOOT` 后 ping 立即恢复 0% 丢包——纯状态性故障，非器质性。

**真因**：PIT-048 的内部堆地板（`SPIRAM_MALLOC_ALWAYSINTERNAL=16384` 碎片化
驱动 + wifi 133KB/tcpip 动态 98KB/静态 90KB）在"NVR/查看端拉流 + CSI"负载下把
新建分配全数饿死；深压时连 wifi 驱动自身 TX 缓冲都分配失败 → `lmac stop hw txq`
——**TXQ 停发是分配饥饿的症状，不是 PA 损坏的证据**（原判把症状当器质性证据）。
关键盲区：外部拉流端（NVR/浏览器僵尸页，PIT-049）是**板外常量**，每次"重启
验证"都在 30-60s 内被它们重新压死——排除矩阵漏控了这组板外变量，"软件重置全过
即硬件"的推理链在板外变量未控时不成立。"AP 回落信标 2m 扫不到"也与堆压解释
相容（TXQ 已停→信标根本没发出去）。旧日志段已覆盖，`lmac` 原行不可复核，降级
为不可引用。

**修复与二次实测（2026-09-13 落地本板）**：`SPIRAM_MALLOC_ALWAYSINTERNAL`
16384→2048 刷入（USB 整镜像；弱链路+双路拉流下 Web OTA 上行仅 3.6-6KB/s、
两次截断 589KB/1.96MB——OTA 链路本身也是堆地板受害者，此形态下 USB 是正当
回退）；顺带 SPA AI 回读修复 app.js（md5 b213b699…）落地。
**2048 的真实边界（重要，修正首验解读）**：空载/轻载下 errno12 3.7/min→0
（PIT-048 补遗首验，但那是无拉流窗口的读数）；**重载（CSI+双路 VGA 拉流）
下 2048 不救服务面**——同窗口实测 errno12 ≈12.7/min、internal free 13-15KB、
ping 全丢/HTTP 10s 爬行，`AT+CFGSET=csi_enabled,0` 后 ping **立即** 0% 丢包
（但 RTT 仍 1.7-2.4s、internal 仍 ~13.5KB——两路 VGA 拉流本身就钉死地板，
CSI 只是加码）。即：**n16r8 内部 RAM 天花板 = 2 路 VGA 观看者 + CSI 不可
兼得**；推流本体（已建立的流）在两种状态下都健康。待用户拍板项：四仓
sdkconfig.defaults 推广（带本数据）、NVR 服务板是否执行 CSI-off 政策
（镜像 ai/luatos 的"摄像头优先"PIT-038 补遗二）、lwIP/堆深改（既有家族项）。

**教训（硬件判决纪律，判不可逆结论前必读）**：
1. **"ping 不通"≠"TX 死"**：下判前先看 `frame_broadcaster subs=N`——正在
   推流 = TX 活着；ping/新连接死只是"新建分配"死；
2. **排除矩阵必须控板外变量**：重启/重刷后的复核窗要 > 外部客户端回压时间
   （≥60s），且须在无拉流窗口独立复核一次，否则测的是"负载"不是"板子"；
3. 硬件判决不可逆（换板/RMA），下判前必须穷尽状态性解释 + 不同负载窗口复测；
4. 本板 CH343 桥 **EN 未接线**：open ttyACM1 不会复位（"open 即复位"的
   PIT-003/044 印象对该板不成立——实测 uptime 连续），复位走 `AT+REBOOT`；
5. 原生 USB-JTAG 串口镜像会间歇断流，"板没日志了"先重开口验证再说。

**顺带家族级发现**（保留有效）：
1. **SPIFFS OTA 半写会毁 UI**：上传中断流时设备已擦除目标分区（本次事故
   起点）——SPIFFS 上传同样受 PIT-048 补遗②的坏窗截断签名约束，受损后走
   USB 整镜像恢复；
2. AT 控制台是网络全黑时唯一的配置通道（WIFI/WIFI2/AI 开关/IP 查询全可用），
   但 `at_console.py --cmds` 会把逗号串拼成一条（WIFI2 的值会吞掉后续命令，
   必须逐条发）；
3. `pkill -f` 的模式若出现在自己命令行后段（如后面要重启的采集器路径）会
   匹配自身自杀整条链——用 `[.]` 变形或按 pid 杀。

### PIT-052 板上跑着来历不明的固件 + AI 全关白烧 57KB 内部堆：.119 三联症状全灭案（2026-09-13，n16r8 第一台单元）

**症状**：用户报"能开 Web，无法看实时预览、无法被 NVR 拉流、分辨率调不动"。
网络侧取证：`/api/capture` 正常出图；MJPEG :81 与 RTSP :554 均 **TCP 通、
请求发出、0 字节被 RST**；NVR（.9）每 1-2s 重连全灭进防锤护栏。

**真因（两层叠加）**：
1. **内部堆地板饿死任务创建**（PIT-048/051 同病，本台是重载下的必败区）：
   `xTaskCreatePinnedToCore(mjpeg_cli, 4KB)` / espp RTSP 会话线程（2×8KB）
   在碎片化地板上分配失败 → listen 任务 accept 后立刻 close() = 裸 RST。
   httpd 是启动期建好的固定线程池所以 Web 活着；camera fb 在 PSRAM 所以
   capture 活着——"Web 好流全死"正是这个分层签名。
2. **AI 全关仍无条件加载 HumanFaceDetect 模型 + quirc（-32.7KB 实测）+
   24.5KB 栈 ai_pipe 任务**——为禁用功能常驻烧 ~57KB，是地板上最大的
   可回收项。
3. **当日神秘固件 b2c0d66**（编译于 16:36，不在本仓 reflog/远端/CI 任何
   可达谱系；判定为 PIT-051 会话的 ALWAYSINTERNAL=2048 实验构建被刷上
   .119）比已知基线再多吃 ~5KB——恰好把 4KB 任务分配从"勉强能"翻到
   "必挂"。用户看到的故障态就是它。

**日志签名（秒级定位）**：
```
I mjpeg_streamer: Stream accept from <ip>
E mjpeg_streamer: Failed to create client task      ← RST 的直接原因
E rtsp: Failed to create video feed task（或不监听）
W csi_motion: [espectre TrafficGen:535] ping send failed (errno=12)   ← 同病佐证
```
快算内部堆：`/api/status` 的 `free_heap − free_psram`（本例 ~13.6KB 地板）。

**修复（43a5d7d，feat/csi-tuning-v1.7，已上 .119 全量验证）**：AI 惰性
初始化——`ai_init()` 只建互斥锁置 `s_dormant`，模型/QR/任务挪入
`ai_heavy_init()`，首个特性"开"（boot 布线/POST /api/ai/AT）经 `ai_wake()`
触发；唤醒失败保持休眠且**失败路径必须回收模型/QR/缓冲**（否则重试唤醒
每次泄漏 ~15KB，实测抓到）。AI 全关地板 13.6→43KB；120s 官方探针
1025 帧/8.53fps/225.8KB/s/0 断连双流并发；RTSP/分辨率热重配/capture 全绿。
已知边界：地板形成后运行期开 AI 仍可能 24.5KB 栈无连续块而唤醒失败
（graceful）；boot 期唤醒发生在碎片形成前正常。**本修与 PIT-051 的
ALWAYSINTERNAL=2048 正交叠加**（43KB 读数即两者同开）。

**教训**：
1. **接手任何"板子坏了"先对固件户口**：`/api/ota/info` 的 `app_version`
   + boot banner 的 compile time 对仓库 `git describe`——对不上就是最大
   变量，先刷已知基线再诊断（本次基线复测直接把神秘 5KB 隔离出来）；
2. "TCP 通 + 请求发出 + 0 字节 RST" = 设备侧任务/线程创建失败的经典签名
   （对照：503 文本响应才是护栏/满员拒绝）；
3. 长任务常驻栈 + 无条件加载的重资产（模型/解码器）要看**配置开关**，
   不只是代码路径开关——"功能关了"不等于"资源没占"；
4. 弱链路+双路拉流下 Web OTA 上传会反复截断（本次 3 次：204KB/2.18MB/
   2.87MB），且半写只污染目标槽不伤运行槽——重试无果后果断走 USB
   （PIT-026：先停采集器再插 esptool）。

**2026-09-13 晚补充（UI 全面回归测试中的新表现）**：长运行（~90min）堆碎片化
地板下，**任何触发 camera_reinit 的操作（质量滑杆拖动即触发！契约写明
quality 走 reinit）可能失败**——`cam_dma_config: DMA buffer 16384 Byte
malloc failed, largest free block 11264`（DMA 描述符要 16KB 连续内部块）→
回滚 init 同因失败 → 按设计 1s 自动重启（40s 恢复，无崩溃，rst:0xc 干净）。
复现：21:47:55 .119 质量滑杆测试中段自愈重启一次。**待家族决策**：quality
改 live-apply（sensor->set_quality，无 DMA 重配）可消除大部分 reinit 时机，
但契约 §"framesize/quality requires reinit" 要改 + 四仓同步。次要：LED
亮度往返 1% 舍入（set 30 → get 29，duty↔percent 换算地板）。UI 侧确认
正常：AI 唤醒失败后开关状态/UI 回读仍一致（graceful），重启后 sessionStorage
密码保留、SPA 自愈重连预览。

### PIT-053 "信号强但 Web 间歇打不开"双根因：射频休眠默认值 + 双 NVR 拉流负载（2026-09-13，n16r8 .119）

**症状**：PIT-052 修复数小时后用户再报"http://192.0.2.119/ 又看不到了"。
板侧全绿（无重启、MJPEG 150KB/s、堆 43KB），但主机 ping 静默基线 10% 丢包、
RTT 均值 177ms/峰值 525ms——**RSSI -40dBm 强信号下完全不合理**。

**真因（两个独立因素）**：
1. **`esp_wifi_set_ps` 从未调用 → IDF 默认 MIN_MODEM 射频休眠**：射频在
   DTIM 间隔休眠，浏览器页加载这类延迟敏感流量直接吃延迟尖峰+丢包。
   家族 PIT-001 早就备注"出厂应 PS_NONE"，n16r8 仓漏设（其他仓勿假设
   已设，逐仓 grep `esp_wifi_set_ps`）。
2. **双 NVR 拉流负载**：.9（录像）+ .30（查看端，PIT-038 遗留的外部锤击
   源）同时拉 MJPEG 时弱链路进失聪窗（ping 52.5% 丢包）——.30 间歇性
   出现，用户感知就是"时好时坏"。这是空口物理，固件只能缓解不能救。

**A/B 数据（同 1 客户端负载，120s ping）**：
| 配置 | 丢包 | RTT 均值 | min |
|---|---|---|---|
| MIN_MODEM（旧） | 10% | 177ms | 5.3ms |
| PS_NONE（6d0b8cc） | **3.3%** | **106ms** | 3.6ms |

**附带实验（负结果同样入库）**：强制 `WIFI_BW20`（AGENTS 挂账的"关 HT40"
候选）在双客户端拉流下是**负优化**——每帧双倍空口时间，ping 52.5% 丢包，
已回退跟随 AP 协商。"关 HT40"候选正式关闭，"挪信道"仍待 AP 侧动作。

**修复（6d0b8cc，已 OTA 上板验证）**：`esp_wifi_set_ps(WIFI_PS_NONE)` +
返回值日志；顺带 /api/ai/status 休眠态 404→200 零值（SPA 500ms 轮询下
404 每请求刷两行日志且语义错误）。

**教训**：
1. "信号强（RSSI>-45）但丢包高"先查省电模式再怀疑信道——一条
   `esp_wifi_set_ps` 的遗漏就是 177ms vs 106ms、10% vs 3% 的差距；
2. A/B 射频结论必须同负载对比：外部查看端（.30 类）间歇接入会把
   对照组污染成 52.5%，先 `grep "Stream accept"` 确认采样窗口的
   客户端数再下结论；
3. 负优化实验结果（HT20 强制）也要归档——否则"关 HT40"候选会被
   反复重试。

---

## 2. 板级坑速查（细节在各仓 AGENTS.md）

| 板子 | 高危坑摘要 | 详情 |
|---|---|---|
| ai-thinker (ESP32) | camera 必须在 STA 连上后 init（DMA freeze）；GPIO14 SD/camera 共享；GPIO0=XCLK 不能做按键；PHY 每次全校准+必要时擦 phy_init 分区；flash 后 RTS 卡复位需 `esptool --no-stub run`；HT20+20dBm 弱信号配置；**画质 10-63/UXGA 上限（PIT-021）**；**AT=家族核心 at_command.c+at_port.c（2026-09-05）；台架板串口 RX 硬件损坏 AT 不可用（PIT-030）**；**ESPectre CSI 仅感知⚠️（初代内部 RAM 天花板 ~5KB，LLTF20 档成立，PIT-034）；生产门关 2026-09-08（摄像头优先，PIT-038 补遗二）；2026-09-09 反转：CSI 替代 ΣΔ 成生产触发（ΣΔ 60KB 内部缓冲退役腾出空间，:81 互斥门解除，三线并存 6h soak，PIT-040）**；**AMPDU 已关+常驻任务静态栈+motion 网格 PSRAM+httpd 栈红线 8192+护栏仅新违规续期+漫游扫描退避（PIT-039）** | `ai-thinker-esp32-cam/AGENTS.md` |
| esp32s3-n16r8 | esp32-camera 走 `patches/` 补丁机制；POST /api/config 白名单校钥（见 PIT-005）；**已支持双 WiFi（2026-09-04：开机 RSSI 择优 + DHCP 盲区切网 + 连败切网，AT+WIFI2）**；AMPDU 仍关闭（2026-09-04 复测维持）；**NVS 键 ≤15 字符红线（PIT-022：ai_motion_enable 曾废掉全部保存）**；RTSP 会话创建异常已兜（PIT-025）；**分辨率上限 SXGA（2026-09-05 二次翻案：真因 defaults 漏抄 SPIRAM_SPEED_80M/WiFi-LWIP 迁移 + XCLK 20M 帧损坏，PIT-021 二次附录）**；**IDLE1 TWDT 洪水已根治（构建期 CHECK_IDLE_TASK_CPU1=n，PIT-028）**；**ESPectre CSI 完美可用✅（wifi_manager abort 竞态已修+任务核0，PIT-034）**；**第二台单元（CH343/OV5640）无自动下载电路：USB 烧录走板载原生 USB-OTG 口（PIT-044 已回填）；LED init 无调用者已修+GPIO2 实配（issue #11，2985d1f）；v1.7 推流回归=churn 无护栏打穿 socket 表（护栏已对齐 ai/luatos，2985d1f 实测 90s 重启循环→零重启）；遗留：内部堆地板已定性=基线非回归（wifi 133KB+tcpip 98KB+静态 90KB+碎片化，espectre ping ENOMEM 258 次/4.6h 为已知劣化，PIT-048；诊断钩子 `#if` 合入 d9d5a36）；**PIT-051：堆地板在拉流+CSI 负载下可压死整个服务面（ping/新连接死而已建立的推流活着——曾被误判"射频 TX 硬件故障"，2026-09-13 判决收回）；本板已切 ALWAYSINTERNAL=2048——空载有效、重载（2 路 VGA+CSI）仍钉死地板 ~13-15KB，2048 非充分解，家族推广与 CSI-off 政策待拍板；**PIT-052：AI 全关白烧 ~57KB 已修（惰性初始化 43a5d7d，第一台 .119 实测地板 13.6→43KB、三联症状全灭；与 2048 正交叠加）****；HEAP_TASK_TRACKING 诊断构建在板上自毁勿再开（PIT-048）；NVR 可经 mDNS 绕过 onvif_enable 发现门控** | `esp32s3-n16r8-cam/AGENTS.md` |
| luatos (A10) | PSRAM 禁用是设计（勿开，boot loop）；IDF 钉 v5.5.4；WPA3 关、AMPDU 已重开（2026-09-03 复验）；STA 强制 HT20；单流硬上限+热重配走重启（PIT-012）；EMFILE 已修；**VGA 锁定+画质 10-63（PIT-021）**；**MJPEG 死客户端探测+SNDTIMEO 已补（PIT-024 曾漏同步致堆耗尽）**；**ESPectre CSI 仅感知⚠️（min_heap=108B 推流并存崩，PIT-034）；生产门关 2026-09-08（摄像头优先，PIT-038 补遗二）** | `luatos-esp32s3-a10-camera/AGENTS.md` |
| seeed (XIAO) | 实戴 OV5640（PIT-006）；+audio 最全仓；esp32-camera 补丁 vendor 在 `components/`；EMFILE 已修；**93°C 过热观察项（PIT-016）**；**UXGA 上限（FHD/QXGA 温度剔除）+画质 10-63+分辨率重启应用（PIT-019/021）**；停录像杀流已修（PIT-020 预览模式）；**WS 违例强制断开+SD 周期重试+record_on_boot 开关（PIT-023）**；**OTA 首传可能被 step-8a 自检回滚（重传即过）；sensor 层判活用句柄勿用旗标（PIT-029）；AT 通道受共口日志洪峰影响（PIT-031）；SD 段轮换停顿已修（PIT-033）；**ESPectre CSI 运动感知试点已跑通（vendored+8 处适配，PIT-034，Kconfig 门控默认关）** | `seeed-esp32s3-cam/AGENTS.md` |

---

## 3. 流程红线（不可越）

- **烧录优先走 Web OTA（2026-09-04 规范）**：可达设备一律 Web OTA 交付固件/UI
  （用法与验证见 PIT-017），持续保证固件+前端上传链路健康；USB 仅限救砖/全新芯片/
  luatos（单分区无 OTA；n16r8 例外已于 2026-09-05 随 eb65387 撤销，Web OTA 可用）。详见根 `AGENTS.md`
  "Flashing policy"。
- **tag/release 只能由用户在当前会话显式触发**（2026-09-02 教训：推断式打 tag 被纠正）。
- 硬件测试前先 `ls /dev/serial/by-id/` 扫实际连接、按串口日志识别板子；
  完整协议见根 `AGENTS.md`"Hardware testing protocol"。
- 四仓不互抄 pin 表 / partitions.csv / sdkconfig.defaults / PSRAM 配置
  （flash 大小、PSRAM 类型、传感器全不同，抄=砖或 boot loop）。

---

## 4. 新条目模板

```markdown
### PIT-0XX 标题（YYYY-MM-DD，<仓/跨项目>）

**症状**：用户/日志看到什么。
**真因**：根因链，一句话讲清因果。
**日志签名**：可 grep 的关键行（如有）。
**修复**：具体改动 + 已验证于哪些仓。
**教训**：下次如何一眼识别 / 通用规则。
```

条目编号递增不复用；同坑复发在原条目下追加"复发记录"。

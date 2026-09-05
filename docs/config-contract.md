# MiBee Cam 家族配置契约（v1.0，2026-09-05）

> **定位**：四仓（ai-thinker / esp32s3-n16r8 / luatos / seeed）配置子系统的统一契约：
> 持久化格式、字段名与取值域、校验矩阵、默认值、迁移策略、SD 卡 provisioning 格式。
> 地位同 `docs/api-contract.md`（HTTP 面）与 `docs/at-command.md`（串口面）；
> 本文件是三面共享的**配置内容真源**。api-contract §8 "配置内部模型不强改"
> 条款自 v1.3 起由本契约取代。
>
> **历史**：此前三种持久化架构并存（ai-thinker/luatos 版本化 blob、n16r8 逐键、
> seeed 逐键+独立命名空间），四套字段命名与默认值漂移（含 `cam_framesize`
> 同值不同义）。2026-09-05 全家族统一。

## 1. 持久化格式（四仓一致）

- **NVS 逐键存储**，命名空间统一 `mibee_cfg`。
- **键名 ≤15 字符**（NVS 硬限制），所有键字面量必须 `_Static_assert(sizeof(key)<=16)`
  （PIT-022：超长键曾使整个 `config_save()` 失败）。
- **版本键 `schema_ver`**（u16）：本契约的家族 schema 版本，编号独立于各仓
  历史版本号（ai-thinker blob v16 / luatos blob v3 / seeed schema v2 迁移后
  一律落 `schema_ver=1`）。
- **版本提升策略**：新增键 = 缺键取默认值（天然向前兼容）；改名/删除/语义变更 =
  显式迁移步骤 + schema_ver bump，并在本文件 §8 登记。
- **单键自治**：读时缺键 = 编译默认值；写时单键失败只影响该键（不得中止整批
  写入——PIT-022 的另一半教训），失败逐键 WARN。
- **落盘时机**：任何写路径（HTTP /api/config、AT 指令、SD 导入）在改动后
  立即持久化；`AT+SAVE` 为幂等空操作。
- **统一访问 API**（AT 共享核心与 web/SD 导入共用的地基，各仓签名一致）：
  `config_get_copy(cfg*)` 取快照、`config_set_*` 类型化写、`config_save()` 落盘、
  字段反射表（名/类型/域/敏感位）供 AT CFGGET·CFGSET 白名单生成。

## 2. 分辨率刻度（与 api-contract §5 同源）

`cam_framesize` 的值 = **esp32-camera 组件 `framesize_t` 枚举**（四仓组件 md5
一致：QVGA=6, VGA=10, SVGA=11, XGA=12, HD=13, SXGA=14, UXGA=15）。旧自有刻度
按 api-contract §5 迁移表在 config 加载时翻译。合法性 = 落在该板
`supported_resolutions`（三层上限交集）内，越界 400/ERROR。

## 3. 字段总表

列：JSON 字段（HTTP 权威名）· NVS 键（≤15 字符）· 类型 · 取值域 · 家族默认。
"板覆盖"见表 §5。**敏感字段**（NVS/AT 读侧永不回显，HTTP GET 掩码 `****`）：
`wifi_pass` `wifi_pass_2` `web_password` `rtsp_pass` `webhook_secret` `webdav_pass`。

### 3.1 核心字段（四板必备，同名同域同校验）

| JSON 字段 | NVS 键 | 类型/域 | 家族默认 |
|---|---|---|---|
| device_name | device_name | str≤32 | `MiBeeCam` |
| wifi_ssid / wifi_pass | wifi_ssid / wifi_pass | str≤32 / str≤64 | 空 |
| wifi_ssid_2 / wifi_pass_2 | wifi_ssid_2 / wifi_pass_2 | str≤32 / str≤64 | 空 |
| allow_ap_fallback | ap_fallback | u8 {0,1} | 1 |
| timezone | timezone | str≤47（POSIX TZ） | 空（=UTC） |
| web_password | web_password | str≥6 | Kconfig `MIBEE_CAM_DEFAULT_WEB_PASSWORD`（公开默认 `mibeecam2026`，PIT-027） |
| cam_framesize | cam_framesize | u8 = framesize_t，板内合法域 | 10 (VGA) |
| cam_fps | cam_fps | u8 1-30 | 15 |
| cam_quality | cam_quality | u8 10-63（PIT-021） | 12 |
| cam_vflip / cam_hmirror | cam_vflip / cam_hmirror | u8 {0,1} | 0 |
| onvif_enable | onvif_enable | u8 {0,1} | 1 |
| xclk_freq_mhz | xclk_freq_mhz | u8 ∈{10,16,20} | 板值（§5） |

### 3.2 能力门控字段组（"不适用即省略"——无该能力的板不实现不暴露）

| 组 | 能力 | 字段（JSON 名；NVS 键不同时括注） |
|---|---|---|
| 画质微调 | 板支持 | cam_brightness / cam_contrast / cam_saturation / cam_sharpness（i8 -2..2）、day_night_mode（u8 0-2） |
| SD 运维 | sd | sd_log_enabled、cleanup_low_pct（1-99）、cleanup_high_pct（`cleanup_high`；5-80 且 ≥low+5；语义=空闲百分比，api-contract §11.6） |
| 录像 | recording | record_mode、segment_sec（u16 5-3600）、frame_drop_enabled（`frame_drop_en`）、record_on_boot、video_record_to_sd（`video_to_sd`）、audio_record_to_sd（`audio_to_sd`，兼 audio 能力） |
| 延时摄影 | timelapse | **家族标准 = 动态模型 8 字段**：timelapse_enabled（`timelapse_en`）、timelapse_interval_s（u8 1-255）、timelapse_burst_count、timelapse_mode（u8 0=静态 1=动态）、timelapse_min_interval_s（`tl_min_int_s`）、timelapse_max_interval_s（`tl_max_int_s`）、timelapse_decay_factor、timelapse_decay_period |
| 移动侦测 | 板支持 | **家族超集模型**：motion_enabled、motion_sensitivity（u8 0-100，越大越灵敏）、motion_cooldown_s（u16 1-300，两次触发最小间隔）、motion_active_interval_s（`motion_act_int_s`；u8 1-30，持续活动期再触发间隔） |
| AI 管线 | ai | ai_face_en、ai_motion_en、ai_qr_en（u8 {0,1}） |
| RTSP | rtsp | rtsp_user（str≤32，默认 `admin`）、rtsp_pass（str≤64） |
| WebSocket | websocket | ws_enable（u8 {0,1}，默认 1） |
| Webhook | 板支持 | alert_webhook_enabled（`webhook_en`）、alert_webhook_url（`webhook_url`，str≤256）、webhook_secret（str≤64） |
| NAS/WebDAV | 板支持 | webdav_enabled、webdav_url（str≤128）、webdav_user（str≤32）、webdav_pass（str≤64）、webdav_base_path（`webdav_base`，str≤128，拒绝 `..`） |
| WiFi 无线电 | 板支持 | wifi_roam_rssi（0=关 或 -90..-50）、wifi_roam_gap_s（`wifi_roam_gap`，5-15）、wifi_tx_power、wifi_power_save（`wifi_power_sv`）、wifi_reconnect_hours（`wifi_recon_h`） |
| 板级扩展 | — | 登记后允许（见 §7） |

## 4. 校验矩阵（HTTP POST / AT 写入 / SD 导入三方同表，越界一律拒绝）

cam_fps 1-30 · cam_quality 10-63 · cam_framesize ∈ 板 supported_resolutions ·
web_password ≥6 非空 · cleanup_low_pct 1-99 且 cleanup_high_pct ≥low+5 且 ≤80 ·
segment_sec 5-3600 · motion_sensitivity 0-100 · motion_cooldown_s 1-300 ·
timelapse_interval_s 1-255 · xclk_freq_mhz ∈{10,16,20} · wifi_roam_rssi 0 或
-90..-50 · timezone 长度 1-64（非空时）· webdav_base_path 拒绝 `..` ·
所有字符串字段先验长度截断拒绝（不静默截断凭据）。

## 5. 默认值：家族表 + 板级覆盖

家族默认见 §3.1；**板级覆盖**（硬件调优事实，唯一合法偏离点，改值须实测背书）：

| 项 | ai-thinker | n16r8 | luatos | seeed |
|---|---|---|---|---|
| cam_framesize | — | — | —（板上限 VGA） | **11 (SVGA)**（OV5640 热调优） |
| cam_fps / cam_quality | — | — | — | **12 / 18**（同上，PIT-016） |
| xclk_freq_mhz | 20 | 16 | 20 | 16 |
| allow_ap_fallback | — | — | — | **0**（保留现行为） |
| wifi_roam_rssi | — | — | — | **-75** |
| rtsp 鉴权源 | — | rtsp_user/rtsp_pass | — | rtsp_user/rtsp_pass（v1.0 起入契约；存量迁移见 §6） |

## 6. 旧格式迁移（一次性，四仓各一，代码须可重入且幂等）

| 仓 | 旧格式 | 迁移 |
|---|---|---|
| ai-thinker | `camcfg/config` blob v16 + magic | 复用现有 V1→V16 梯子解出语义 → 逐键写入 `mibee_cfg`（刻度 0-3 → framesize_t）→ 旧键改名 `config_bak` 留存只读；梯子代码迁移后删除 |
| luatos | `mibee_cfg/config` blob v3（前代 device_cfg 已迁） | 同上（刻度 0-3 翻译；板上限 VGA 钳位）；`onvif_enabled`→`onvif_enable`、`jpeg_quality`→`cam_quality`、`wifi_ssid2`→`wifi_ssid_2` |
| n16r8 | `mibee_cfg` 逐键（无版本） | 键名对齐 + 补 timezone/cam_fps 等新键（缺省即默认）+ 写 `schema_ver=1`；刻度恒等 |
| seeed | `cam_config` 逐键 schema v2 | 键值复制到 `mibee_cfg` + 键名/刻度翻译（0-5 → framesize_t；timelapse 2 字段→8 字段按 §6.1）+ rtsp_pass 一次性种子（= 现 web_password，本板 RTSP 原用 web_password 鉴权）；旧命名空间留存只读 |

**迁移保持性验证**（每次部署必做）：升级前 dump `GET /api/config`（+ AT CFGGET
白名单）→ 升级后 diff；WiFi 凭据以设备自动重连为准；分辨率值按 §2 表核对；
密码字段掩码跳过。

### 6.1 模型收敛映射

- **motion**：ai-thinker/luatos 旧 `motion_threshold`(0-100，越小越灵) →
  `motion_sensitivity = 100 - threshold`；旧 `motion_cooldown` → `motion_cooldown_s`；
  ai-thinker `motion_saved_threshold`（计数去抖）废弃（由 cooldown 语义吸收）。
  seeed 旧 `motion_sensitivity` 直迁；`motion_idle_interval_sec` → `motion_cooldown_s`；
  `motion_active_interval_sec` → `motion_active_interval_s`。
- **timelapse**：seeed 旧 2 字段（interval_sec + mode 0/1/2）→ 8 字段家族模型：
  interval 直迁；mode 0→0（静态）；mode 1/2→1（动态），min/max/decay 取家族默认。
- **webhook**：luatos 旧 url+secret（无开关）→ 补 `alert_webhook_enabled=1`；
  ai/seeed `alert_webhook_*` 直迁；secret 无则空。

## 7. 板级扩展字段（登记制，同 AT §5）

| 仓 | 字段 | 说明 |
|---|---|---|
| ai-thinker | flash_threshold | 侦测联动闪光灯阈值 |
| luatos | server_url（**改为可选**，修复旧版非空校验 bug）、mdns_hostname | 遗留上传目标 / mDNS 主机名 |
| seeed | （无） | |

## 8. 变更记录

- **v1.0（2026-09-05）**：初版。逐键 NVS + `mibee_cfg` 统一；framesize_t 统一刻度；
  核心字段表/能力门控字段组/校验矩阵/默认值与板级覆盖；motion 超集与 timelapse
  动态模型定标；SD provisioning 统一格式（§9）；四仓旧格式迁移方案。
- （后续变更在此追加：字段增删、语义变更、迁移步骤，均须 bump schema_ver。）

## 9. SD 卡 provisioning 统一格式

- 位置与文件（家族统一，ai-thinker 旧 `/sdcard/config.txt` + `wifi.ssid=` 行风格废止）：
  - `/sdcard/config/wifi.txt` —— WiFi 凭据：`wifi_ssid=<...>` / `wifi_pass=<...>`
    （及 `_2` 备用行）；
  - `/sdcard/config/config.txt` —— 通用键值（键名 = 本契约 JSON 字段名）；
  - `/sdcard/config/nas.txt` —— NAS/WebDAV 字段。
- 行格式 `KEY=VALUE`，`#` 注释，UTF-8，行尾去空白。
- **覆盖规则（安全红线）**：wifi.txt 仅在 NVS 中**无任何凭据**时生效（绝不静默
  覆盖在线凭据）；config.txt/nas.txt 逐键应用仍走 §4 校验。
- `one_time` 清除标记：文件首行 `one_time` 时导入成功后删除该文件（工厂
  provisioning 用）；seeed 既有行为，四仓统一。

---
*本文档为四仓共同规范，修改任一板的配置子系统前先改这里；配套：
`docs/api-contract.md`（HTTP 面）、`docs/at-command.md`（串口面）。*

# MiBee Cam 家族 API 契约 v1.2

> 适用四仓：`ai-thinker-esp32-cam` · `esp32s3-n16r8-cam` · `luatos-esp32s3-a10-camera` · `seeed-esp32s3-cam`
>
> 原则：**无差异部分完全一致；有差异部分只允许通过"能力门控 + 动态元数据"产生差异。**
> 禁止字段名、数值刻度、语义分叉。`GET /api/capabilities` 的 `api_version` 即本版本号；
> 破坏性变更必须 bump 版本并在本文档记录迁移说明。
>
> **v1.1 变更（2026-09-02）**：家族统一默认密码、密码修改入口、遗留差异收敛（见 §8/§9）。
> **v1.2 变更（2026-09-03）**：SD 文件批量管理 + 安全格式化 + cleanup 语义统一（见 §11）。

## 1. 信封与鉴权（所有板一致）

- 成功：`{"ok":true,"data":...}` HTTP 200；失败：`{"ok":false,"error":"<msg>"}` + 400/401/404/500/503。
- 写操作鉴权：`X-Password` 请求头。
- **默认密码（v1.1）**：所有设备出厂/空密码迁移后统一为 `***REMOVED-DEFAULT-PASSWORD***`；
  服务端拒绝空或长度 <6 的密码（400）。空密码的 `SET_PASSWORD_FIRST` 状态仅在
  极早期固件上出现，新固件保留该兼容分支但正常流程不会触达。
- **修改密码**：`POST /api/config` 携带 `{"web_password":"<新密码>"}`，需带当前密码的
  `X-Password` 头（旧密码即隐式验证）。UI 入口：WiFi 页 → 修改密码（旧密码经
  `GET /api/auth` 预验证）。
- 密码类字段在 GET 响应中以 `"****"` 掩码；POST 回传 `"****"` 视为"未修改"。
- CORS：`OPTIONS /*` → 204；所有响应带 `Access-Control-Allow-Origin: *`。
- MJPEG 流在独立 TCP 服务器 `:81/stream`（`multipart/x-mixed-replace`，超限 503）。
  客户端上限按硬件不同：ai-thinker 1 / n16r8 2 / luatos 2 / seeed 3，
  通过 `status.stream_clients_max` 下发。

## 2. 核心端点（四板 100% 一致，必须实现）

| Method | Path | Auth | 说明 |
|---|---|---|---|
| GET | `/api/status` | open | 设备状态（字段见 §4） |
| GET | `/api/config` | open | 当前配置（密码掩码） |
| POST | `/api/config` | write | 部分更新；WiFi 变更写 NVS、重启生效 |
| GET | `/api/capabilities` | open | 能力矩阵（见 §3） |
| GET | `/api/capture` | open | 单帧 JPEG（`image/jpeg`） |
| GET | `/api/scan` | open | WiFi 扫描 `{networks:[{ssid,rssi,auth}]}`（RSSI 降序） |
| POST | `/api/time` | write | 手动设时间 `{year,month,day,hour,min,sec}` |
| POST | `/api/reset` | write | 恢复出厂并重启 |
| POST | `/api/reboot` | write | 重启 |
| GET | `/api/auth` | open | 校验密码 `{auth,password_set}`（未设密码时 auth=true） |
| GET | `/metrics` | open | Prometheus 文本 |

## 3. 能力门控（`GET /api/capabilities`）

规则：`capabilities.X == true` ⇒ 对应端点必须存在且语义一致；`== false` ⇒ 端点不注册
（404/405 可接受），**前端保证永不调用**。

| 端点 | 统一语义 | ai | n16r8 | luatos | seeed |
|---|---|---|---|---|---|
| `POST /api/led` `{"brightness":0-100}` | 亮度语义；简单 GPIO 板 0=灭/＞0=亮 | ✅(兼容 `?action=`) | ✅ | — | — |
| `GET /api/led` | 读回亮度/状态 | ✅ | ✅ | — | — |
| `POST /api/ai` + `GET /api/ai/status` | `{face,motion,qr}` 开关；检测结果 | — | ✅ | — | — |
| `POST /api/record?action=start\|stop` + `GET /api/record` | 录像控制/状态 | ✅ | — | — | ✅(v1.1 补 GET) |
| `/api/files` GET/DELETE(+`type=`) · `POST /api/files/batch` · `/api/download` · `POST /api/format` | SD 文件管理（v1.2 见 §11） | ✅ | — | — | ✅ |
| `/api/ota/info` · `/api/ota/upload` · `/api/ota/spiffs` | OTA | ✅ | 分区已备、端点未实现（能力位=false） | — | ✅(+URL 触发) |
| `GET /api/audio` | G.711 μ-law 裸流 8kHz | — | — | — | ✅ |
| `GET /ws` | WebSocket 事件推送（见 §6） | — | — | ✅ | ✅ |
| `/onvif/device_service` · `/onvif/media_service` | ONVIF SOAP | ✅ | ✅ | ✅ | ✅ |
| RTSP `:554/stream` | **必须 digest 鉴权** | — | ✅(rtsp_user/pass) | — | ✅(web_password) |

非布尔扩展键：`api_version`（本契约版本字符串）、`wifi_scan`。
延时摄影：统一走 config 的 `timelapse_*` 字段 + `/api/record`（ai-thinker 的
`/api/timelapse/*` 端点为遗留 tolerated variant，计划收敛）。

## 4. `/api/status` 字段命名（核心字段，全家族一致）

| 字段 | 类型 | 说明 |
|---|---|---|
| `device_name` | str | 设备名 |
| `firmware_version` | str | 固件版本 |
| `uptime` | num(s) | 运行时长 |
| `wifi_state` | str | 小写枚举 `ap\|connecting\|connected\|disconnected` |
| `ip` | str | IP |
| `wifi_rssi` / `wifi_channel` | num | STA 时返回 |
| `current_ssid` | str | **当前实际连接**的 SSID（区别于 config 的配置值；未连接为 `""`；seeed/ai-thinker 已实现，其余仓跟进时补） |
| `wifi_net` | str | 当前使用的配置槽位 `primary\|secondary`（双 WiFi 板返回，单 WiFi 板省略） |
| `camera` | str | 实测传感器型号（OV2640/OV3660/OV5640；**信设备不信文档**） |
| `resolution` | str | 当前分辨率名 |
| `free_heap` / `min_heap` | num | 堆 |
| `free_psram` | num | 无 PSRAM 板**省略该字段**（置 null 均不允许） |
| `stream_clients` / `stream_clients_max` | num | MJPEG 客户端 |
| `chip_temp` | num(°C) | 有温度传感器板返回 |

"不适用即省略"是通用规则：任何板不支持的字段直接不出现在 JSON 中，前端按字段缺省隐藏控件。
板级扩展字段允许追加（如 seeed 的 `recording`/`sd_*`、luatos 的 `heap_baseline`）。

## 5. `/api/camera` 与分辨率/画质刻度

- `GET /api/camera` 返回：`resolution`(str)、`cam_framesize`(num)、`cam_quality`(num)、
  `supported_resolutions:[{label,value}]`、**`quality_min`/`quality_max`（2026-09-04 起，
  板端声明的画质滑杆边界）**，以及该板支持的传感器微调字段
  （`cam_brightness/contrast/saturation/sharpness`、`cam_hmirror`、`cam_vflip`、`day_night_mode`
  —— 不支持的省略）。
- **`value` 数值刻度是板相关的**（ai 0-3 / seeed 0-5（板级上限 UXGA）/ n16r8 仅 VGA=10（模组实测）/ luatos 0-3）。
  前端**禁止硬编码分辨率表**，只从 `supported_resolutions` 填充下拉框，POST 只回传列表内的 value；
  画质滑杆的 min/max 必须取自 `quality_min`/`quality_max`（字段缺失时回退 1-63 兼容旧固件）。
  AI↔VGA 联动通过 label 前缀 `VGA` 识别。
- `POST /api/camera` 接受同名字段，**越界值一律 400**（错误信息含板级上限）。
  应用语义按板不同：
  - **ai-thinker**：分辨率/画质热重配（camera 互斥锁 + drain，无重启）。
  - **seeed**：画质热应用（OV5640 set_quality 单寄存器写）；**分辨率变化保存+1s 后重启应用**
    （OV5640 运行时 set_framesize 实测无效，PIT-019）。响应含 `rebooting:true`。
  - **luatos**：任何摄像头变更保存+1s 后重启应用（fb_count=1 DRAM 热重配竞态，PIT-012）。
    响应含 `rebooting:true`。

### 5.1 板级实测上限（2026-09-04，各板实机 90s 推流+状态采样）

| 板 | 传感器 | 分辨率上限 | 画质范围 | 上限档实测 | 超限后果（剔除理由） |
|---|---|---|---|---|---|
| ai-thinker | OV2640 | UXGA (0-3) | 10-63 | UXGA 采集 ~1.7fps / 推流 ~0.6fps，无崩溃 | —（传感器上限即板上限，UXGA 照常提供，慢但稳定） |
| seeed | OV5640（实戴） | **UXGA** (0-5) | 10-63 | UXGA 0.9fps、峰值 93.5°C | FHD 峰值 97.5°C / QXGA 100.5°C，超 S3 规格 85°C（PIT-016） |
| luatos | OV2640 | **VGA** (仅 0) | 10-63 | VGA 4.8-5.9fps，帧 13-23KB | SVGA 起 fb=96KB 在 DRAM（无 PSRAM）触发堆枯竭螺旋（PIT-012） |
| n16r8 | OV3660（模组） | **VGA**（仅 framesize 10） | 10-63 | VGA 26fps 广播 / 0.33s 拍照，零故障 | SVGA/XGA 热重配后取帧死（capture 0B）；HD+ 触发 cam_hal FB-OVF 风暴楔死 httpd；冷启动存 HD 时传感器仍输出 VGA（配置与实际脱节）——2026-09-04 上板实测 |

画质下限 10 的依据：esp32-camera 的 JPEG 帧缓冲按 `宽×高/5` 分配（假设最高 1:5 压缩），
q<10 在细节丰富的场景会超预算产生截断帧；q10 实测（ai-thinker UXGA 224KB / seeed UXGA 193KB）
均留有余量。

## 6. WebSocket 事件（`/ws`，websocket 能力板）

统一格式：`{"type":"<event>","timestamp":<unix_s>,"data":{...}}`

| type | data | 触发 |
|---|---|---|
| `motion_started` / `motion_cleared` | `{"score":0-100}`（有则给） | 移动侦测状态翻转 |
| `recording_started` / `recording_stopped` | `{}` | 录像启停 |
| `wifi_state_changed` | `{"state":"connected\|..."}` | WiFi 状态变化 |
| `stream_client_connected/disconnected`、`health_warning`、`upload_success/failed`、`wifi_switched_ssid` | 板级扩展 | 按板订阅 |

## 7. 前端规范

- **S3 三板**（n16r8/luatos/seeed）：共享同一套 SPA（`index.html`/`app.js`/`i18n.js`/`style.css`
  四文件 md5 一致，单一源码，**禁止按板分叉**）。全部板差异运行时来自
  capabilities + status 字段缺省 + `supported_resolutions`。
- **ai-thinker**：2026-09-03 起同样服务统一 SPA 四文件（与三 S3 仓 md5 一致）；
  旧 MPA 页面保留在原路径兜底（preview/config/files/setup.html 直达可用）。
- 鉴权 UX：密码存 sessionStorage，写操作自动附 `X-Password`；401 时提示到系统页设置。

## 8. 已知容忍差异（记录在案，计划收敛）

| 项 | 现状 | 方向 |
|---|---|---|
| ~~ai-thinker `/api/timelapse/*`~~ | **v1.1 已解决**：端点移除，启停走 config `timelapse_enabled`，运行态入 status | — |
| ~~ai-thinker `/api/led?action=`~~ | **v1.1 已解决**：JSON body 为主语义，`?action=` 保留兼容 | — |
| seeed `/api/record` 无 GET | 状态在 status.recording | 补 GET |
| ~~n16r8 OTA 端点~~ | **v1.1 已解决**：ota_updater 移植完成，ota=true | — |
| 配置内部存储 | blob+version(ai/luatos/seeed) vs 逐键(n16r8/seeed逐键) | HTTP 契约已统一；内部模型不强改 |
| ai-thinker OTA 401 响应 | 直发等价 JSON 字节（未走 send_json_error） | 已合入 |
| seeed 历史密码未知 | v1.1 一次性种子迁移（NVS 标记 `pw_seed_v1`）统一为默认密码 | 已合入 |

## 9. v1.0 破坏性变更清单（相对各仓旧版）

- `status.sensor` → `camera`（ai）；`status.camera_resolution` → `resolution`（n16r8）；
  `status.cam_framesize`(str) → `resolution`（luatos）；`status.wifi_mode` → `wifi_state`
  （值改小写枚举，ai 的中文值废除）；`heap_free/heap_min` → `free_heap/min_heap`（luatos）；
  `mjpeg_clients` → `stream_clients`（n16r8）。
- config 键：`wifi_ssid2/pass2` → `wifi_ssid_2/wifi_pass_2`、`onvif_enabled` → `onvif_enable`（luatos）。
- WS 事件：`motion_detected/motion_end` → `motion_started/motion_cleared`（luatos）。
- n16r8 `POST /api/camera` framesize 合法域 0-24 → 0-15（与广播列表一致）。
- n16r8 RTSP 自 v1.0 起强制 digest 鉴权（rtsp_user/rtsp_pass，默认 admin/admin）。

## 10. v1.1 变更清单（2026-09-02）

- 家族统一默认管理密码 `***REMOVED-DEFAULT-PASSWORD***`；空密码加载时自动迁移；seeed 对存量设备做一次性强制种子。
- 服务端统一拒绝空/`<6` 位密码（400）。
- UI 新增"修改密码"模态（旧密码验证 + 新密码 + 确认），三 S3 仓同步。
- seeed：新增 `GET /api/record`；n16r8：移植 OTA 三端点（`/api/ota`、`/api/ota/info`、
  `/api/ota/upload`、`/api/ota/spiffs`），capabilities `ota:true`；
  修复 n16r8 首次设密未持久化的白名单缺漏 bug。
- ai-thinker：移除 `/api/timelapse/*`（启停走 config，运行态入 status）；
  `/api/led` 增加 JSON body 主语义。
- luatos：SPIFFS 改为构建期自动打包（`spiffs_create_partition_image`），手动 spiffsgen 流程作废。

---
*本文档为四仓共同规范，修改任一板的 API 前先改这里。生成于 2026-09-02 统一化改造。*

## 11. v1.2 变更清单（2026-09-03，SD 管理与存储可靠性）

1. **`POST /api/files/batch`**（sd 能力板：ai-thinker / seeed，`X-Password` 鉴权）：
   - 请求二选一：`{"names":["<相对名>",...]}` 或 `{"scope":"all|photos|recordings"}`；
   - 响应 `{"deleted":n,"failed":m}`；
   - 跳过（计 failed）当前正在写入的录像段；拒绝含 `..` 的路径。
2. **`GET /api/files`** 分页统一：`?type=all|photos|recordings&offset=&limit=`（limit≤200），
   响应含 `total`；ai-thinker 旧实现 type=all 时 offset 只作用于照片段（翻页重复/漏项）已修。
3. **`DELETE /api/files?name=&type=photo|recording`**：新增 `type`（缺省 photo 兼容）；
   ai-thinker 旧实现删录像必失败（只实现了照片删除）已修。
4. **`POST /api/format`**（sd 能力板，鉴权）：
   - seeed：运行时格式化（停录→格式化→恢复，既有实现不变）；
   - ai-thinker：**申请-重启-开机格式化**——置 NVS 请求 → 应答后 2s 重启 →
     开机在相机初始化之前格式化（GPIO14 相机/SD 共享总线，相机运行中格式化必挂死，
     旧实现因此直接 503 且**未鉴权**，均已修）。
   - 响应均为 `{"message":...}`。
5. **`/api/status` 存储字段对齐**：ai-thinker 补齐 seeed 先例字段
   `sd_present`/`sd_total_bytes`/`sd_free_bytes`/`sd_free_percent`/`recording`
   （此前统一 SPA 在 ai-thinker 上显示"未检测到 SD 卡"而文件列表正常）。
6. **config `cleanup_low_pct`/`cleanup_high_pct` 语义统一为"空闲百分比"**
   （触发：free% < low；停止：free% >= high）。ai-thinker 旧语义为"已用百分比"，
   同样数值含义与 seeed 相反（线上曾配置 80/30，实际含义是"已用>80% 触发、
   删到已用<30% 为止"，会清掉近半卡内容），V16 迁移重置为家族默认 20/30。
7. SPA 存储页新增：类型筛选、分页（加载更多）、勾选批量删除、按类型清空、
   格式化（双重确认）；串流页新增 MJPEG/RTSP 地址只读展示。

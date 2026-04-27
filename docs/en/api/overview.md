# API 概述

> ESP32-S3 摄像头监控固件 REST API 文档

[状态查询](status.md) | [配置接口](config.md) | [文件管理](files.md) | [设备控制](control.md) | [视频流](stream.md) | [WiFi 扫描](wifi.md) | [完整示例](examples.md)

---

本固件基于 `web_server.c`、`mjpeg_streamer.c`、`config_manager.c` 源码编写，最后更新：2026-04-24。

## 基础地址

| 模式 | 地址 |
|------|------|
| AP 模式（默认） | `http://192.168.4.1` |
| STA 模式 | `http://<设备IP>` |

## 认证机制

部分接口需要密码认证，支持两种方式传递密码：

| 方式 | 格式 | 示例 |
|------|------|------|
| 请求头 | `X-Password: <密码>` | `X-Password: admin` |
| 查询参数 | `?password=<密码>` | `?password=admin` |

- **默认密码**：`admin`（可通过 `POST /api/config` 修改 `web_password` 字段）
- 认证逻辑优先检查 `X-Password` 请求头，其次检查 `password` 查询参数
- 认证失败返回 `401 Unauthorized`，响应体：`{"ok": false, "error": "Unauthorized"}`

## 统一响应格式

所有 API 接口返回 `Content-Type: application/json`，格式统一为：

**成功响应**：
```json
{
  "ok": true,
  "data": { ... }
}
```

**错误响应**：
```json
{
  "ok": false,
  "error": "错误描述信息"
}
```

> 注意：`POST /api/config` 成功时返回 `{"ok": true}`，不含 `data` 字段。

## CORS 跨域支持

所有 HTTP 响应（包括错误响应和静态文件）均包含以下 CORS 头：

```
Access-Control-Allow-Origin: *
Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS
Access-Control-Allow-Headers: Content-Type, X-Password
```

`OPTIONS` 请求（预检请求）返回上述 CORS 头和空响应体，状态码 200。

## 接口总览

| # | 方法 | 路径 | 认证 | 描述 |
|---|------|------|------|------|
| 1 | GET | `/api/status` | 否 | 获取设备状态 |
| 2 | GET | `/api/config` | 否 | 获取当前配置 |
| 3 | POST | `/api/config` | **是** | 更新配置 |
| 4 | GET | `/api/files` | 否 | 获取录制文件列表 |
| 5 | DELETE | `/api/files?name=xxx` | **是** | 删除指定文件 |
| 6 | GET | `/api/download?name=xxx` | 否 | 下载指定文件 |
| 7 | GET | `/api/scan` | 否 | 扫描 WiFi 网络 |
| 8 | POST | `/api/time` | **是** | 手动设置系统时间 |
| 9 | POST | `/api/record?action=start\|stop` | **是** | 控制录像 |
| 10 | POST | `/api/reset` | **是** | 恢复出厂设置 |
| 11 | GET | `/stream` | 否 | MJPEG 实时视频流 |
| 12 | OPTIONS | `/*` | 否 | CORS 预检请求 |
| 13 | GET | `/*` | 否 | 静态文件（SPIFFS） |

## HTTP 状态码

| 状态码 | 含义 | 触发场景 |
|--------|------|----------|
| 200 | 成功 | 请求处理成功 |
| 400 | 请求错误 | 参数缺失、JSON 格式错误、路径遍历检测 |
| 401 | 认证失败 | 密码错误或未提供密码（需要认证的接口） |
| 404 | 资源不存在 | 文件不存在、静态文件未找到 |
| 500 | 服务器内部错误 | WiFi 扫描失败、时间设置失败 |
| 503 | 服务不可用 | MJPEG 流客户端连接数已达上限 |

## 附录

### SD 卡路径

| 用途 | 路径 |
|------|------|
| 录制文件 | `/sdcard/recordings/` |
| WiFi 配置覆盖 | `/sdcard/config/wifi.txt` |
| NAS 配置覆盖 | `/sdcard/config/nas.txt` |

### 配置优先级

```
SD 卡配置文件 > NVS 存储 > 编译时默认值
```

### SPIFFS 分区

Web 界面静态文件存储在 SPIFFS 分区（约 256KB），路径前缀 `/spiffs/`。

### 服务器配置

| 参数 | 值 |
|------|-----|
| 端口 | 80 |
| 最大 URI 处理器 | 20 |
| 任务栈大小 | 8192 字节 |
| 接收超时 | 30 秒 |
| 发送超时 | 30 秒 |
| URI 匹配模式 | 通配符（wildcard） |

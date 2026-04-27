# 设备控制接口

> [← 文件管理](files.md) | [视频流 →](stream.md)

---

## POST /api/record?action=start|stop — 控制录像

手动开始或停止视频录制。

**认证**：需要密码认证

**源码**：`api_record_handler`（web_server.c）

**查询参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `action` | string | 是 | 操作类型：`"start"` 开始录像、`"stop"` 停止录像 |

**成功响应（开始录像）**：
```json
{
  "ok": true,
  "data": {
    "action": "start",
    "status": "recording"
  }
}
```

**成功响应（停止录像）**：
```json
{
  "ok": true,
  "data": {
    "action": "stop",
    "status": "stopped"
  }
}
```

**响应字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `action` | string | 回显请求的 action 参数值 |
| `status` | string | 操作结果状态，详见下表 |

**status 可能值**：

| status 值 | 触发条件 |
|-----------|----------|
| `"recording"` | `action=start` 且录像启动成功 |
| `"stopped"` | `action=stop` 且录像停止成功 |
| `"error"` | `action=start` 或 `action=stop` 操作执行失败（如无 SD 卡） |
| `"unknown_action"` | action 参数不是 `"start"` 也不是 `"stop"` |

> 即使返回 `"error"` 或 `"unknown_action"`，HTTP 状态码仍为 200。
> 只有认证失败时才返回 401。

**错误响应**：

| 状态码 | 条件 | 错误信息 |
|--------|------|----------|
| 401 | 未提供正确密码 | `"Unauthorized"` |

**cURL 示例**：
```bash
# 开始录像
curl -X POST "http://192.168.4.1/api/record?action=start" \
  -H "X-Password: admin"

# 停止录像
curl -X POST "http://192.168.4.1/api/record?action=stop" \
  -H "X-Password: admin"
```

**JavaScript 示例**：
```javascript
async function toggleRecording(start) {
  const action = start ? 'start' : 'stop';
  const resp = await fetch(`/api/record?action=${action}`, {
    method: 'POST',
    headers: { 'X-Password': 'admin' }
  });
  const { data } = await resp.json();
  console.log(`操作: ${data.action}, 状态: ${data.status}`);
  return data.status;
}
```

---

## POST /api/time — 手动设置时间

手动设置设备系统时间。在无法通过 NTP 自动同步时（如 AP 模式），可通过此接口手动校时。

**认证**：需要密码认证

**源码**：`api_time_handler`（web_server.c）

**请求体**（6 个字段全部必填）：
```json
{
  "year": 2026,
  "month": 4,
  "day": 24,
  "hour": 14,
  "min": 30,
  "sec": 0
}
```

**请求字段说明**：

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `year` | number | 是 | 年份（如 2026） |
| `month` | number | 是 | 月份（1-12） |
| `day` | number | 是 | 日期（1-31） |
| `hour` | number | 是 | 小时（0-23） |
| `min` | number | 是 | 分钟（0-59） |
| `sec` | number | 是 | 秒（0-59） |

> 六个字段缺一不可，缺少任意字段将返回 400 错误。

**成功响应**：
```json
{
  "ok": true
}
```

**错误响应**：

| 状态码 | 条件 | 错误信息 |
|--------|------|----------|
| 401 | 未提供正确密码 | `"Unauthorized"` |
| 400 | 请求体为空（超过 512 字节限制） | `"Empty body"` |
| 400 | JSON 解析失败 | `"Invalid JSON"` |
| 400 | 缺少时间字段 | `"Missing time fields"` |
| 500 | 设置时间失败 | `"Failed to set time"` |

**cURL 示例**：
```bash
curl -X POST http://192.168.4.1/api/time \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"year": 2026, "month": 4, "day": 24, "hour": 14, "min": 30, "sec": 0}'
```

**JavaScript 示例**：
```javascript
async function setDeviceTime(date) {
  const resp = await fetch('/api/time', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json',
      'X-Password': 'admin'
    },
    body: JSON.stringify({
      year: date.getFullYear(),
      month: date.getMonth() + 1,
      day: date.getDate(),
      hour: date.getHours(),
      min: date.getMinutes(),
      sec: date.getSeconds()
    })
  });
  return await resp.json();
}

// 将浏览器当前时间同步到设备
setDeviceTime(new Date());
```

---

## POST /api/reset — 恢复出厂设置

将设备配置恢复为出厂默认值并重启。配置立即重置，设备在发送响应后重启。

**认证**：需要密码认证

**源码**：`api_reset_handler`（web_server.c）

**请求体**：无

**响应**：
```json
{
  "ok": true,
  "data": {
    "message": "Rebooting..."
  }
}
```

> **注意**：设备在发送此响应后立即执行重启。客户端收到此响应后应预期连接断开。
> 重启后设备将使用默认配置启动（默认进入 AP 模式，密码恢复为 `admin`）。

**出厂默认值**：

| 配置项 | 默认值 |
|--------|--------|
| wifi_ssid | `""` (AP 模式) |
| wifi_pass | `""` |
| device_name | `"ParrotCam"` |
| web_password | `"admin"` |
| resolution | `1` (SVGA) |
| fps | `10` |
| segment_sec | `300` |
| jpeg_quality | `12` |
| ftp_enabled | `false` |
| webdav_enabled | `false` |

**错误响应**：

| 状态码 | 条件 | 错误信息 |
|--------|------|----------|
| 401 | 未提供正确密码 | `"Unauthorized"` |

**cURL 示例**：
```bash
curl -X POST http://192.168.4.1/api/reset \
  -H "X-Password: admin"
```

**JavaScript 示例**：
```javascript
async function factoryReset() {
  if (!confirm('确定要恢复出厂设置吗？设备将重启。')) return;
  const resp = await fetch('/api/reset', {
    method: 'POST',
    headers: { 'X-Password': 'admin' }
  });
  const result = await resp.json();
  if (result.ok) {
    console.log('设备正在重启...');
    // 等待设备重启后重新连接
    setTimeout(() => window.location.reload(), 10000);
  }
}
```

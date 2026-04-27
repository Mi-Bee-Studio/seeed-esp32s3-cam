# 文件管理接口

> [← 配置接口](config.md) | [设备控制 →](control.md)

---

## GET /api/files — 获取录制文件列表

获取 SD 卡 `/sdcard/recordings/` 目录下的录制文件列表。

**认证**：无需认证

**源码**：`api_files_get_handler`（web_server.c）

**响应示例**：
```json
{
  "ok": true,
  "data": {
    "files": [
      {
        "name": "20260424_120000.avi",
        "size": 10485760,
        "date": "2026-04-24 12:00:00"
      },
      {
        "name": "20260424_130000.avi",
        "size": 5242880,
        "date": "2026-04-24 13:00:00"
      }
    ]
  }
}
```

**响应字段说明**：

| 字段 | 类型 | 说明 |
|------|------|------|
| `files` | array | 文件信息数组，最多返回 64 个文件 |
| `files[].name` | string | 文件名 |
| `files[].size` | number | 文件大小（字节） |
| `files[].date` | string | 文件日期/时间字符串 |

> 无文件或 SD 卡未插入时返回空数组：`{"ok": true, "data": {"files": []}}`

**cURL 示例**：
```bash
curl http://192.168.4.1/api/files
```

**JavaScript 示例**：
```javascript
const resp = await fetch('/api/files');
const { data } = await resp.json();
data.files.forEach(f => {
  console.log(`${f.name} — ${(f.size / 1048576).toFixed(1)} MB — ${f.date}`);
});
```

---

## DELETE /api/files?name=xxx — 删除文件

删除 SD 卡上指定的录制文件。

**认证**：需要密码认证

**源码**：`api_files_delete_handler`（web_server.c）

**查询参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | 要删除的文件名（不含路径前缀） |

**安全机制**：
- 文件名中包含 `..` 的请求将被拒绝（防止路径遍历攻击）
- 实际删除路径为 `/sdcard/recordings/<name>`

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
| 400 | 缺少查询参数 | `"Missing query"` |
| 400 | 缺少 name 参数 | `"Missing name parameter"` |
| 400 | 文件名包含 `..` | `"Invalid name"` |
| 404 | 文件不存在或删除失败 | `"File not found or delete failed"` |

**cURL 示例**：
```bash
curl -X DELETE "http://192.168.4.1/api/files?name=20260424_120000.avi" \
  -H "X-Password: admin"
```

**JavaScript 示例**：
```javascript
async function deleteFile(filename) {
  const resp = await fetch(
    `/api/files?name=${encodeURIComponent(filename)}`,
    {
      method: 'DELETE',
      headers: { 'X-Password': 'admin' }
    }
  );
  return await resp.json();
}
```

---

## GET /api/download?name=xxx — 下载文件

下载 SD 卡上指定的录制文件，返回 AVI 格式的二进制数据。

**认证**：无需认证

**源码**：`api_download_handler`（web_server.c）

**查询参数**：

| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | 要下载的文件名（不含路径前缀） |

**安全机制**：
- 文件名中包含 `..` 的请求将被拒绝（防止路径遍历攻击）
- 实际下载路径为 `/sdcard/recordings/<name>`

**响应头**：
```
Content-Type: video/avi
Content-Length: <文件大小>
Content-Disposition: attachment; filename="<文件名>"
Access-Control-Allow-Origin: *
```

**响应体**：二进制 AVI 数据，以 1024 字节分块传输。

**错误响应**：

| 状态码 | 条件 | 错误信息 |
|--------|------|----------|
| 400 | 缺少查询参数 | `"Missing query"` |
| 400 | 缺少 name 参数 | `"Missing name parameter"` |
| 400 | 文件名包含 `..` | `"Invalid name"` |
| 404 | 文件不存在 | `"File not found"` |

**cURL 示例**：
```bash
# 下载并保存为本地文件
curl -o video.avi "http://192.168.4.1/api/download?name=20260424_120000.avi"
```

**JavaScript 示例**：
```javascript
async function downloadFile(filename) {
  const resp = await fetch(
    `/api/download?name=${encodeURIComponent(filename)}`
  );
  if (!resp.ok) throw new Error(`下载失败: ${resp.status}`);
  const blob = await resp.blob();
  // 创建下载链接
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}
```

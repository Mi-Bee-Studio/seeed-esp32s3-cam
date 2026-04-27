# File Management Interface

> [← Configuration Interface](config.md) | [Device Control →](control.md)

---

## GET /api/files — Get Recording File List

Get recording file list under SD card `/sdcard/recordings/` directory.

**Authentication**: None required

**Source**: `api_files_get_handler` (web_server.c)

**Response Example**:
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

**Response Field Description**:

| Field | Type | Description |
|-------|------|-------------|
| `files` | array | File info array, returns max 64 files |
| `files[].name` | string | Filename |
| `files[].size` | number | File size (bytes) |
| `files[].date` | string | File date/time string |

> Returns empty array when no files or SD card not inserted: `{"ok": true, "data": {"files": []}}`

**cURL Example**:
```bash
curl http://192.168.4.1/api/files
```

**JavaScript Example**:
```javascript
const resp = await fetch('/api/files');
const { data } = await resp.json();
data.files.forEach(f => {
  console.log(`${f.name} — ${(f.size / 1048576).toFixed(1)} MB — ${f.date}`);
});
```

---

## DELETE /api/files?name=xxx — Delete File

Delete specified recording file on SD card.

**Authentication**: Password required

**Source**: `api_files_delete_handler` (web_server.c)

**Query Parameters**:

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | Yes | Filename to delete (without path prefix) |

**Security Mechanism**:
- Requests with `..` in filename are rejected (prevents path traversal attacks)
- Actual delete path is `/sdcard/recordings/<name>`

**Success Response**:
```json
{
  "ok": true
}
```

**Error Responses**:

| Status Code | Condition | Error Message |
|-------------|-----------|---------------|
| 401 | Wrong or missing password | `"Unauthorized"` |
| 400 | Missing query parameter | `"Missing query"` |
| 400 | Missing name parameter | `"Missing name parameter"` |
| 400 | Filename contains `..` | `"Invalid name"` |
| 404 | File does not exist or delete failed | `"File not found or delete failed"` |

**cURL Example**:
```bash
curl -X DELETE "http://192.168.4.1/api/files?name=20260424_120000.avi" \
  -H "X-Password: admin"
```

**JavaScript Example**:
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

## GET /api/download?name=xxx — Download File

Download specified recording file on SD card, returns AVI format binary data.

**Authentication**: None required

**Source**: `api_download_handler` (web_server.c)

**Query Parameters**:

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `name` | string | Yes | Filename to download (without path prefix) |

**Security Mechanism**:
- Requests with `..` in filename are rejected (prevents path traversal attacks)
- Actual download path is `/sdcard/recordings/<name>`

**Response Headers**:
```
Content-Type: video/avi
Content-Length: <file size>
Content-Disposition: attachment; filename="<filename"
Access-Control-Allow-Origin: *
```

**Response Body**: Binary AVI data, transferred in 1024-byte chunks.

**Error Responses**:

| Status Code | Condition | Error Message |
|-------------|-----------|---------------|
| 400 | Missing query parameter | `"Missing query"` |
| 400 | Missing name parameter | `"Missing name parameter"` |
| 400 | Filename contains `..` | `"Invalid name"` |
| 404 | File does not exist | `"File not found"` |

**cURL Example**:
```bash
# Download and save as local file
curl -o video.avi "http://192.168.4.1/api/download?name=20260424_120000.avi"
```

**JavaScript Example**:
```javascript
async function downloadFile(filename) {
  const resp = await fetch(
    `/api/download?name=${encodeURIComponent(filename)}`
  );
  if (!resp.ok) throw new Error(`Download failed: ${resp.status}`);
  const blob = await resp.blob();
  // Create download link
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  a.click();
  URL.revokeObjectURL(url);
}
```
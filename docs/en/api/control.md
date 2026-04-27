# Device Control Interface

> [← File Management](files.md) | [Video Stream →](stream.md)

---

## POST /api/record?action=start|stop — Control Recording

Manually start or stop video recording.

**Authentication**: Password required

**Source**: `api_record_handler` (web_server.c)

**Query Parameters**:

| Parameter | Type | Required | Description |
|-----------|------|----------|-------------|
| `action` | string | Yes | Action type: `"start"` to start recording, `"stop"` to stop recording |

**Success Response (Start Recording)**:
```json
{
  "ok": true,
  "data": {
    "action": "start",
    "status": "recording"
  }
}
```

**Success Response (Stop Recording)**:
```json
{
  "ok": true,
  "data": {
    "action": "stop",
    "status": "stopped"
  }
}
```

**Response Field Description**:

| Field | Type | Description |
|-------|------|-------------|
| `action` | string | Echoes the requested action parameter value |
| `status` | string | Operation result status, see table below |

**Status Possible Values**:

| Status Value | Trigger Condition |
|--------------|-------------------|
| `"recording"` | `action=start` and recording started successfully |
| `"stopped"` | `action=stop` and recording stopped successfully |
| `"error"` | `action=start` or `action=stop` operation failed (e.g., no SD card) |
| `"unknown_action"` | action parameter is neither `"start"` nor `"stop"` |

> Even if `"error"` or `"unknown_action"` is returned, HTTP status code is still 200.
> Only authentication failure returns 401.

**Error Responses**:

| Status Code | Condition | Error Message |
|-------------|-----------|---------------|
| 401 | Wrong or missing password | `"Unauthorized"` |

**cURL Examples**:
```bash
# Start recording
curl -X POST "http://192.168.4.1/api/record?action=start" \
  -H "X-Password: admin"

# Stop recording
curl -X POST "http://192.168.4.1/api/record?action=stop" \
  -H "X-Password: admin"
```

**JavaScript Example**:
```javascript
async function toggleRecording(start) {
  const action = start ? 'start' : 'stop';
  const resp = await fetch(`/api/record?action=${action}`, {
    method: 'POST',
    headers: { 'X-Password': 'admin' }
  });
  const { data } = await resp.json();
  console.log(`Action: ${data.action}, Status: ${data.status}`);
  return data.status;
}
```

---

## POST /api/time — Manually Set Time

Manually set device system time. When NTP auto-sync is not possible (e.g., AP mode), use this endpoint to manually calibrate time.

**Authentication**: Password required

**Source**: `api_time_handler` (web_server.c)

**Request Body** (all 6 fields required):
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

**Request Field Description**:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `year` | number | Yes | Year (e.g., 2026) |
| `month` | number | Yes | Month (1-12) |
| `day` | number | Yes | Day (1-31) |
| `hour` | number | Yes | Hour (0-23) |
| `min` | number | Yes | Minute (0-59) |
| `sec` | number | Yes | Second (0-59) |

> All six fields are required. Missing any field returns 400 error.

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
| 400 | Empty body (exceeds 512 byte limit) | `"Empty body"` |
| 400 | JSON parsing failed | `"Invalid JSON"` |
| 400 | Missing time fields | `"Missing time fields"` |
| 500 | Failed to set time | `"Failed to set time"` |

**cURL Example**:
```bash
curl -X POST http://192.168.4.1/api/time \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"year": 2026, "month": 4, "day": 24, "hour": 14, "min": 30, "sec": 0}'
```

**JavaScript Example**:
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

// Sync browser current time to device
setDeviceTime(new Date());
```

---

## POST /api/reset — Factory Reset

Restore device configuration to factory default values and reboot. Configuration is reset immediately, device reboots after sending response.

**Authentication**: Password required

**Source**: `api_reset_handler` (web_server.c)

**Request Body**: None

**Response**:
```json
{
  "ok": true,
  "data": {
    "message": "Rebooting..."
  }
}
```

> **Note**: Device executes reboot immediately after sending this response. Client should expect connection disconnect after receiving this response.
> After reboot, device will start with default configuration (default AP mode, password restored to `admin`).

**Factory Default Values**:

| Config Item | Default |
|-------------|---------|
| wifi_ssid | `""` (AP mode) |
| wifi_pass | `""` |
| device_name | `"MiBeeHomeCam"` |
| web_password | `"admin"` |
| resolution | `1` (SVGA) |
| fps | `10` |
| segment_sec | `300` |
| jpeg_quality | `12` |
| ftp_enabled | `false` |
| webdav_enabled | `false` |

**Error Responses**:

| Status Code | Condition | Error Message |
|-------------|-----------|---------------|
| 401 | Wrong or missing password | `"Unauthorized"` |

**cURL Example**:
```bash
curl -X POST http://192.168.4.1/api/reset \
  -H "X-Password: admin"
```

**JavaScript Example**:
```javascript
async function factoryReset() {
  if (!confirm('Are you sure you want to factory reset? Device will reboot.')) return;
  const resp = await fetch('/api/reset', {
    method: 'POST',
    headers: { 'X-Password': 'admin' }
  });
  const result = await resp.json();
  if (result.ok) {
    console.log('Device is rebooting...');
    // Wait for device to reboot then reconnect
    setTimeout(() => window.location.reload(), 10000);
  }
}
```
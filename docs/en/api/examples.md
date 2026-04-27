# Complete Examples

> [← WiFi Scan](wifi.md) | [API Overview](overview.md)

---

## JavaScript API Client封装

```javascript
class MiBeeHomeCam API {
  constructor(baseURL = '', password = 'admin') {
    this.base = baseURL;
    this.password = password;
  }

  get headers() {
    return {
      'Content-Type': 'application/json',
      'X-Password': this.password
    };
  }

  async getStatus() {
    const resp = await fetch(`${this.base}/api/status`);
    return resp.json();
  }

  async getConfig() {
    const resp = await fetch(`${this.base}/api/config`);
    return resp.json();
  }

  async updateConfig(fields) {
    const resp = await fetch(`${this.base}/api/config`, {
      method: 'POST',
      headers: this.headers,
      body: JSON.stringify(fields)
    });
    return resp.json();
  }

  async getFiles() {
    const resp = await fetch(`${this.base}/api/files`);
    return resp.json();
  }

  async deleteFile(name) {
    const resp = await fetch(
      `${this.base}/api/files?name=${encodeURIComponent(name)}`,
      { method: 'DELETE', headers: this.headers }
    );
    return resp.json();
  }

  async downloadFile(name) {
    const resp = await fetch(
      `${this.base}/api/download?name=${encodeURIComponent(name)}`
    );
    return resp.blob();
  }

  async scanWifi() {
    const resp = await fetch(`${this.base}/api/scan`);
    return resp.json();
  }

  async setTime(date) {
    const resp = await fetch(`${this.base}/api/time`, {
      method: 'POST',
      headers: this.headers,
      body: JSON.stringify({
        year: date.getFullYear(),
        month: date.getMonth() + 1,
        day: date.getDate(),
        hour: date.getHours(),
        min: date.getMinutes(),
        sec: date.getSeconds()
      })
    });
    return resp.json();
  }

  async startRecording() {
    const resp = await fetch(`${this.base}/api/record?action=start`, {
      method: 'POST',
      headers: this.headers
    });
    return resp.json();
  }

  async stopRecording() {
    const resp = await fetch(`${this.base}/api/record?action=stop`, {
      method: 'POST',
      headers: this.headers
    });
    return resp.json();
  }

  async factoryReset() {
    const resp = await fetch(`${this.base}/api/reset`, {
      method: 'POST',
      headers: this.headers
    });
    return resp.json();
  }

  getStreamURL() {
    return `${this.base}/stream`;
  }
}
```

## JavaScript Usage Examples

```javascript
const cam = new MiBeeHomeCamAPI('http://192.168.4.1', 'admin');

// Get and display status
async function showStatus() {
  const { data } = await cam.getStatus();
  console.log(`Recording: ${data.recording}`);
  console.log(`WiFi: ${data.wifi_state} (${data.ip})`);
  console.log(`Storage: ${data.sd_free_percent}% free`);
  console.log(`Uptime: ${Math.floor(data.uptime / 3600)}h ${Math.floor((data.uptime % 3600) / 60)}m`);
}

// Configure WiFi and start recording
async function quickSetup(ssid, pass) {
  await cam.updateConfig({ wifi_ssid: ssid, wifi_pass: pass });
  console.log('WiFi configured, device will connect to network');
}

// Download all recording files
async function downloadAll() {
  const { data } = await cam.getFiles();
  for (const file of data.files) {
    console.log(`Downloading: ${file.name} (${(file.size / 1048576).toFixed(1)} MB)`);
    const blob = await cam.downloadFile(file.name);
    // Process downloaded file...
  }
}
```

---

## cURL Command Manual

### Device Status and Configuration

```bash
# View device status
curl http://192.168.4.1/api/status

# View current configuration
curl http://192.168.4.1/api/config

# Modify device name
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"device_name": "KitchenCam"}'

# Configure WiFi connection (effective after reboot)
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"wifi_ssid": "HomeWiFi", "wifi_pass": "wifipassword"}'
```

### Recording Control

```bash
# Start recording
curl -X POST "http://192.168.4.1/api/record?action=start" \
  -H "X-Password: admin"

# Stop recording
curl -X POST "http://192.168.4.1/api/record?action=stop" \
  -H "X-Password: admin"
```

### File Management

```bash
# List all recording files
curl http://192.168.4.1/api/files

# Download specified file
curl -o recording.avi "http://192.168.4.1/api/download?name=20260424_120000.avi"

# Delete specified file
curl -X DELETE "http://192.168.4.1/api/files?name=20260424_120000.avi" \
  -H "X-Password: admin"
```

### Network and Time

```bash
# Scan WiFi
curl http://192.168.4.1/api/scan

# Manually set time
curl -X POST http://192.168.4.1/api/time \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"year": 2026, "month": 4, "day": 24, "hour": 14, "min": 30, "sec": 0}'
```

### NAS Upload Configuration

```bash
# Configure FTP upload
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{
    "ftp_host": "192.168.1.200",
    "ftp_port": 21,
    "ftp_user": "camuser",
    "ftp_pass": "camsecret",
    "ftp_path": "/MiBeeHomeCam",
    "ftp_enabled": true
  }'

# Configure WebDAV upload
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{
    "webdav_url": "https://dav.example.com/MiBeeHomeCam",
    "webdav_user": "davuser",
    "webdav_pass": "davsecret",
    "webdav_enabled": true
  }'
```

### Camera Parameters

```bash
# Set to XGA resolution, 15 FPS
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"resolution": 2, "fps": 15}'

# Set high quality JPEG (lower value = higher quality)
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"jpeg_quality": 6}'
```

### Factory Reset

```bash
curl -X POST http://192.168.4.1/api/reset \
  -H "X-Password: admin"
```
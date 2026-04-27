# 完整示例

> [← WiFi 扫描](wifi.md) | [API 概述](overview.md)

---

## JavaScript API 客户端封装

```javascript
class ParrotCamAPI {
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

## JavaScript 使用示例

```javascript
const cam = new ParrotCamAPI('http://192.168.4.1', 'admin');

// 获取并显示状态
async function showStatus() {
  const { data } = await cam.getStatus();
  console.log(`录像: ${data.recording}`);
  console.log(`WiFi: ${data.wifi_state} (${data.ip})`);
  console.log(`存储: ${data.sd_free_percent}% 可用`);
  console.log(`运行: ${Math.floor(data.uptime / 3600)}h ${Math.floor((data.uptime % 3600) / 60)}m`);
}

// 配置 WiFi 并开始录像
async function quickSetup(ssid, pass) {
  await cam.updateConfig({ wifi_ssid: ssid, wifi_pass: pass });
  console.log('WiFi 已配置，设备将连接网络');
}

// 下载所有录制文件
async function downloadAll() {
  const { data } = await cam.getFiles();
  for (const file of data.files) {
    console.log(`正在下载: ${file.name} (${(file.size / 1048576).toFixed(1)} MB)`);
    const blob = await cam.downloadFile(file.name);
    // 处理下载的文件...
  }
}
```

---

## cURL 命令手册

### 设备状态与配置

```bash
# 查看设备状态
curl http://192.168.4.1/api/status

# 查看当前配置
curl http://192.168.4.1/api/config

# 修改设备名称
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"device_name": "KitchenCam"}'

# 配置 WiFi 连接（重启后生效）
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"wifi_ssid": "HomeWiFi", "wifi_pass": "wifipassword"}'
```

### 录像控制

```bash
# 开始录像
curl -X POST "http://192.168.4.1/api/record?action=start" \
  -H "X-Password: admin"

# 停止录像
curl -X POST "http://192.168.4.1/api/record?action=stop" \
  -H "X-Password: admin"
```

### 文件管理

```bash
# 列出所有录制文件
curl http://192.168.4.1/api/files

# 下载指定文件
curl -o recording.avi "http://192.168.4.1/api/download?name=20260424_120000.avi"

# 删除指定文件
curl -X DELETE "http://192.168.4.1/api/files?name=20260424_120000.avi" \
  -H "X-Password: admin"
```

### 网络与时间

```bash
# 扫描 WiFi
curl http://192.168.4.1/api/scan

# 手动设置时间
curl -X POST http://192.168.4.1/api/time \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"year": 2026, "month": 4, "day": 24, "hour": 14, "min": 30, "sec": 0}'
```

### NAS 上传配置

```bash
# 配置 FTP 上传
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{
    "ftp_host": "192.168.1.200",
    "ftp_port": 21,
    "ftp_user": "camuser",
    "ftp_pass": "camsecret",
    "ftp_path": "/ParrotCam",
    "ftp_enabled": true
  }'

# 配置 WebDAV 上传
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{
    "webdav_url": "https://dav.example.com/ParrotCam",
    "webdav_user": "davuser",
    "webdav_pass": "davsecret",
    "webdav_enabled": true
  }'
```

### 摄像头参数

```bash
# 设置为 XGA 分辨率、15 FPS
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"resolution": 2, "fps": 15}'

# 设置高质量 JPEG（数值越小质量越高）
curl -X POST http://192.168.4.1/api/config \
  -H "Content-Type: application/json" \
  -H "X-Password: admin" \
  -d '{"jpeg_quality": 6}'
```

### 恢复出厂设置

```bash
curl -X POST http://192.168.4.1/api/reset \
  -H "X-Password: admin"
```

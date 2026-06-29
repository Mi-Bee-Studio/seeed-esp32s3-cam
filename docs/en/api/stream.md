# GET /stream — MJPEG Real-time Video Stream

> [← Device Control](control.md) | [WiFi Scan →](wifi.md)

---

Get MJPEG real-time video stream from camera. Browsers can directly use this endpoint as `src` attribute of `<img>` tag.

> Note: MJPEG stream runs on **port 81** (independent TCP server), while the web interface runs on port 80. The stream URL is `http://<deviceIP>:81/stream`.

**Authentication**: None required

**Source**: `mjpeg_stream_handler` (mjpeg_streamer.c)

**Response Headers**:
```
Content-Type: multipart/x-mixed-replace; boundary=frame
Access-Control-Allow-Origin: *
```

**Response Body Format**:

Stream response consists of consecutive JPEG frames, each frame format:
```
\r\n--frame\r\n
Content-Type: image/jpeg\r\n
Content-Length: <frame data bytes>\r\n
\r\n
<JPEG binary data>\r\n
```

**Limitations**:
- Supports maximum **2 concurrent client** connections
- Returns `503 Service Unavailable` when connection limit exceeded, response body: `Max stream connections reached`
- Frame rate controlled by `fps` config (default 10 FPS)
- Each JPEG frame data sent in 4096-byte chunks
- Resources automatically released when client disconnects

**cURL Example**:
```bash
# Save one frame as JPEG (auto disconnects after timeout)
curl --max-time 1 http://192.168.4.1:81/stream > frame.jpg 2>/dev/null
```

**HTML Embed Examples**:
```html
<!-- Simplest usage: directly embed in img tag -->
<img src="http://192.168.4.1:81/stream" style="width: 100%;" alt="Live feed">

<!-- Complete page with auto-reconnect -->
<!DOCTYPE html>
<html>
<head>
  <title>Live Feed</title>
</head>
<body>
  <img id="stream" src="/stream" style="max-width: 100%;"
       onerror="setTimeout(() => this.src='/stream?t='+Date.now(), 3000)">
</body>
</html>
```

**JavaScript Playback Control Example**:
```javascript
const img = document.getElementById('stream');

function startStream() {
  img.src = 'http://<deviceIP>:81/stream?t=' + Date.now();
}

function stopStream() {
  img.src = '';  // Disconnect stream
}

// Auto reconnect on disconnect
img.onerror = () => {
  setTimeout(startStream, 3000);
};
```

---

## GET /api/audio — HTTP Chunked Audio Stream

> [← MJPEG Stream](#get-stream--mjpeg-real-time-video-stream) | [Device Control →](control.md)

---

Get real-time G.711 μ-law audio stream via HTTP chunked transfer encoding. Used by the Web UI preview page for live audio monitoring.

**Authentication**: None required

**Source**: `audio_stream_handler` (web_server.c)

**Response Headers**:
```
Content-Type: application/octet-stream
Transfer-Encoding: chunked
Access-Control-Allow-Origin: *
```

**Response Body**: Continuous stream of G.711 μ-law encoded audio bytes (8 kHz, mono, 20 ms frames = 160 bytes each).

**Client-side Decoding**: The browser decodes μ-law to 16-bit PCM via a lookup table and plays via Web Audio API (`AudioContext` at 8 kHz sample rate).

**DSP Pipeline** (firmware-side):
1. I2S PDM capture (DSR_16S, 1.024 MHz PDM clock, hardware anti-aliased)
2. DC offset removal (IIR filter, α≈0.999)
3. Spectral noise subtraction (256-point FFT, Wiener gain, minimum-statistics noise estimation)
4. Software gain (4×) + hard clamp
5. Noise gate with hysteresis (open@300, close@150)
6. G.711 μ-law encoding

**cURL Example**:
```bash
# Capture 3 seconds of audio
curl --max-time 3 http://<deviceIP>/api/audio > audio.g711 2>/dev/null
```

---

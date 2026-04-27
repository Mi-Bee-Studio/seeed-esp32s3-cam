# GET /stream — MJPEG Real-time Video Stream

> [← Device Control](control.md) | [WiFi Scan →](wifi.md)

---

Get MJPEG real-time video stream from camera. Browsers can directly use this endpoint as `src` attribute of `<img>` tag.

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
curl --max-time 1 http://192.168.4.1/stream > frame.jpg 2>/dev/null
```

**HTML Embed Examples**:
```html
<!-- Simplest usage: directly embed in img tag -->
<img src="http://192.168.4.1/stream" style="width: 100%;" alt="Live feed">

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
  img.src = '/stream?t=' + Date.now();
}

function stopStream() {
  img.src = '';  // Disconnect stream
}

// Auto reconnect on disconnect
img.onerror = () => {
  setTimeout(startStream, 3000);
};
```
# GET /stream — MJPEG 实时视频流

> [← 设备控制](control.md) | [WiFi 扫描 →](wifi.md)

---

获取摄像头的 MJPEG 实时视频流。浏览器可直接将此端点作为 `<img>` 标签的 `src` 使用。

**认证**：无需认证

**源码**：`mjpeg_stream_handler`（mjpeg_streamer.c）

**响应头**：
```
Content-Type: multipart/x-mixed-replace; boundary=frame
Access-Control-Allow-Origin: *
```

**响应体格式**：

流式响应由连续的 JPEG 帧组成，每帧格式如下：
```
\r\n--frame\r\n
Content-Type: image/jpeg\r\n
Content-Length: <帧数据字节数>\r\n
\r\n
<JPEG 二进制数据>\r\n
```

**限制**：
- 最多支持 **2 个并发客户端**连接
- 超过连接数限制时返回 `503 Service Unavailable`，响应体：`Max stream connections reached`
- 帧率由配置项 `fps` 控制（默认 10 FPS）
- 每帧 JPEG 数据以 4096 字节分块发送
- 客户端断开连接时自动释放资源

**cURL 示例**：
```bash
# 将一帧保存为 JPEG（超时后自动断开）
curl --max-time 1 http://192.168.4.1/stream > frame.jpg 2>/dev/null
```

**HTML 嵌入示例**：
```html
<!-- 最简用法：直接嵌入 img 标签 -->
<img src="http://192.168.4.1/stream" style="width: 100%;" alt="实时画面">

<!-- 带自动重连的完整页面 -->
<!DOCTYPE html>
<html>
<head>
  <title>实时画面</title>
</head>
<body>
  <img id="stream" src="/stream" style="max-width: 100%;"
       onerror="setTimeout(() => this.src='/stream?t='+Date.now(), 3000)">
</body>
</html>
```

**JavaScript 播放控制示例**：
```javascript
const img = document.getElementById('stream');

function startStream() {
  img.src = '/stream?t=' + Date.now();
}

function stopStream() {
  img.src = '';  // 断开流连接
}

// 断线自动重连
img.onerror = () => {
  setTimeout(startStream, 3000);
};
```

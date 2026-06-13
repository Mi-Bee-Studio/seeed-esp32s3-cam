# 端口分离设计：为什么 MJPEG 流传输使用独立端口

## 背景

MiBee Cam 在不同端口上运行两个网络服务：

| 服务 | 端口 | 模块 | 职责 |
|------|------|------|------|
| REST API + Web UI | 80 | `web_server.c`（ESP-IDF httpd） | 配置、文件管理、OTA、状态查询 |
| MJPEG 实时预览 | 81 | `mjpeg_streamer.c`（原生 TCP） | 实时摄像头画面 |

这个分离不是随意的——它是为了确保系统稳定性而做出的关键架构决策。

## 根本原因：ESP-IDF httpd 是单线程的

ESP-IDF 的 `httpd` 组件（`esp_http_server`）运行在**单线程事件循环**上。它使用一个内部任务对所有已注册的 socket 调用 `select()`，然后逐一分发请求给处理函数。

这意味着：

1. **长连接处理函数会阻塞一切。** MJPEG 流（`multipart/x-mixed-replace`）本质上是一个长连接——处理函数必须持续发送 JPEG 帧，**永远不返回**，直到客户端断开。如果这放在 httpd 里运行，它会独占整个事件循环。

2. **Socket 池耗尽。** 端口 80 的 httpd 配置了 `max_open_sockets = 4`。两个 MJPEG 客户端就会消耗掉一半的可用连接，REST API 和 Web UI 只剩 2 个连接。

3. **无法控制优先级。** httpd 运行在 IDF 分配的默认优先级（通常较低）。MJPEG 流传输、REST API 调用、WebSocket 推送和文件下载都在同一个事件循环中平等竞争。无法让关键 API 响应优先于帧传输。

4. **网络压力下的级联故障。** ESP32-S3 的 WiFi 收发共用无线通道。当 MJPEG 流在大量发送帧数据时，任何 TCP 重传或 WiFi DELBA 事件（在嘈杂的 2.4GHz 环境中很常见）都会产生反压，使 httpd 事件循环停滞——导致整个 Web UI 和 API 无响应。

## 架构：端口 81 如何解决这个问题

### 独立 TCP 服务器

`mjpeg_streamer.c` 在端口 81 上创建自己的 TCP 服务器 socket，完全绕过 httpd：

```
mjpeg_streamer_init()
  └── socket() + bind(port 81) + listen()
  └── xTaskCreatePinnedToCore(mjpeg_listen_task, ..., Core 1, 优先级 3)
        └── accept() 循环
             └── 为每个客户端: xTaskCreatePinnedToCore(mjpeg_client_task, ..., Core 1, 优先级 2)
                  └── fbroadcast_receive() → send() 循环（分块 JPEG）
                  └── 断开时清理资源
```

### 关键设计属性

| 属性 | 端口 80（httpd） | 端口 81（mjpeg_streamer） |
|------|-----------------|--------------------------|
| 线程模型 | 单事件循环 | 每客户端一个 FreeRTOS 任务 |
| 最大连接数 | 4 个 socket | 2 个客户端（`MAX_STREAM_CLIENTS`） |
| 任务优先级 | httpd 默认（低） | 监听=3，客户端=2 |
| CPU 核心 | 默认 | 绑定到 Core 1 |
| 栈大小 | 16384 字节 | 每客户端任务 4096 字节 |
| 阻塞性 | 处理函数必须快速返回 | 客户端任务可阻塞在 `fbroadcast_receive()` |
| 慢客户端影响 | 阻塞整个 httpd | 仅影响该客户端的任务 |

### 帧分发管道

端口 80 和端口 81 都通过 `frame_broadcaster.c` 消费摄像头帧，但通过不同的机制：

```
Camera (OV2640)
  └── esp_camera_fb_get()  [camera_driver.c]
        │
video_recorder (Core 0, 优先级 5)       ← 唯一的帧生产者
  ├── camera_capture(&frame)
  ├── jpeg_copy = heap_caps_malloc(PSRAM)  ← 与摄像头缓冲区解耦
  ├── memcpy(jpeg_copy, frame.buf, ...)
  ├── camera_return_fb(&frame)             ← 尽快释放摄像头
  └── fbroadcast_publish(jpeg_copy, len)
        │
frame_broadcaster [frame_broadcaster.c]
  ├── 更新最新帧缓存（PSRAM）
  ├── 为每个订阅者：
  │     ├── 分配独立的 PSRAM 副本
  │     ├── 从深度为 1 的队列中排空旧帧（慢客户端丢弃）
  │     └── 将新帧推入订阅者队列
  │
  ├── 订阅者 1：mjpeg_streamer（端口 81）
  │     └── 在专用任务中 fbroadcast_receive()
  │
  └── 订阅者 2：（保留供未来使用，如 RTSP）
```

**关键细节**：广播器为每个订阅者分配独立的 PSRAM 副本。这意味着 MJPEG 客户端任务永远不会持有摄像头驱动缓冲区的引用——摄像头可以立即捕获下一帧，无需等待网络 I/O 完成。

## 如果不分离端口会怎样

如果 MJPEG 流传输由端口 80 的 httpd URI 处理函数处理：

1. **API 在负载下无响应。** 当向浏览器流传输帧时，httpd 事件循环被卡在流处理函数中。配置更改、文件下载、OTA 上传——全部排队等待。

2. **WebSocket 停止推送。** WebSocket 服务器（`ws_server.c`）注册在同一个 httpd 实例上。实时状态更新和运动事件会被延迟或丢失。

3. **无法优雅降级。** 一个慢速的 MJPEG 客户端（例如 WiFi 信号差导致 TCP 重传）会降低**所有**服务的质量，而不仅仅是该客户端的流。

4. **看门狗风险。** 如果 httpd 在卡住的 MJPEG 发送上阻塞时间过长，30 秒 TWDT 可能触发 panic 重启——整个设备因为一个客户端连接不良而崩溃重启。

## 资源预算

| 资源 | 端口 80（httpd） | 端口 81（mjpeg_streamer） | 合计 |
|------|-----------------|--------------------------|------|
| TCP socket | 最多 4 个 | 2 + 1 监听 | 7 |
| FreeRTOS 任务 | 1（httpd 内部） | 1 监听 + 最多 2 客户端 | 4 |
| 栈（DRAM） | 16384 字节 | 4096 × 3 = 12288 字节 | ~28 KB |
| 每帧 PSRAM | N/A | ~15-30 KB × 2 订阅者 | ~60 KB |

## 配置参考

- **流端口**：硬编码为 `STREAM_PORT 81`，在 `mjpeg_streamer.c` 中
- **最大流客户端数**：`MAX_STREAM_CLIENTS 2`，在 `mjpeg_streamer.c` 中
- **发送超时**：`SEND_TIMEOUT_MS 5000`——卡住的客户端自动断开
- **帧丢弃**：广播器使用深度为 1 的队列——慢客户端自动丢帧
- **httpd socket 数**：`config.max_open_sockets = 4`，在 `web_server.c` 中

## 相关文档

- [系统架构](architecture.md)——完整系统概览
- [API 参考](api/overview.md)——端口 80 上的 REST API 端点
- `main/mjpeg_streamer.c`——实现代码（381 行）
- `main/frame_broadcaster.c`——帧分发中心（252 行）

# MiBee Cam 性能优化分阶段实施计划

## 总览

基于三个 Explore agent 的深度分析（RTOS 任务架构 / 内存缓冲 / 流媒体编解码）和两个专项核查（SDMMC 硬件可行性 + 零拷贝方案 + TWDT 审计），形成 5 个**可独立验证**的优化阶段。按"风险递增、收益递减"排序，每阶段通过验证后再进入下一阶段。

涉及改动文件（共约 10 个）：
- 配置层：`sdkconfig.defaults`、`main/storage_manager.c`（SDMMC 切换）
- 核心数据流：`main/frame_broadcaster.{c,h}`、`main/audio_broadcaster.{c,h}`、`main/video_recorder.c`、`main/rtsp_server.cpp`
- 模块层：`main/motion_detector.{c,h}`、`main/camera_driver.c`、`main/web_server.c`、`main/storage_manager.c`、`main/main.c`
- 死代码清理：`main/video_recorder.{c,h}`、`main/storage_manager.c`

---

## 阶段 1：配置层零风险优化 + Bug 修复 ⚡

**目标**：仅改配置和修复明显 bug，零架构改动，立即可见收益。

### 1.1 `sdkconfig.defaults` 调整（4 项）

| 项 | 旧值 | 新值 | 依据 |
|---|---|---|---|
| `CONFIG_SPIRAM_SPEED_40M` | 40MHz | `CONFIG_SPIRAM_SPEED_80M=y` | `sdkconfig:1850` 当前 40MHz，Octal PSRAM 支持 80MHz，所有 memcpy 带宽翻倍 |
| `CONFIG_ESP_WIFI_AMPDU_TX_ENABLED` | `=n` | `=y` | `sdkconfig.defaults:65`，WiFi 聚合帧显著提升 MJPEG/RTSP 吞吐 |
| `CONFIG_ESP_WIFI_AMPDU_RX_ENABLED` | `=n` | `=y` | 同上 |
| `CONFIG_SD_MMC_1_LINE_MODE` | `=y`（死配置） | 删除 | 非 ESP-IDF v6.0 真实 Kconfig 符号，无任何效果，混淆后续维护 |

**注意**：改 `sdkconfig.defaults` 后需要 `idf.py fullclean && idf.py set-target esp32s3 && idf.py build`。PSRAM 80MHz 若不稳定可快速回滚。

### 1.2 Bug 修复（3 项）

**(a) `MAX_RTSP_CLIENTS` 生效**（`main/rtsp_server.cpp:67`）
当前 `MAX_RTSP_CLIENTS=2` 定义但从未在 accept 路径检查。需要在 espp `RtspServer` 的 accept 循环插入会话计数判断，超限时拒绝新连接。涉及修改 `patches/espp__rtsp/src/rtsp_server.cpp`（accept_task_function）+ 本地 `rtsp_server.cpp` 暴露计数接口。**或者**更简单：在本地 `video_feed_task` / `audio_feed_task` 启动前检查 `has_active_sessions()` 已达上限时直接不订阅。

**(b) 删除无效 TWDT reset**（`main/web_server.c:669`）
httpd worker 任务从未 `esp_task_wdt_add(NULL)`，此处 `esp_task_wdt_reset()` 返回 `ESP_ERR_NOT_FOUND` 静默失败。直接删除该调用（httpd 任务本就不受 TWDT 监控，删除无害）。

**(c) 删除死代码 `recorder_watchdog_feed`**（`main/video_recorder.c:1097-1100` + `.h:57`）
全仓库无调用者。

### 1.3 运动检测 JPEG 解码降采样（`main/motion_detector.c:181`）
`JPEG_IMAGE_SCALE_0` → `JPEG_IMAGE_SCALE_4X`。运动检测只需 80×60 灰度，全分辨率 RGB888 解码纯属浪费。单次解码耗时从 ~500ms 降到 ~30ms（1/16 像素）。`downsample_to_gray` 的块大小自动适配（SVGA: 10×10 → 2.5×2.5）。
**配套**：`video_recorder.c:788` 的 `drop_threshold_us` 可从 2000000 降回 500000，`video_recorder.c:993` recorder 栈可从 10240 试降到 6144（验证后定）。

### 1.4 验证方法
- `idf.py build` 通过
- 烧录后串口 60s 监控：heap/PSRAM 正常、无 panic、TWDT 无误触发
- Web UI 拉 MJPEG 流观察流畅度
- 启用动态延时模式验证运动检测仍正常触发（log 里 motion score 非零）
- 测试 RTSP 第 3 个客户端连接被拒

### 1.5 回滚
纯配置/局部 bug 修复，每项独立可回滚。

---

## 阶段 2：frame_broadcaster 零拷贝改造 🔧

**目标**：每帧 PSRAM 拷贝次数从 `1 + 1 + N`（典型 4 次）降到 **1 次**。同时修复 `rtsp_server_stop` 强杀任务问题。

### 2.1 引入引用计数缓冲对象

新增内部类型（`main/frame_broadcaster.h`）：
```c
typedef struct frame_buf {
    volatile int32_t refcount;  /* __atomic 内建，C/C++ ABI 一致 */
    uint8_t       *data;        /* PSRAM */
    size_t         len;
    uint32_t       seq;
} frame_buf_t;

/* frame_msg_t 改为指针持有 */
typedef struct {
    frame_buf_t *fb;
} frame_msg_t;
```

**接口签名完全不变**（subscribe/unsubscribe/receive/release/get_latest/publish），消费者代码无感知——除字段访问从 `msg.data`/`msg.len`/`msg.seq` 改为 `msg.fb->data`/`msg.fb->len`/`msg.fb->seq`。需同步更新 `mjpeg_streamer.c`、`rtsp_server.cpp` 中的字段访问。

### 2.2 publish 实现改造（`main/frame_broadcaster.c:169-228`）

- 分配 1 个 `frame_buf_t`，做唯一一次 PSRAM memcpy
- `s_cache` 改为持引用（替换 `free+malloc+memcpy` 为引用递增）
- 每个订阅者通过 `xQueueSend` 拿到同一 `frame_buf_t*`，refcount +1
- drain 旧帧时 `frame_buf_release` 旧引用
- 用 `__atomic_fetch_add/sub` 保证跨核递增/递减安全（FreeRTOS `xQueueSend/Receive` 的临界区已提供内存可见性屏障）

### 2.3 recorder 配合（`main/video_recorder.c:751-765, 832-833`）

保留 recorder 对 camera_fb 的那次 memcpy（独苗拷贝，注释 `:747-750` 已论证必要性），但把它包装为 `frame_buf_t*`：
```c
frame_buf_t *fb = frame_buf_alloc(frame.buf, frame.len);  /* 内部 1 次 PSRAM malloc+memcpy */
camera_return_fb(&frame);
fbroadcast_publish_take(fb);   /* 移交引用，内部不再 memcpy */
/* 运动检测 + SD 写入仍用 fb->data */
frame_buf_release(fb);          /* recorder 释放自己的引用 */
```

### 2.4 修复 `rtsp_server_stop` 强杀（`main/rtsp_server.cpp:291-311`）

当前 `vTaskDelete(s_video_task)` 会泄漏订阅者持有的帧引用。改为：
- 设置 `s_running = false`（已有）
- 等待任务自行退出（feed_task 的 `fbroadcast_receive` 1s 超时已保证 ≤1s 退出）
- 用 `xTaskNotifyWait` 或信号量 + `vTaskDelay` 轮询 `s_video_task == NULL`（参考 `video_recorder.c:1030-1035` 的 8s 同步等待模式）

同样修复 `audio_feed_task`（虽未零拷贝改造，但为了一致性也用同一退出模式）。

### 2.5 audio_broadcaster 镜像改造（可选）

`main/audio_broadcaster.{c,h}` 是 frame_broadcaster 的镜像设计。音频帧小（160B）且频率高（50Hz），零拷贝收益有限但碎片化风险高。**建议本阶段先不动 audio_broadcaster**，待 frame_broadcaster 验证稳定后再决定。

### 2.6 验证方法
- 编译通过（C/C++ 混编 ABI 验证）
- 烧录后**长时间压测**：双 MJPEG 客户端 + 1 RTSP 客户端 + 持续录制 30 分钟
- 60s health monitor 观察 PSRAM 占用是否稳定不增长（无引用泄漏）
- 拔插 RTSP 客户端 20 次，验证 `rtsp_server_stop` 退出干净、无内存增长
- 主动触发 SD 卡故障（拔卡）验证 cleanup 路径不泄漏 frame_buf

### 2.7 回滚
单 commit 可整体回滚。改造前后帧率对比（应持平或略升，主要收益在内存稳定性和 PSRAM 带宽释放）。

---

## 阶段 3：运动检测解耦 + 双核再平衡 🧩

**目标**：把运动检测从 P5 recorder 主循环移到独立订阅者任务，释放 Core 0。

### 3.1 扩展 frame_sub_type_t（`main/frame_broadcaster.h:32-34`）

新增 `FRAMESUB_MOTION`。修正 subscribe 内部硬编码 `"MJPEG"` 日志（`:121`）为按 type 打印。

### 3.2 新建 motion_detector 任务

在 `main/motion_detector.c` 增加 `motion_detector_start()` / `motion_detector_stop()`，内部：
```c
xTaskCreatePinnedToCore(motion_task, "motion_det", 8192, NULL, 2 /* P2 */, NULL, 1 /* Core 1 */);
```
- 任务内 `fbroadcast_subscribe(FRAMESUB_MOTION)` → 循环 receive → `motion_detector_process` → release
- 结果通过现有的 `motion_detector_get_last_result()` 接口暴露（或新增回调）
- recorder 在 `video_recorder.c:769-782` 移除同步调用，改为读取最近一次结果

### 3.3 阈值恢复

`video_recorder.c:788` `drop_threshold_us` 从 2000000 恢复为 500000（运动检测不再阻塞 recorder）。recorder 栈 10240 可降回 6144。

### 3.4 双核再平衡（可选，谨慎）

考虑把 `audio_capture`（`main/audio_driver.c:255-262`）从 Core 0 移到 Core 1，让 Core 0 专注 recorder。**此项需压测验证**，因为 I2S PDM DMA 跨核可能有开销。**若阶段 3 验证显示 Core 0 仍非瓶颈，可跳过此项。**

### 3.5 验证方法
- 运动检测响应延迟应 ≤ 1 帧（不再等 recorder 循环到达）
- recorder 主循环周期从 ~500ms+（动态延时模式下）降回 ~50-100ms
- Core 0 idle 占比提升（通过 `esp_task_wdt` 间接观察，或加临时统计）
- 录制帧率在动态延时模式下提升

### 3.6 回滚
独立模块改动，回滚 motion_detector 任务化即可恢复同步调用。

---

## 阶段 4：SD 卡栈全面优化 💾

**目标**：录制吞吐从 ~2.5MB/s 提升到 ~10MB/s（理论），并增强掉电保护。

### 4.1 ⚠️ 前置：硬件验证（必做）

切换 SDMMC 前必须用万用表/示波器确认 XIAO ESP32-S3 Sense 板上 SD 卡座的 **CMD/D0/CLK** 实际走线对应的 ESP32-S3 GPIO 编号。项目文档自相矛盾：
- `docs/en/hardware.md:74-80` 说 CMD=10（但 GPIO10 是 `CAM_PIN_XCLK`，与 `AGENTS.md:56-57` 禁令冲突）
- Seeed wiki 不同段落出现过 CMD=GPIO8 / GPIO10 等不同说法
- `docs/en/architecture.md:43` 说 SPI 模式

**如果硬件验证确认 CMD 引脚可用且不与摄像头冲突**，进入 4.2；否则跳过 4.2，仅做 4.3-4.5（应用层优化）。

### 4.2 SDMMC 1-bit 切换（`main/storage_manager.c`，10+ 处改动）

按核查报告的改动表：
- `:30-36` 头文件 `sdspi_host.h`/`spi_master.h` → `sdmmc_host.h`/`sdmmc_default_configs.h`
- `:44-48` 引脚定义（CS 删除，CLK/CMD/D0 按硬件实测重映射）
- `:99-109` 日志和 `gpio_reset_pin` 调整
- `:114-136` host 配置从 `SDSPI_HOST_DEFAULT()` 改为 `SDMMC_HOST_DEFAULT()` + `.flags = SDMMC_HOST_FLAG_1BIT`
- `:153` `esp_vfs_fat_sdspi_mount` → `esp_vfs_fat_sdmmc_mount`
- `:629-672` `sdspi_mount/unmount` 辅助函数同步改造
- `:699, 731` 调用点更新
- **同步修正**：`main/storage_manager.h:47-48` 错误注释、`docs/en/hardware.md` 和 `docs/en/architecture.md` 的矛盾描述统一为 SDMMC

ESP-IDF v6.0.1 所有 SDMMC API（`esp_vfs_fat_sdmmc_mount`、`SDMMC_HOST_DEFAULT`、`SDMMC_SLOT_CONFIG_DEFAULT`、`sdmmc_host_init/init_slot`）均已验证可用，ESP32-S3 支持 GPIO 矩阵重映射（`SOC_SDMMC_USE_GPIO_MATRIX=1`）。

### 4.3 SD 写入缓冲合并（`main/video_recorder.c:478-540`）

`write_avi_frame` 当前每帧 3 次 fwrite（8B 头 + JPEG 体 + 1B 填充）。改为：
- 把 8B 头拼接到 JPEG 数据前（需要把 JPEG 数据前置 8B 空间，或用栈上 16B header+payload 小缓冲）
- 合并为单次 fwrite（除非有 odd-length padding）
- `write_avi_audio_chunk` 同样改造

预期每帧 SPI 总线事务从 3 次降到 1 次。

### 4.4 idx1 索引改 PSRAM 分配（`main/video_recorder.c:171-192`）

`realloc` 在内部堆上长到 1.2MB（100000×12B）会失败或碎片化。改为 `heap_caps_realloc(..., MALLOC_CAP_SPIRAM)`，或初始化时一次性预分配固定上限的 PSRAM 块。

### 4.5 fflush → f_sync（`main/video_recorder.c:865-872`）

当前 `fflush` 只刷 stdio 层，掉电仍丢 FAT 元数据。改为通过 `fileno(fp)` + `fsync()`，或直接用 FatFS 原生 `f_sync`。周期保持每 10 帧（连续模式）/每帧（延时模式）。

### 4.6 验证方法
- **4.2 SDMMC**：mount 成功，能读写文件，实测持续写入速率（用 dd-like 测试或录制大文件看 MB/s）
- **4.3-4.5**：录制 1 小时不掉帧、无 SD write failed 日志、掉电测试（录制中拔电源，验证 AVI 文件可播放且 idx1 完整）
- PSRAM 占用不因 idx1 增长

### 4.7 回滚
- 4.2 失败可立即回滚到 SDSPI（git revert 单 commit）
- 4.3-4.5 各自独立，单项可回滚

---

## 阶段 5：剩余健壮性 + httpd 收尾 🛡️

**目标**：内存压力场景的健壮性、监控主动性、Web UI 响应性。

### 5.1 download 接口流式化（`main/web_server.c:822-876`）

当前整文件 4MB 一次性 malloc PSRAM。改为流式 `read` + `httpd_resp_send_chunk` 循环（4KB/8KB 步进）。消除与流媒体的 PSRAM 竞争，4MB cap 可同时放宽（或保留 cap 但移除整文件 alloc）。

### 5.2 max_open_sockets 调整 + 音频流独立（`main/web_server.c:1544`）

`max_open_sockets=4` 在 `/api/audio` 长连接 + 下载 + ws + 浏览同时存在时会顶满。两个方向：
- **简单**：把 `max_open_sockets` 提到 6-8（代价：内存）
- **彻底**：把 `/api/audio`（`web_server.c:1464-1493`）移到独立 TCP 服务器，参考 `mjpeg_streamer.c` 模式（端口 82）。**推荐此项**，与 MJPEG 架构一致。

### 5.3 health monitor 增强（`main/main.c:276-312`）

- 加 `heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)` 诊断碎片化
- PSRAM 紧张时主动动作：调用 `fbroadcast_drop_cache()`（新增接口，释放最新帧缓存，下一帧再重建）

### 5.4 可选：`storage_cleanup` 优化（`main/storage_manager.c:436-480`）

`find_oldest_recursive` 没有 `d_type` 过滤，每个 entry 都 `stat()`。参考 `list_files_recursive:255-302` 的优化模式，减少 cleanup 时 SD I/O。

### 5.5 可选：esp-dsp FFT（`main/audio_ns.c:75-108`）

手写 FFT（注释明确"no DSP lib"）。接入 `esp-dsp` 组件的 `dsps_fft2r_fc32` 可 2-3× 加速。但当前 CPU 开销已 <1%，**优先级最低**，仅在前面阶段完成后还有余力时做。

### 5.6 验证方法
- download 4MB 文件时 PSRAM 占用峰值显著下降
- 并发场景测试：1 下载 + 1 ws + 2 MJPEG 同时，所有请求成功
- health monitor 输出 largest_free_block
- 长时间运行 PSRAM 碎片化趋势观察

---

## 实施顺序与时间预估

| 阶段 | 改动面 | 风险 | 预估耗时 | 独立验证 |
|---|---|---|---|---|
| 1 | sdkconfig + 3 处 bug + SCALE_4X | 极低 | 1-2h | ✅ |
| 2 | frame_broadcaster 零拷贝 + rtsp_stop | 中（需压测） | 3-4h | ✅ |
| 3 | 运动检测任务化 | 中 | 2-3h | ✅ |
| 4 | SDMMC + 写缓冲 + idx1 + f_sync | 高（硬件依赖） | 4-6h | ✅ |
| 5 | download 流式 + httpd + health | 低 | 2-3h | ✅ |

**每阶段提交独立 commit，便于回滚和 code review。**

## 风险与不确定性

1. **SDMMC 硬件验证**（阶段 4 前置）：若硬件验证发现 CMD 引脚不可用或不稳定，阶段 4 退化为仅做应用层优化（4.3-4.5），跳过 SDMMC 切换。这是计划中唯一无法仅从代码确定的事项。
2. **PSRAM 80MHz 稳定性**（阶段 1.1）：Octal PSRAM 理论支持，但具体模组可能有 SI 问题。如不稳定立即回滚。
3. **零拷贝原子操作**（阶段 2）：用 `__atomic` 内建而非 `<stdatomic.h>`，避免 C/C++ ABI 差异。需验证 ESP32-S3 双核下 `__atomic_fetch_add` 是 lock-free（应使用 LOCK 前缀 CMPXCHG 指令）。
4. **运动检测订阅者化后的结果同步**（阶段 3）：recorder 读到的运动结果是"上一帧的"，有 1 帧延迟。对动态延时应用可接受，但需确认逻辑正确。

## 不在本次范围内的事项

- audio_broadcaster 零拷贝（收益小，留待后续）
- PSRAM DMA 启用（`CONFIG_CAMERA_PSRAM_DMA=n` 是提交 `8062e97` 为稳定性主动禁用的，不在本次回退范围）
- LWIP socket 数 / TCP 缓冲调优（影响面太大，需独立专题）
- WiFi TX power（已在 `d876c2e` 提到 20dBm，无需再动）

---

**等待你批准后，从阶段 1 开始实施。每阶段完成并验证后我会暂停汇报，确认后再进入下一阶段。**
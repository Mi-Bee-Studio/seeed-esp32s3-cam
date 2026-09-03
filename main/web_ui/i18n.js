'use strict';

(function () {
  const STORAGE_KEY = 'mibee.lang';

  const STRINGS = {
    en: {
      /* ---------- Status bar ---------- */
      'status.wifi': 'WiFi',
      'status.ip': 'IP',
      'status.resolution': 'Resolution',
      'status.quality': 'Quality',
      'status.heap': 'Heap',
      'status.psram': 'PSRAM',
      'status.clients': 'Clients',
      'status.ai': 'AI',
      'status.ai.active': 'Active',
      'status.ai.inactive': 'Inactive',

      /* ---------- Drawer ---------- */
      'drawer.controls': 'Controls',

      /* ---------- Section titles ---------- */
      'section.ai_features': 'AI Features',
      'section.flash_led': 'Flash LED',
      'section.wifi_config': 'WiFi Configuration',
      'section.camera': 'Camera',
      'section.streaming': 'Streaming',
      'section.system': 'System',

      /* ---------- AI chips ---------- */
      'ai.chip.face': 'Face',
      'ai.chip.motion': 'Motion',
      'ai.chip.qr': 'QR',
      'ai.face': 'Face Detection',
      'ai.motion': 'Motion Detection',
      'ai.qr': 'QR Code',
      'ai.overlay': 'Show Overlay',
      'ai.note': 'AI requires VGA resolution (640x480)',
      'ai.enabled': 'Enabled {feature} detection',
      'ai.disabled': 'Disabled {feature} detection',
      'ai.require_vga': 'AI requires VGA',

      /* ---------- Camera settings ---------- */
      'camera.framesize': 'Resolution',
      'camera.quality': 'JPEG Quality',
      'camera.brightness': 'Brightness',
      'camera.contrast': 'Contrast',
      'camera.saturation': 'Saturation',
      'camera.sharpness': 'Sharpness',
      'camera.hmirror': 'Horizontal Mirror',
      'camera.vflip': 'Vertical Flip',
      'camera.mirror': 'Mirror',
      'camera.flip': 'Flip',

      /* ---------- Flash LED ---------- */
      'flash.brightness': 'LED Brightness',
      'flash.off': 'Off',

      /* ---------- Network ---------- */
      'network.ssid': 'WiFi SSID',
      'network.ssid_label': 'SSID',
      'network.ssid_placeholder': 'Network name',
      'network.ssid_required': 'SSID is required',
      'network.password': 'WiFi Password',
      'network.password_label': 'Password',
      'network.password_placeholder': 'Leave blank to keep current',
      'network.new_password_placeholder': 'Enter new password',
      'network.current': 'Current:',
      'network.net_primary': 'Primary',
      'network.net_secondary': 'Backup',
      'network.ssid2_label': 'Backup SSID',
      'network.ssid2_placeholder': 'Backup network (optional)',
      'network.password2_label': 'Backup Password',
      'network.backup_hint': 'Used automatically when the primary network is unreachable',
      'network.save': 'Save & Reboot',
      'network.save_reconnect': 'Save & Reconnect',
      'network.saving': 'Saved \u2014 rebooting\u2026',
      'network.countdown': 'Rebooting in {s}s\u2026',

      /* ---------- Streaming ---------- */
      'streaming.rtsp_user': 'RTSP Username',
      'streaming.rtsp_pass': 'RTSP Password',
      'streaming.onvif': 'ONVIF Discovery',
      'streaming.hint': 'Open these URLs in VLC / NVR',
      'streaming.mjpeg': 'MJPEG (browser)',
      'streaming.rtsp': 'RTSP (VLC / NVR)',
      'streaming.onvif_hint': 'ONVIF discovery',
      'streaming.onvif_note': 'Search via WS-Discovery in Synology / Milestone / etc.',

      /* ---------- Stream status ---------- */
      'stream.connecting': 'Connecting\u2026',
      'stream.error': 'Stream error, reconnecting\u2026',
      'stream.reconnecting': 'Stream disconnected \u2014 reconnecting\u2026',

      /* ---------- System ---------- */
      'system.uptime': 'Uptime',
      'system.free_heap': 'Free Heap',
      'system.free_psram': 'Free PSRAM',
      'theme.toggle': 'Dark / Light',
      'lang.toggle': 'EN / \u4e2d\u6587',

      /* ---------- Actions ---------- */
      'btn.save': 'Save',
      'btn.apply': 'Apply',
      'btn.cancel': 'Cancel',

      /* ---------- Toast messages ---------- */
      'toast.saved': 'Settings saved',
      'toast.restarting': 'Camera restarting\u2026',
      'toast.error': 'Error: {msg}',
      'toast.wifi.saved': 'WiFi saved \u2014 device will reboot',
      'toast.reboot': 'Rebooting\u2026',
      'toast.config_failed': 'Failed to load config: {msg}',
      'toast.save_failed': 'Config save failed: {msg}',
      'toast.led_failed': 'LED control failed: {msg}',
      'toast.ai_failed': 'AI toggle failed: {msg}',
      'toast.camera_restarting': 'Camera restarting\u2026',
      'toast.rebooting': 'Device rebooting\u2026',
      'toast.reconnect': 'Reconnecting\u2026',
      'toast.disabled': 'Disabled',
      'toast.enabled': 'Enabled',
      'toast.no_change': 'No changes',

      /* ---------- AI overlay ---------- */
      'overlay.faces': 'Faces: {n}',
      'overlay.motion': 'Motion: {score}',
      'overlay.qr': 'QR: {n}',
      'overlay.initialized': 'AI overlay initialized',
      'overlay.ready': 'AI overlay ready',
      'overlay.no_detections': 'No detections',

      /* ---------- v2 unified UI ---------- */
      'status.recording.idle': 'Idle',
      'status.recording.recording': 'REC',
      'status.recording.paused': 'Paused',
      'status.sd': 'SD',
      'section.storage': 'Storage',
      'system.ota': 'Firmware Update',
      'camera.day_night': 'Day / Night',
      'camera.day_night.color': 'Color',
      'camera.day_night.bw': 'B&W',
      'camera.day_night.auto': 'Auto',
      'storage.record': 'Record',
      'storage.stop': 'Stop',
      'storage.refresh': 'Refresh',
      'storage.download': 'Download',
      'storage.delete': 'Delete',
      'storage.empty': 'No files',
      'storage.sd_free': 'SD free: {pct}%',
      'storage.no_sd': 'No SD card',
      'storage.sd_ok': 'SD card ready',
      'storage.confirm_delete': 'Delete {name}?',
      'files.type_all': 'All',
      'files.type_photos': 'Photos',
      'files.type_recordings': 'Clips',
      'files.select_all': 'Select all',
      'files.delete_selected': 'Delete selected',
      'files.clear_type': 'Clear',
      'files.format': 'Format SD',
      'files.more': 'Load more',
      'files.confirm_batch': 'Delete {n} selected file(s)?',
      'files.confirm_clear': 'Permanently delete ALL "{type}" files on the SD card?',
      'files.format_warn': 'Formatting erases EVERYTHING on the SD card (recordings, photos, logs). This cannot be undone.',
      'files.format_warn2': 'Last warning: all files will be permanently erased. Continue?',
      'files.format_sure': 'Erase everything',
      'stream.snapshot': 'Snapshot',
      'stream.audio': 'Listen',
      'stream.audio_stop': 'Stop Audio',
      'network.scan': 'Scan',
      'network.scanning': 'Scanning…',
      'network.device_name': 'Device Name',
      'network.timezone': 'Timezone (POSIX)',
      'network.admin_password': 'Admin Password',
      'network.admin_password_hint': 'Set once — leave blank to keep',
      'network.apply': 'Save',
      'system.fw_version': 'Firmware',
      'system.chip_temp': 'Chip Temp',
      'system.min_heap': 'Min Heap',
      'system.reboot': 'Reboot',
      'system.factory_reset': 'Factory Reset',
      'system.confirm_reboot': 'Reboot the device?',
      'system.confirm_reset': 'Factory reset ALL settings? Device will reboot.',
      'system.ota_fw': 'Firmware (.bin)',
      'system.ota_spiffs': 'Web UI (.bin)',
      'system.ota_upload': 'Upload',
      'toast.upload_done': 'Upload OK — rebooting',
      'toast.upload_failed': 'Upload failed: {msg}',
      'toast.record_started': 'Recording started',
      'toast.record_stopped': 'Recording stopped',
      'toast.record_failed': 'Record failed: {msg}',
      'toast.deleted': 'Deleted',
      'toast.delete_failed': 'Delete failed: {msg}',
      'toast.batch_done': 'Deleted {ok} file(s)',
      'toast.batch_partial': 'Deleted {ok}, failed {fail}',
      'toast.format_done': 'SD card formatted',
      'toast.format_failed': 'Format failed: {msg}',
      'toast.audio_failed': 'Audio failed: {msg}',
      'toast.set_password_first': 'Set the admin password first (Network tab)',
      'toast.need_password': 'Auth failed — set admin password in System tab',
      'wifi.state.ap': 'AP',
      'wifi.state.connecting': 'Connecting…',
      'wifi.state.connected': 'Connected',
      'wifi.state.disconnected': 'Off',
      'ws.motion_started': 'Motion detected (score {score})',
      'ws.motion_cleared': 'Motion cleared',
      'ws.recording_started': 'Recording started',
      'ws.recording_stopped': 'Recording stopped',
      'ws.wifi_state_changed': 'WiFi: {state}',

      /* ---------- v3 "Honey" UI ---------- */
      'auth.title': 'Admin password',
      'auth.set_title': 'Set Admin Password',
      'auth.set_msg': 'First time setup — this password protects all device settings.',
      'auth.unlock_title': 'Unlock Device',
      'auth.unlock_msg': 'Enter the admin password to make changes.',
      'auth.placeholder': 'Password',
      'auth.set_ok': 'Set Password',
      'auth.unlock_ok': 'Unlock',
      'auth.empty': 'Please enter a password',
      'auth.wrong': 'Wrong password',
      'auth.set_fail': 'Failed to set password',
      'auth.ok': 'Unlocked',
      'btn.confirm': 'Confirm',
      'stream.fullscreen': 'Full',
      'stream.reconnecting': 'Stream lost — reconnecting…',
      'network.wifi_hint': 'WiFi changes apply after reboot',
      'network.scan_empty': 'No networks found',
      'storage.used': 'Used {pct}%',
      'system.ota_hint': 'Device reboots after upload',
      'flash.hint': '0 = off, 100 = full brightness',
      'pw.change': 'Change Password',
      'pw.change_msg': 'Enter the current password, then set a new one (min 6 characters).',
      'pw.old': 'Current password',
      'pw.new': 'New password',
      'pw.confirm': 'Confirm new password',
      'pw.ok': 'Change',
      'pw.too_short': 'New password must be at least 6 characters',
      'pw.mismatch': 'Passwords do not match',
      'pw.wrong_old': 'Current password is incorrect',
      'pw.changed': 'Password changed',
      'pw.failed': 'Change failed: {msg}',

      /* ---------- 指标条（人话标签） ---------- */
      'stats.rssi': '信号',
      'stats.rssi.hint': 'WiFi 信号强度（越接近 0 越好）',
      'stats.link': '连接',
      'stats.heap': '内存',
      'stats.heap.hint': '可用运行内存',
      'stats.psram': '扩展内存',
      'stats.psram.hint': '可用扩展内存（图像缓冲）',
      'stats.temp': '温度',
      'stats.temp.hint': '芯片温度（超过 85°C 会变红）',
      'stats.sd': 'SD 剩余',
      'stats.sd.hint': '存储卡剩余空间',
      'stats.uptime': '已运行',
      'stats.uptime.hint': '开机时长',
    },

    zh: {
      /* ---------- 状态栏 ---------- */
      'status.wifi': 'WiFi',
      'status.ip': 'IP',
      'status.resolution': '\u5206\u8fa8\u7387',
      'status.quality': '\u8d28\u91cf',
      'status.heap': '\u5185\u5b58',
      'status.psram': 'PSRAM',
      'status.clients': '\u8fde\u63a5\u6570',
      'status.ai': 'AI',
      'status.ai.active': '\u8fd0\u884c\u4e2d',
      'status.ai.inactive': '\u672a\u542f\u7528',

      /* ---------- 抽屉 ---------- */
      'drawer.controls': '\u63a7\u5236',

      /* ---------- 区块标题 ---------- */
      'section.ai_features': 'AI \u529f\u80fd',
      'section.flash_led': '\u95ea\u5149\u706f',
      'section.wifi_config': 'WiFi \u914d\u7f6e',
      'section.camera': '\u6444\u50cf\u5934',
      'section.streaming': '\u63a8\u6d41',
      'section.system': '\u7cfb\u7edf',

      /* ---------- AI 开关 ---------- */
      'ai.chip.face': '\u4eba\u8138',
      'ai.chip.motion': '\u8fd0\u52a8',
      'ai.chip.qr': '\u4e8c\u7ef4\u7801',
      'ai.face': '\u4eba\u8138\u68c0\u6d4b',
      'ai.motion': '\u8fd0\u52a8\u68c0\u6d4b',
      'ai.qr': '\u4e8c\u7ef4\u7801',
      'ai.overlay': '\u663e\u793a\u53e0\u52a0\u5c42',
      'ai.note': 'AI \u9700\u8981 VGA \u5206\u8fa8\u7387 (640x480)',
      'ai.enabled': '\u5df2\u542f\u7528 {feature} \u68c0\u6d4b',
      'ai.disabled': '\u5df2\u7981\u7528 {feature} \u68c0\u6d4b',
      'ai.require_vga': 'AI \u9700\u8981 VGA',

      /* ---------- 摄像头设置 ---------- */
      'camera.framesize': '\u5206\u8fa8\u7387',
      'camera.quality': 'JPEG \u8d28\u91cf',
      'camera.brightness': '\u4eae\u5ea6',
      'camera.contrast': '\u5bf9\u6bd4\u5ea6',
      'camera.saturation': '\u9971\u548c\u5ea6',
      'camera.sharpness': '\u9510\u5ea6',
      'camera.hmirror': '\u6c34\u5e73\u955c\u50cf',
      'camera.vflip': '\u5782\u76f4\u7ffb\u8f6c',
      'camera.mirror': '\u955c\u50cf',
      'camera.flip': '\u7ffb\u8f6c',

      /* ---------- 闪光灯 ---------- */
      'flash.brightness': 'LED \u4eae\u5ea6',
      'flash.off': '\u5173\u95ed',

      /* ---------- 网络 ---------- */
      'network.ssid': 'WiFi \u540d\u79f0',
      'network.ssid_label': 'SSID',
      'network.ssid_placeholder': '\u7f51\u7edc\u540d\u79f0',
      'network.ssid_required': '\u8bf7\u8f93\u5165 SSID',
      'network.password': 'WiFi \u5bc6\u7801',
      'network.password_label': '\u5bc6\u7801',
      'network.password_placeholder': '\u4fdd\u7559\u5f53\u524d\u5bc6\u7801\u8bf7\u7559\u7a7a',
      'network.new_password_placeholder': '\u8bf7\u8f93\u5165\u65b0\u5bc6\u7801',
      'network.current': '\u5f53\u524d\u8fde\u63a5\uff1a',
      'network.net_primary': '\u4e3b\u7f51\u7edc',
      'network.net_secondary': '\u5907\u7528\u7f51\u7edc',
      'network.ssid2_label': '\u5907\u7528 WiFi SSID',
      'network.ssid2_placeholder': '\u5907\u7528\u7f51\u7edc\uff08\u53ef\u9009\uff09',
      'network.password2_label': '\u5907\u7528\u7f51\u7edc\u5bc6\u7801',
      'network.backup_hint': '\u4e3b\u7f51\u7edc\u4e0d\u53ef\u7528\u65f6\u81ea\u52a8\u5207\u6362\u5230\u5907\u7528\u7f51\u7edc',
      'network.save': '\u4fdd\u5b58\u5e76\u91cd\u542f',
      'network.save_reconnect': '\u4fdd\u5b58\u5e76\u91cd\u8fde',
      'network.saving': '\u5df2\u4fdd\u5b58 \u2014 \u6b63\u5728\u91cd\u542f\u2026',
      'network.countdown': '{s}\u79d2\u540e\u91cd\u542f\u2026',

      /* ---------- 串流 ---------- */
      'streaming.rtsp_user': 'RTSP 用户名',
      'streaming.rtsp_pass': 'RTSP 密码',
      'streaming.onvif': 'ONVIF 发现',
      'streaming.hint': '在 VLC / NVR 中打开以下地址',
      'streaming.mjpeg': 'MJPEG（浏览器）',
      'streaming.rtsp': 'RTSP（VLC / NVR）',
      'streaming.onvif_hint': 'ONVIF 自动发现',
      'streaming.onvif_note': '在群晖 / Milestone 等 NVR 中用 WS-Discovery 搜索',

      /* ---------- 串流状态 ---------- */
      'stream.connecting': '\u8fde\u63a5\u4e2d\u2026',
      'stream.error': '\u6d41\u9519\u8bef\uff0c\u91cd\u65b0\u8fde\u63a5\u4e2d\u2026',
      'stream.reconnecting': '\u89c6\u9891\u6d41\u65ad\u5f00 \u2014 \u91cd\u65b0\u8fde\u63a5\u4e2d\u2026',

      /* ---------- 系统 ---------- */
      'system.uptime': '\u8fd0\u884c\u65f6\u95f4',
      'system.free_heap': '\u53ef\u7528\u5185\u5b58',
      'system.free_psram': '\u53ef\u7528 PSRAM',
      'theme.toggle': '\u6697 / \u4eae',
      'lang.toggle': '\u4e2d\u6587 / EN',

      /* ---------- 操作 ---------- */
      'btn.save': '\u4fdd\u5b58',
      'btn.apply': '\u5e94\u7528',
      'btn.cancel': '\u53d6\u6d88',

      /* ---------- 提示消息 ---------- */
      'toast.saved': '\u8bbe\u7f6e\u5df2\u4fdd\u5b58',
      'toast.restarting': '\u6444\u50cf\u5934\u91cd\u542f\u4e2d\u2026',
      'toast.error': '\u9519\u8bef: {msg}',
      'toast.wifi.saved': 'WiFi \u5df2\u4fdd\u5b58 \u2014 \u8bbe\u5907\u5c06\u91cd\u542f',
      'toast.reboot': '\u91cd\u542f\u4e2d\u2026',
      'toast.config_failed': '\u52a0\u8f7d\u914d\u7f6e\u5931\u8d25: {msg}',
      'toast.save_failed': '\u4fdd\u5b58\u914d\u7f6e\u5931\u8d25: {msg}',
      'toast.led_failed': 'LED \u63a7\u5236\u5931\u8d25: {msg}',
      'toast.ai_failed': 'AI \u5f00\u5173\u5931\u8d25: {msg}',
      'toast.camera_restarting': '\u6444\u50cf\u5934\u91cd\u542f\u4e2d\u2026',
      'toast.rebooting': '\u8bbe\u5907\u91cd\u542f\u4e2d\u2026',
      'toast.reconnect': '\u91cd\u65b0\u8fde\u63a5\u4e2d\u2026',
      'toast.disabled': '\u5df2\u7981\u7528',
      'toast.enabled': '\u5df2\u542f\u7528',
      'toast.no_change': '\u65e0\u66f4\u6539',

      /* ---------- AI 叠加层 ---------- */
      'overlay.faces': '\u4eba\u8138: {n}',
      'overlay.motion': '\u8fd0\u52a8: {score}',
      'overlay.qr': '\u4e8c\u7ef4\u7801: {n}',
      'overlay.initialized': 'AI \u53e0\u52a0\u5c42\u5df2\u521d\u59cb\u5316',
      'overlay.ready': 'AI \u53e0\u52a0\u5c42\u5df2\u5c31\u7eea',
      'overlay.no_detections': '\u672a\u68c0\u6d4b\u5230\u76ee\u6807',

      /* ---------- v2 统一界面 ---------- */
      'status.recording.idle': '空闲',
      'status.recording.recording': '录像中',
      'status.recording.paused': '已暂停',
      'status.sd': 'SD',
      'section.storage': '存储',
      'system.ota': '固件升级',
      'camera.day_night': '日夜模式',
      'camera.day_night.color': '彩色',
      'camera.day_night.bw': '黑白',
      'camera.day_night.auto': '自动',
      'storage.record': '开始录像',
      'storage.stop': '停止录像',
      'storage.refresh': '刷新',
      'storage.download': '下载',
      'storage.delete': '删除',
      'storage.empty': '暂无文件',
      'storage.sd_free': 'SD 剩余: {pct}%',
      'storage.no_sd': '未检测到 SD 卡',
      'storage.sd_ok': 'SD 卡正常',
      'storage.confirm_delete': '删除 {name}？',
      'files.type_all': '全部',
      'files.type_photos': '照片',
      'files.type_recordings': '录像',
      'files.select_all': '全选',
      'files.delete_selected': '删除所选',
      'files.clear_type': '清空',
      'files.format': '格式化',
      'files.more': '加载更多',
      'files.confirm_batch': '删除已选的 {n} 个文件？',
      'files.confirm_clear': '将永久删除 SD 卡上全部“{type}”文件，确定继续？',
      'files.format_warn': '格式化将清空 SD 卡上的所有内容（录像、照片、日志），且无法恢复。',
      'files.format_warn2': '最后确认：所有文件将被永久擦除，是否继续？',
      'files.format_sure': '全部擦除',
      'stream.snapshot': '拍照',
      'stream.audio': '监听',
      'stream.audio_stop': '停止监听',
      'network.scan': '扫描',
      'network.scanning': '扫描中…',
      'network.device_name': '设备名称',
      'network.timezone': '时区 (POSIX)',
      'network.admin_password': '管理密码',
      'network.admin_password_hint': '设置一次 — 留空保持不变',
      'network.apply': '保存',
      'system.fw_version': '固件版本',
      'system.chip_temp': '芯片温度',
      'system.min_heap': '最低剩余内存',
      'system.reboot': '重启设备',
      'system.factory_reset': '恢复出厂',
      'system.confirm_reboot': '确定重启设备？',
      'system.confirm_reset': '恢复出厂将清空全部设置并重启，确定？',
      'system.ota_fw': '固件 (.bin)',
      'system.ota_spiffs': 'Web UI (.bin)',
      'system.ota_upload': '上传',
      'toast.upload_done': '上传成功 — 设备重启中',
      'toast.upload_failed': '上传失败: {msg}',
      'toast.record_started': '录像已开始',
      'toast.record_stopped': '录像已停止',
      'toast.record_failed': '录像操作失败: {msg}',
      'toast.deleted': '已删除',
      'toast.delete_failed': '删除失败: {msg}',
      'toast.batch_done': '已删除 {ok} 个文件',
      'toast.batch_partial': '已删除 {ok} 个，{fail} 个失败',
      'toast.format_done': 'SD 卡格式化完成',
      'toast.format_failed': '格式化失败: {msg}',
      'toast.audio_failed': '音频失败: {msg}',
      'toast.set_password_first': '请先设置管理密码（网络页）',
      'toast.need_password': '鉴权失败 — 请在系统页设置管理密码',
      'wifi.state.ap': 'AP',
      'wifi.state.connecting': '连接中…',
      'wifi.state.connected': '已连接',
      'wifi.state.disconnected': '离线',
      'ws.motion_started': '检测到移动 (分数 {score})',
      'ws.motion_cleared': '移动消失',
      'ws.recording_started': '录像开始',
      'ws.recording_stopped': '录像停止',
      'ws.wifi_state_changed': 'WiFi: {state}',

      /* ---------- v3 "Honey" 界面 ---------- */
      'auth.title': '管理密码',
      'auth.set_title': '设置管理密码',
      'auth.set_msg': '首次使用 — 此密码用于保护设备全部设置。',
      'auth.unlock_title': '解锁设备',
      'auth.unlock_msg': '输入管理密码后才能修改设置。',
      'auth.placeholder': '密码',
      'auth.set_ok': '设置密码',
      'auth.unlock_ok': '解锁',
      'auth.empty': '请输入密码',
      'auth.wrong': '密码错误',
      'auth.set_fail': '设置密码失败',
      'auth.ok': '已解锁',
      'btn.confirm': '确认',
      'stream.fullscreen': '全屏',
      'stream.reconnecting': '信号中断 — 重连中…',
      'network.wifi_hint': 'WiFi 修改重启后生效',
      'network.scan_empty': '未找到网络',
      'storage.used': '已用 {pct}%',
      'system.ota_hint': '上传完成后设备自动重启',
      'flash.hint': '0 = 关闭，100 = 最亮',
      'pw.change': '修改密码',
      'pw.change_msg': '输入当前密码验证后设置新密码（至少 6 位）。',
      'pw.old': '当前密码',
      'pw.new': '新密码',
      'pw.confirm': '确认新密码',
      'pw.ok': '修改',
      'pw.too_short': '新密码至少需要 6 位',
      'pw.mismatch': '两次输入的密码不一致',
      'pw.wrong_old': '当前密码不正确',
      'pw.changed': '密码已修改',
      'pw.failed': '修改失败: {msg}',

      /* ---------- 指标条（人话标签） ---------- */
      'stats.rssi': '信号',
      'stats.rssi.hint': 'WiFi 信号强度（越接近 0 越好）',
      'stats.link': '连接',
      'stats.heap': '内存',
      'stats.heap.hint': '可用运行内存',
      'stats.psram': '扩展内存',
      'stats.psram.hint': '可用扩展内存（图像缓冲）',
      'stats.temp': '温度',
      'stats.temp.hint': '芯片温度（超过 85°C 会变红）',
      'stats.sd': 'SD 剩余',
      'stats.sd.hint': '存储卡剩余空间',
      'stats.uptime': '已运行',
      'stats.uptime.hint': '开机时长',
    },
  };

  var currentLang = 'en';

  function detect() {
    var stored;
    try {
      stored = localStorage.getItem(STORAGE_KEY);
    } catch (_) {
      /* localStorage may be disabled */
    }
    if (stored === 'en' || stored === 'zh') {
      return stored;
    }
    var nav = navigator.language || navigator.userLanguage || '';
    return nav.startsWith('zh') ? 'zh' : 'en';
  }

  function getLang() {
    return currentLang;
  }

  function setLang(lang) {
    if (lang !== 'en' && lang !== 'zh') return;
    currentLang = lang;
    try {
      localStorage.setItem(STORAGE_KEY, lang);
    } catch (_) {
      /* persist best-effort */
    }
    document.documentElement.lang = lang;
    apply();
  }

  function t(key, params) {
    var str =
      (STRINGS[currentLang] && STRINGS[currentLang][key]) || key;
    if (params && typeof params === 'object') {
      for (var k in params) {
        if (Object.prototype.hasOwnProperty.call(params, k)) {
          str = str.split('{' + k + '}').join(String(params[k]));
        }
      }
    }
    return str;
  }

  function apply() {
    /* Replace [data-i18n] text content */
    var els = document.querySelectorAll('[data-i18n]');
    for (var i = 0; i < els.length; i++) {
      var el = els[i];
      var key = el.getAttribute('data-i18n');
      el.textContent = t(key);
    }
    /* Replace [data-i18n-placeholder] placeholders */
    var inputs = document.querySelectorAll('[data-i18n-placeholder]');
    for (var j = 0; j < inputs.length; j++) {
      var inp = inputs[j];
      var pk = inp.getAttribute('data-i18n-placeholder');
      inp.placeholder = t(pk);
    }
    /* Replace [data-i18n-title] title attributes */
    var titled = document.querySelectorAll('[data-i18n-title]');
    for (var k = 0; k < titled.length; k++) {
      var tel = titled[k];
      tel.setAttribute('title', t(tel.getAttribute('data-i18n-title')));
    }
  }

  /* Auto-init on DOMContentLoaded */
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', function () {
      currentLang = detect();
      document.documentElement.lang = currentLang;
      apply();
    });
  } else {
    currentLang = detect();
    document.documentElement.lang = currentLang;
    apply();
  }

  /* Expose global */
  window.i18n = {
    detect: detect,
    getLang: getLang,
    setLang: setLang,
    t: t,
    apply: apply,
    STRINGS: STRINGS,
  };
})();

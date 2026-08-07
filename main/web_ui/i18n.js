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
      'network.save': 'Save & Reboot',
      'network.save_reconnect': 'Save & Reconnect',
      'network.saving': 'Saved \u2014 rebooting\u2026',
      'network.countdown': 'Rebooting in {s}s\u2026',

      /* ---------- Streaming ---------- */
      'streaming.rtsp_user': 'RTSP Username',
      'streaming.rtsp_pass': 'RTSP Password',
      'streaming.onvif': 'ONVIF Discovery',

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
      'network.save': '\u4fdd\u5b58\u5e76\u91cd\u542f',
      'network.save_reconnect': '\u4fdd\u5b58\u5e76\u91cd\u8fde',
      'network.saving': '\u5df2\u4fdd\u5b58 \u2014 \u6b63\u5728\u91cd\u542f\u2026',
      'network.countdown': '{s}\u79d2\u540e\u91cd\u542f\u2026',

      /* ---------- 串流 ---------- */
      'streaming.rtsp_user': 'RTSP \u7528\u6237\u540d',
      'streaming.rtsp_pass': 'RTSP \u5bc6\u7801',
      'streaming.onvif': 'ONVIF \u53d1\u73b0',

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

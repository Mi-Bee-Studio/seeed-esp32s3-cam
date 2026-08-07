'use strict';

/* ==================================================================
 * MiBee Cam — app.js
 * Theme, i18n, API, toast, polling, camera/AI/LED/network controls.
 * Depends on i18n.js loaded first (window.i18n).
 * ================================================================ */

/* ---------- 1. Theme Management ---------- */

const Theme = {
    STORAGE_KEY: 'mibee.theme',

    init() {
        const stored = localStorage.getItem(this.STORAGE_KEY);
        if (stored === 'dark') {
            document.documentElement.setAttribute('data-theme', 'dark');
            document.getElementById('toggle-theme').classList.add('active');
            document.getElementById('toggle-theme').setAttribute('aria-checked', 'true');
        } else if (stored === 'light') {
            document.documentElement.removeAttribute('data-theme');
        } else {
            /* Follow system preference */
            if (window.matchMedia('(prefers-color-scheme: dark)').matches) {
                document.documentElement.setAttribute('data-theme', 'dark');
                document.getElementById('toggle-theme').classList.add('active');
                document.getElementById('toggle-theme').setAttribute('aria-checked', 'true');
            }
        }
        /* Listen for system changes only when no stored preference */
        window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
            if (!localStorage.getItem(this.STORAGE_KEY)) {
                if (e.matches) {
                    document.documentElement.setAttribute('data-theme', 'dark');
                } else {
                    document.documentElement.removeAttribute('data-theme');
                }
            }
        });
    },

    toggle() {
        const isDark = document.documentElement.getAttribute('data-theme') === 'dark';
        if (isDark) {
            document.documentElement.removeAttribute('data-theme');
            localStorage.setItem(this.STORAGE_KEY, 'light');
        } else {
            document.documentElement.setAttribute('data-theme', 'dark');
            localStorage.setItem(this.STORAGE_KEY, 'dark');
        }
        const el = document.getElementById('toggle-theme');
        el.classList.toggle('active');
        el.setAttribute('aria-checked', String(!isDark));
    }
};

/* ---------- 2. API Helper ---------- */

async function api(path, options = {}) {
    const resp = await fetch(path, options);
    const json = await resp.json();
    if (!json.ok) {
        throw new Error(json.error || `HTTP ${resp.status}`);
    }
    return json.data || {};
}

/* ---------- 3. Toast ---------- */

function showToast(msg, duration) {
    if (!duration) duration = 3000;
    const el = document.getElementById('toast');
    el.textContent = msg;
    el.classList.add('show');
    clearTimeout(el._timeout);
    el._timeout = setTimeout(() => el.classList.remove('show'), duration);
}

/* ---------- 4. Toggle click handler ---------- */

function initToggle(id, onChange) {
    const el = document.getElementById(id);
    if (!el) return;
    /* Initialize from aria-checked attribute */
    if (el.getAttribute('aria-checked') === 'true') {
        el.classList.add('active');
    }
    el.addEventListener('click', () => {
        const active = el.classList.toggle('active');
        el.setAttribute('aria-checked', String(active));
        if (onChange) onChange(active);
    });
    /* Also support keyboard activation */
    el.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' || e.key === ' ') {
            e.preventDefault();
            el.click();
        }
    });
}

/* ---------- 5. Tab Navigation ---------- */

function initTabs() {
    const tabs = document.querySelectorAll('.settings-tab');
    tabs.forEach(tab => {
        tab.addEventListener('click', () => {
            document.querySelectorAll('.settings-tab').forEach(t => t.classList.remove('active'));
            document.querySelectorAll('.settings-section').forEach(s => s.classList.remove('active'));
            tab.classList.add('active');
            const section = document.getElementById('section-' + tab.dataset.tab);
            if (section) section.classList.add('active');
        });
    });
}

/* ---------- 6. /status polling (500ms) ---------- */

async function pollStatus() {
    try {
        const data = await api('/api/status');
        document.getElementById('badge-ip').textContent = data.ip || '--';
        document.getElementById('badge-resolution').textContent = data.camera_resolution || '--';
        document.getElementById('badge-quality').textContent = String(data.camera_quality ?? '--');

        const heap = data.free_heap;
        document.getElementById('badge-heap').textContent = heap ? `${(heap / 1024).toFixed(0)}KB` : '--';
        const psram = data.free_psram;
        document.getElementById('badge-psram').textContent = psram ? `${(psram / 1024).toFixed(0)}KB` : '--';
        const clients = data.mjpeg_clients;
        document.getElementById('badge-clients').textContent = clients ?? '0';

        const aiStatus = data.ai_status;
        const aiBadge = document.getElementById('badge-ai');
        const anyActive = aiStatus && (aiStatus.face || aiStatus.motion || aiStatus.qr);
        aiBadge.textContent = anyActive
            ? window.i18n.t('status.ai.active')
            : window.i18n.t('status.ai.inactive');
        aiBadge.style.background = anyActive ? 'var(--success)' : 'var(--surface-3)';

        /* Update system section readouts */
        document.getElementById('sys-uptime').textContent = data.uptime || '--';
        document.getElementById('sys-heap').textContent = heap ? `${(heap / 1024).toFixed(1)}KB` : '--';
        document.getElementById('sys-psram').textContent = psram ? `${(psram / 1024).toFixed(1)}KB` : '--';
    } catch (e) {
        /* Device offline — badges keep previous values */
    }
}

/* ---------- 7. AI overlay from /ai/status (500ms) ---------- */

async function pollAI() {
    try {
        const data = await api('/api/ai/status');
        const canvas = document.getElementById('stream-overlay');
        const ctx = canvas.getContext('2d');
        const img = document.getElementById('stream-img');

        canvas.width = img.clientWidth || 640;
        canvas.height = img.clientHeight || 480;
        ctx.clearRect(0, 0, canvas.width, canvas.height);

        /* Face boxes */
        const face = data.face;
        if (face && face.boxes && face.boxes.length > 0) {
            const scaleX = canvas.width / 640;
            const scaleY = canvas.height / 480;
            ctx.strokeStyle = '#00ff00';
            ctx.lineWidth = 2;
            face.boxes.forEach(box => {
                ctx.strokeRect(
                    box.x * scaleX, box.y * scaleY,
                    box.w * scaleX, box.h * scaleY
                );
            });
        }

        /* Motion score (top-left indicator) */
        const motion = data.motion;
        if (motion && motion.score > 0) {
            ctx.fillStyle = 'rgba(255, 165, 0, 0.8)';
            ctx.font = '12px -apple-system, sans-serif';
            ctx.fillText('M:' + motion.score.toFixed(0), 8, 16);
        }

        /* QR codes (top-right) */
        const qr = data.qr;
        if (qr && qr.count > 0) {
            ctx.fillStyle = 'rgba(0, 122, 255, 0.8)';
            ctx.font = '12px -apple-system, sans-serif';
            ctx.fillText('QR:' + qr.count, canvas.width - 60, 16);
        }
    } catch (e) {
        /* 404 = AI not running — silently skip */
    }
}

/* ---------- 8. Camera panel: GET + POST (debounced) ---------- */

let cameraSaveTimeout = null;

async function loadCamera() {
    try {
        const data = await api('/api/camera');
        document.getElementById('cam-framesize').value = data.cam_framesize ?? 10;
        document.getElementById('cam-quality').value = data.cam_quality ?? 12;
        document.getElementById('cam-quality-val').textContent = data.cam_quality ?? 12;
        document.getElementById('cam-brightness').value = data.cam_brightness ?? 0;
        document.getElementById('cam-brightness-val').textContent = data.cam_brightness ?? 0;
        document.getElementById('cam-contrast').value = data.cam_contrast ?? 0;
        document.getElementById('cam-contrast-val').textContent = data.cam_contrast ?? 0;
        document.getElementById('cam-saturation').value = data.cam_saturation ?? 0;
        document.getElementById('cam-saturation-val').textContent = data.cam_saturation ?? 0;
        document.getElementById('cam-sharpness').value = data.cam_sharpness ?? 0;
        document.getElementById('cam-sharpness-val').textContent = data.cam_sharpness ?? 0;

        const hmToggle = document.getElementById('cam-hmirror');
        hmToggle.classList.toggle('active', !!data.cam_hmirror);
        hmToggle.setAttribute('aria-checked', String(!!data.cam_hmirror));

        const vfToggle = document.getElementById('cam-vflip');
        vfToggle.classList.toggle('active', !!data.cam_vflip);
        vfToggle.setAttribute('aria-checked', String(!!data.cam_vflip));

        updateAIVGACoupling();
    } catch (e) {
        console.error('loadCamera:', e);
    }
}

function saveCamera() {
    clearTimeout(cameraSaveTimeout);
    cameraSaveTimeout = setTimeout(async () => {
        try {
            const payload = {
                cam_framesize: parseInt(document.getElementById('cam-framesize').value),
                cam_quality: parseInt(document.getElementById('cam-quality').value),
                cam_brightness: parseInt(document.getElementById('cam-brightness').value),
                cam_contrast: parseInt(document.getElementById('cam-contrast').value),
                cam_saturation: parseInt(document.getElementById('cam-saturation').value),
                cam_sharpness: parseInt(document.getElementById('cam-sharpness').value),
                cam_hmirror: document.getElementById('cam-hmirror').classList.contains('active'),
                cam_vflip: document.getElementById('cam-vflip').classList.contains('active'),
            };
            await api('/api/camera', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload)
            });
            showToast(window.i18n.t('toast.saved'));
            document.getElementById('cam-quality-val').textContent = payload.cam_quality;
            document.getElementById('cam-brightness-val').textContent = payload.cam_brightness;
            document.getElementById('cam-contrast-val').textContent = payload.cam_contrast;
            document.getElementById('cam-saturation-val').textContent = payload.cam_saturation;
            document.getElementById('cam-sharpness-val').textContent = payload.cam_sharpness;
            await loadCamera();
        } catch (e) {
            showToast(window.i18n.t('toast.error', { msg: e.message }));
        }
    }, 300);
}

/* ---------- 9. AI ↔ VGA Coupling ---------- */

function updateAIVGACoupling() {
    const framesize = parseInt(document.getElementById('cam-framesize').value);
    const isVGA = framesize === 10;
    const anyAI = document.getElementById('ai-face').classList.contains('active')
               || document.getElementById('ai-motion').classList.contains('active')
               || document.getElementById('ai-qr').classList.contains('active');

    /* Disable AI toggles when non-VGA selected */
    const aiToggles = ['ai-face', 'ai-motion', 'ai-qr'];
    aiToggles.forEach(id => {
        const el = document.getElementById(id);
        if (!el) return;
        el.disabled = !isVGA;
        el.style.opacity = isVGA ? '1' : '0.4';
        el.style.pointerEvents = isVGA ? 'auto' : 'none';
    });

    /* If any AI is on, restrict framesize to VGA only */
    const select = document.getElementById('cam-framesize');
    if (anyAI) {
        Array.from(select.options).forEach(opt => {
            opt.disabled = opt.value !== '10';
        });
    } else {
        Array.from(select.options).forEach(opt => {
            opt.disabled = false;
        });
    }
}

/* ---------- 10. AI toggle POST /ai ---------- */

async function saveAI() {
    try {
        const payload = {
            face: document.getElementById('ai-face').classList.contains('active'),
            motion: document.getElementById('ai-motion').classList.contains('active'),
            qr: document.getElementById('ai-qr').classList.contains('active'),
        };
        await api('/api/ai', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        updateAIVGACoupling();
    } catch (e) {
        showToast(window.i18n.t('toast.ai_failed', { msg: e.message }));
        /* Revert — re-read AI status from /status on next poll */
    }
}

/* ---------- 11. Flash LED POST /led ---------- */

let ledTimeout = null;

function saveLED() {
    clearTimeout(ledTimeout);
    ledTimeout = setTimeout(async () => {
        try {
            const brightness = parseInt(document.getElementById('led-brightness').value);
            await api('/api/led', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ brightness: brightness })
            });
            document.getElementById('led-brightness-val').textContent = brightness;
        } catch (e) {
            showToast(window.i18n.t('toast.led_failed', { msg: e.message }));
        }
    }, 300);
}

/* ---------- 12. Network save POST /config + reboot ---------- */

async function saveWiFi() {
    try {
        const ssid = document.getElementById('wifi-ssid').value;
        const pass = document.getElementById('wifi-pass').value;
        const payload = {};
        if (ssid) payload.wifi_ssid = ssid;
        if (pass) payload.wifi_pass = pass;
        if (!payload.wifi_ssid) {
            showToast(window.i18n.t('network.ssid_required'), 3000);
            return;
        }
        await api('/api/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        showToast(window.i18n.t('toast.wifi.saved'));
        let count = 10;
        const countdown = setInterval(() => {
            count--;
            if (count > 0) {
                showToast(window.i18n.t('network.countdown', { s: String(count) }), 1200);
            } else {
                clearInterval(countdown);
            }
        }, 1000);
    } catch (e) {
        showToast(window.i18n.t('toast.save_failed', { msg: e.message }));
    }
}

/* ---------- 13. Streaming save POST /config ---------- */

async function saveStreaming() {
    try {
        const user = document.getElementById('rtsp-user').value;
        const pass = document.getElementById('rtsp-pass').value;
        const onvif = document.getElementById('onvif-enable').classList.contains('active');
        const payload = {};
        if (user) payload.rtsp_user = user;
        if (pass) payload.rtsp_pass = pass;
        payload.onvif_enable = onvif;
        await api('/api/config', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        showToast(window.i18n.t('toast.saved'));
    } catch (e) {
        showToast(window.i18n.t('toast.save_failed', { msg: e.message }));
    }
}

/* ---------- 14. Stream reconnect with exponential backoff ---------- */

function initStreamReconnect() {
    const img = document.getElementById('stream-img');
    let reconnectDelay = 1000;
    const MAX_DELAY = 30000;

    /* Initial connect: absolute URL to the independent MJPEG TCP server on :81 */
    img.src = 'http://' + window.location.hostname + ':81/stream';
    img.addEventListener('error', () => {
        /* img may already have been refreshed; debounce */
        if (img._reconnecting) return;
        img._reconnecting = true;

        setTimeout(() => {
            img.src = 'http://' + window.location.hostname + ':81/stream?' + Date.now();
            img._reconnecting = false;
        }, reconnectDelay);
        reconnectDelay = Math.min(reconnectDelay * 1.5, MAX_DELAY);
    });

    img.addEventListener('load', () => {
        reconnectDelay = 1000;
        img._reconnecting = false;
    });
}

/* ---------- 15. Init on DOMContentLoaded ---------- */

document.addEventListener('DOMContentLoaded', () => {
    Theme.init();
    initTabs();
    initStreamReconnect();

    /* Theme toggle */
    initToggle('toggle-theme', () => Theme.toggle());

    /* Language toggle */
    initToggle('toggle-lang', () => {
        const newLang = window.i18n.getLang() === 'en' ? 'zh' : 'en';
        window.i18n.setLang(newLang);
        document.getElementById('toggle-lang').setAttribute('aria-checked',
            newLang === 'zh' ? 'true' : 'false');
        /* Re-apply i18n to any dynamically created content */
        window.i18n.apply();
    });

    /* Camera toggles */
    initToggle('cam-hmirror', () => saveCamera());
    initToggle('cam-vflip', () => saveCamera());

    /* AI toggles */
    initToggle('ai-face', () => saveAI());
    initToggle('ai-motion', () => saveAI());
    initToggle('ai-qr', () => saveAI());

    /* ONVIF toggle */
    initToggle('onvif-enable', () => saveStreaming());

    /* Range inputs with live value display + auto-save */
    const rangeIds = ['cam-quality', 'cam-brightness', 'cam-contrast',
                      'cam-saturation', 'cam-sharpness', 'led-brightness'];
    rangeIds.forEach(id => {
        const input = document.getElementById(id);
        const valSpan = document.getElementById(id + '-val');
        if (input && valSpan) {
            input.addEventListener('input', () => {
                valSpan.textContent = input.value;
                if (id.startsWith('cam-')) saveCamera();
                if (id.startsWith('led-')) saveLED();
            });
        }
    });

    /* Framesize select change */
    document.getElementById('cam-framesize').addEventListener('change', () => {
        updateAIVGACoupling();
        saveCamera();
    });

    /* Save buttons */
    document.getElementById('btn-wifi-save')?.addEventListener('click', saveWiFi);
    document.getElementById('btn-streaming-save')?.addEventListener('click', saveStreaming);

    /* Initial data load */
    loadCamera();

    /* Polling loops */
    pollStatus();
    pollAI();
    setInterval(pollStatus, 500);
    setInterval(pollAI, 500);
});

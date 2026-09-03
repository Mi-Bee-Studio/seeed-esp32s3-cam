'use strict';

/* ==================================================================
 * MiBee Cam — app.js v3 "Honey"（统一交互层，三仓 md5 一致）
 *
 * 板差异全部运行时来自 API（契约 v1.0，docs/api-contract.md）:
 *   - capabilities 驱动 tab/按钮显隐
 *   - /api/camera 字段缺省驱动传感器控件隐藏
 *   - supported_resolutions 动态填充分辨率
 * 交互规范:
 *   - 所有异步按钮必须有 busy 态；破坏性操作走 Modal.confirm
 *   - 401/SET_PASSWORD_FIRST 统一走 AuthSheet（解锁/首设），成功后自动重试原操作
 *   - 流断线 → 骨架屏 + 重连提示；恢复 → 隐藏
 * ================================================================ */

/* ---------- 1. utils ---------- */

const $ = (id) => document.getElementById(id);

function icon(name, cls) {
    return `<svg class="icon ${cls || ''}"><use href="#i-${name}"/></svg>`;
}

function fmtKB(bytes) {
    if (bytes === undefined || bytes === null) return '--';
    if (bytes >= 1024 * 1024 * 1024) return (bytes / 1024 / 1024 / 1024).toFixed(2) + 'GB';
    if (bytes >= 1024 * 1024) return (bytes / 1024 / 1024).toFixed(1) + 'MB';
    return (bytes / 1024).toFixed(0) + 'KB';
}

function fmtUptime(s) {
    if (s === undefined || s === null) return '--';
    const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
    if (d > 0) return `${d}d ${h}h`;
    if (h > 0) return `${h}h ${m}m`;
    return `${m}m ${s % 60}s`;
}

function debounce(fn, ms) {
    let t = null;
    return (...args) => { clearTimeout(t); t = setTimeout(() => fn(...args), ms); };
}

/* SD 在位判断：新固件报 sd_present；旧固件只有 sd_total_mb/sd_total_bytes。
 * 2026-09-03 事故：ai-thinker 旧固件无 sd_present，存储卡片一直显示"未检测到 SD 卡"
 * 而文件列表却正常——前端必须兼容两种字段集。 */
function sdIsPresent(d) {
    if (d.sd_present !== undefined) return d.sd_present === true;
    return (d.sd_total_bytes > 0) || (d.sd_total_mb > 0);
}
function sdTotals(d) {
    const total = d.sd_total_bytes !== undefined ? d.sd_total_bytes
        : d.sd_total_mb !== undefined ? d.sd_total_mb * 1048576 : 0;
    const free = d.sd_free_bytes !== undefined ? d.sd_free_bytes
        : d.sd_free_mb !== undefined ? d.sd_free_mb * 1048576 : 0;
    return { total, free };
}

/* 忙态按钮：await busy(btn, asyncFn) */
async function busy(btn, fn) {
    if (!btn || btn.classList.contains('is-busy')) return;
    btn.classList.add('is-busy');
    try { return await fn(); }
    finally { btn.classList.remove('is-busy'); }
}

/* 滑杆轨道填充（webkit 无原生 progress 样式） */
function paintSlider(input) {
    const min = Number(input.min), max = Number(input.max);
    const p = ((Number(input.value) - min) / (max - min)) * 100;
    input.style.setProperty('--track',
        `linear-gradient(to right, var(--accent) 0%, var(--accent) ${p}%, var(--surface-3) ${p}%)`);
}

/* ---------- 2. Theme / lang ---------- */

const Theme = {
    KEY: 'mibee.theme',
    apply(dark) {
        if (dark) document.documentElement.setAttribute('data-theme', 'dark');
        else document.documentElement.removeAttribute('data-theme');
        $('icon-theme').innerHTML = `<use href="#i-${dark ? 'sun' : 'moon'}"/>`;
    },
    isDark() { return document.documentElement.getAttribute('data-theme') === 'dark'; },
    init() {
        const stored = localStorage.getItem(this.KEY);
        const dark = stored ? stored === 'dark'
            : window.matchMedia('(prefers-color-scheme: dark)').matches;
        this.apply(dark);
    },
    toggle() {
        const dark = !this.isDark();
        localStorage.setItem(this.KEY, dark ? 'dark' : 'light');
        this.apply(dark);
    }
};

/* ---------- 3. Toast ---------- */

function toast(msg, opts = {}) {
    const root = $('toast-root');
    const el = document.createElement('div');
    el.className = `toast tone-${opts.type || 'info'}`;
    const ic = opts.type === 'success' ? 'check' : opts.type === 'error' ? 'alert' : 'info';
    el.innerHTML = icon(ic) + `<span></span>`;
    el.lastElementChild.textContent = msg;
    root.appendChild(el);
    while (root.children.length > 3) root.firstElementChild.remove();
    setTimeout(() => {
        el.classList.add('out');
        setTimeout(() => el.remove(), 220);
    }, opts.duration || 2600);
}

/* ---------- 4. Modal ---------- */

const Modal = {
    _dismiss: null,

    _open(build) {
        $('modal-root').innerHTML = '';
        const backdrop = document.createElement('div');
        backdrop.className = 'modal-backdrop';
        const modal = document.createElement('div');
        modal.className = 'modal';
        build(modal, (result) => {
            backdrop.remove();
            if (Modal._dismiss) { const d = Modal._dismiss; Modal._dismiss = null; d(result); }
        });
        backdrop.appendChild(modal);
        backdrop.addEventListener('click', (e) => {
            if (e.target === backdrop) modal.dispatchEvent(new Event('modal-cancel'));
        });
        $('modal-root').appendChild(backdrop);
    },

    /** Modal.confirm({title, message, danger, okText, cancelText}) → Promise<boolean> */
    confirm(opts = {}) {
        return new Promise((resolve) => {
            Modal._open((modal, done) => {
                modal.innerHTML = `
                    <div class="modal-icon ${opts.danger ? 'danger' : ''}">${icon(opts.icon || (opts.danger ? 'alert' : 'info'))}</div>
                    <div class="modal-title"></div>
                    <p class="modal-msg"></p>
                    <div class="modal-actions">
                        <button class="btn btn-ghost act-cancel"></button>
                        <button class="btn ${opts.danger ? 'btn-danger' : 'btn-primary'} act-ok"></button>
                    </div>`;
                modal.querySelector('.modal-title').textContent = opts.title || '';
                modal.querySelector('.modal-msg').textContent = opts.message || '';
                const cancel = modal.querySelector('.act-cancel');
                const ok = modal.querySelector('.act-ok');
                cancel.textContent = opts.cancelText || window.i18n.t('btn.cancel');
                ok.textContent = opts.okText || window.i18n.t('btn.confirm');
                const onCancel = () => done(false);
                cancel.addEventListener('click', onCancel);
                modal.addEventListener('modal-cancel', onCancel);
                ok.addEventListener('click', () => done(true));
            });
            Modal._dismiss = resolve;
        });
    },

    close() {
        if (Modal._dismiss) { const d = Modal._dismiss; Modal._dismiss = null; d(false); }
        $('modal-root').innerHTML = '';
    }
};

/* ---------- 5. Auth（会话密码 + 鉴权抽屉） ---------- */

const Auth = {
    KEY: 'mibee.pw',
    get() { return sessionStorage.getItem(this.KEY) || ''; },
    set(pw) { sessionStorage.setItem(this.KEY, pw); },
    clear() { sessionStorage.removeItem(this.KEY); },

    _open: false,

    /** 打开鉴权抽屉。mode: 'unlock' | 'set'。返回 Promise<boolean> 是否成功 */
    ensure(mode = 'unlock') {
        if (Auth._open) return Promise.resolve(false);
        Auth._open = true;
        return new Promise((resolve) => {
            const isSet = mode === 'set';
            Modal._open((modal, done) => {
                modal.innerHTML = `
                    <div class="modal-icon">${icon('lock')}</div>
                    <div class="modal-title"></div>
                    <p class="modal-msg"></p>
                    <div class="pw-field">
                        <input type="password" id="auth-pw" autocomplete="current-password">
                        <button class="pw-eye" id="auth-eye" tabindex="-1">${icon('eye', 'sm')}</button>
                    </div>
                    <div class="field-error" id="auth-err"></div>
                    <div class="modal-actions">
                        <button class="btn btn-ghost act-cancel"></button>
                        <button class="btn btn-primary act-ok"></button>
                    </div>`;
                const t = window.i18n.t;
                modal.querySelector('.modal-title').textContent = t(isSet ? 'auth.set_title' : 'auth.unlock_title');
                modal.querySelector('.modal-msg').textContent = t(isSet ? 'auth.set_msg' : 'auth.unlock_msg');
                const pwInput = modal.querySelector('#auth-pw');
                pwInput.placeholder = t('auth.placeholder');
                modal.querySelector('.act-cancel').textContent = t('btn.cancel');
                const okBtn = modal.querySelector('.act-ok');
                okBtn.textContent = t(isSet ? 'auth.set_ok' : 'auth.unlock_ok');
                const errEl = modal.querySelector('#auth-err');

                const eye = modal.querySelector('#auth-eye');
                eye.addEventListener('click', () => {
                    const show = pwInput.type === 'password';
                    pwInput.type = show ? 'text' : 'password';
                    eye.innerHTML = icon(show ? 'eye-off' : 'eye', 'sm');
                });

                const finish = (ok) => { Auth._open = false; done(ok); resolve(ok); };

                const submit = () => busy(okBtn, async () => {
                    const pw = pwInput.value;
                    if (!pw) { errEl.textContent = t('auth.empty'); return; }
                    try {
                        if (isSet) {
                            /* 首设密码：直接写配置 */
                            const resp = await fetch('/api/config', {
                                method: 'POST',
                                headers: { 'Content-Type': 'application/json' },
                                body: JSON.stringify({ web_password: pw })
                            });
                            const j = await resp.json().catch(() => ({}));
                            if (!j.ok) throw new Error(j.error || `HTTP ${resp.status}`);
                        } else {
                            /* 解锁：校验 */
                            const resp = await fetch('/api/auth', { headers: { 'X-Password': pw } });
                            if (resp.status === 401) throw new Error('unauthorized');
                        }
                        Auth.set(pw);
                        toast(t('auth.ok'), { type: 'success' });
                        finish(true);
                    } catch (e) {
                        errEl.textContent = t(isSet ? 'auth.set_fail' : 'auth.wrong');
                    }
                });

                okBtn.addEventListener('click', submit);
                pwInput.addEventListener('keydown', (e) => { if (e.key === 'Enter') submit(); });
                modal.addEventListener('modal-cancel', () => finish(false));
                setTimeout(() => pwInput.focus(), 120);
            });
            Modal._dismiss = null; /* AuthSheet 自管 resolve */
        });
    }
};

/* ---------- 6. API helper（401 → AuthSheet → 自动重试一次） ---------- */

let apiSeq = 0;
async function api(path, options = {}, opts = {}) {
    /* 密码必须在 attempt() 内部读取：2026-09-03 发现 401→AuthSheet→重试
     * 用的是入口时构建的旧 headers（无 X-Password），首次写操作必然二次 401 */
    const attempt = async () => {
        const headers = Object.assign({}, options.headers);
        const pw = Auth.get();
        if (pw) headers['X-Password'] = pw;
        const resp = await fetch(path, Object.assign({}, options, { headers }));
        let json;
        try { json = await resp.json(); }
        catch (e) { throw new Error(`HTTP ${resp.status}`); }
        if (!json.ok) {
            const err = new Error(json.error || `HTTP ${resp.status}`);
            err.status = resp.status;
            err.code = json.error;
            return { __fail: err };
        }
        return { data: json.data || {} };
    };

    let r = await attempt();
    if (r.__fail && (r.__fail.status === 401)) {
        const needSet = r.__fail.code === 'SET_PASSWORD_FIRST';
        if (!opts.noAuthSheet) {
            const ok = await Auth.ensure(needSet ? 'set' : 'unlock');
            if (ok) r = await attempt();
        }
    }
    if (r.__fail) throw r.__fail;
    return r.data;
}

/* ---------- 7. Capabilities ---------- */

const Caps = {};

function showSeg(name) {
    const b = document.querySelector(`.seg-btn[data-tab="${name}"]`);
    if (b) b.hidden = false;
}

function fillStreamUrls() {
    const host = window.location.hostname;
    $('stream-url-mjpeg').value = 'http://' + host + ':81/stream';
    if (Caps.rtsp) {
        $('row-stream-rtsp').hidden = false;
        $('stream-url-rtsp').value = 'rtsp://' + host + ':554/stream';
    }
    if (Caps.onvif) $('row-stream-onvif').hidden = false;
}

function applyCapabilities() {
    if (Caps.ai) showSeg('ai');
    if (Caps.flash_led) showSeg('flash');
    if (Caps.sd) showSeg('storage');
    if (Caps.recording) $('btn-record').hidden = false;
    if (Caps.audio) $('btn-audio').hidden = false;
    if (Caps.websocket) WS.connect();
    if (Caps.ota) $('ota-panel').hidden = false;
    if (Caps.wifi_scan) $('btn-wifi-scan').hidden = false;
    if (Caps.rtsp || Caps.onvif) showSeg('streaming');
}

/* ---------- 8. Tabs ---------- */

function initTabs() {
    document.querySelectorAll('.seg-btn').forEach(btn => {
        btn.addEventListener('click', () => {
            document.querySelectorAll('.seg-btn').forEach(b => b.classList.remove('active'));
            document.querySelectorAll('.deck-page').forEach(p => p.classList.remove('active'));
            btn.classList.add('active');
            const page = $('page-' + btn.dataset.tab);
            if (page) page.classList.add('active');
        });
    });
}

/* ---------- 9. Status polling ---------- */

let lastRecState = 'idle';
let lastStatus = null;

function statChip(labelKey, value, tone, iconName, titleKey) {
    const t = window.i18n.t;
    return `<span class="stat-chip ${tone || ''}" title="${t(titleKey || labelKey)}">` +
           `${icon(iconName)}<i>${t(labelKey)}</i><b>${value}</b></span>`;
}

async function pollStatus() {
    let d;
    try { d = await api('/api/status', {}, { noAuthSheet: true }); }
    catch (e) { $('net-dot').className = 'status-dot bad'; return; }
    lastStatus = d;

    /* header */
    $('net-dot').className = 'status-dot ' + (d.wifi_state === 'connected' ? 'ok' : d.wifi_state === 'ap' ? 'ok' : 'bad');
    $('hd-name').textContent = d.device_name || 'MiBee Cam';
    const curSsid = d.current_ssid !== undefined && d.current_ssid && d.wifi_state === 'connected'
        ? d.current_ssid : '';
    $('hd-sub').textContent = `${curSsid ? curSsid + ' · ' : ''}${d.camera || ''} · ${d.resolution || '--'} · ${d.ip || '--'}`;
    $('cam-sub').textContent = `${d.camera || '--'} @ ${d.resolution || '--'}`;

    /* current-connection row (WiFi tab) — only on firmware that reports current_ssid */
    const curRow = $('row-wifi-current');
    if (curRow && d.current_ssid !== undefined) {
        const t = window.i18n.t;
        curRow.hidden = false;
        const ok = d.wifi_state === 'connected';
        $('wifi-current-dot').className = 'status-dot ' + (ok ? 'ok' : 'bad');
        $('wifi-current-name').textContent = ok ? (d.current_ssid || '—')
            : t('wifi.state.' + (d.wifi_state || 'disconnected'));
        const bits = [];
        if (ok) {
            if (d.wifi_net !== undefined) bits.push(t('network.net_' + d.wifi_net));
            if (d.wifi_channel !== undefined) bits.push('ch' + d.wifi_channel);
            if (d.wifi_rssi !== undefined) bits.push(d.wifi_rssi + ' dBm');
        }
        $('wifi-current-detail').textContent = bits.join(' · ');
    }

    /* stage chips */
    $('chip-clients').textContent =
        (d.stream_clients ?? 0) + (d.stream_clients_max !== undefined ? '/' + d.stream_clients_max : '');

    /* recording */
    const rec = d.recording || 'idle';
    lastRecState = rec;
    const pill = $('rec-pill');
    const isRec = rec === 'recording' || rec === 'paused';
    pill.hidden = !isRec;
    if (isRec) $('rec-file').textContent = window.i18n.t('status.recording.recording');
    if (!$('btn-record').hidden) {
        const btn = $('btn-record');
        btn.classList.toggle('is-rec', isRec);
        btn.innerHTML = icon(isRec ? 'stop' : 'record') +
            `<span>${window.i18n.t(isRec ? 'storage.stop' : 'storage.record')}</span>`;
    }

    /* stats strip — 每个数值都带人话标签（普通人可读） */
    const chips = [];
    const rssi = d.wifi_rssi;
    if (rssi !== undefined && d.wifi_state === 'connected') {
        const tone = rssi > -60 ? 'tone-ok' : rssi > -75 ? '' : 'tone-warn';
        chips.push(statChip('stats.rssi', rssi + ' dBm', tone, 'wifi', 'stats.rssi.hint'));
    } else {
        chips.push(statChip('stats.link', window.i18n.t('wifi.state.' + (d.wifi_state || 'disconnected')), '', 'wifi', 'stats.rssi.hint'));
    }
    chips.push(statChip('stats.heap', fmtKB(d.free_heap), '', 'chip', 'stats.heap.hint'));
    if (d.free_psram !== undefined) chips.push(statChip('stats.psram', fmtKB(d.free_psram), 'tone-ok', 'sd', 'stats.psram.hint'));
    if (d.chip_temp !== undefined) {
        const temp = d.chip_temp;
        chips.push(statChip('stats.temp', (temp.toFixed ? temp.toFixed(0) : temp) + '°C',
            temp > 85 ? 'tone-danger' : temp > 75 ? 'tone-warn' : '', 'temp', 'stats.temp.hint'));
    }
    if (sdIsPresent(d)) {
        const { total, free } = sdTotals(d);
        const pct = d.sd_free_percent !== undefined ? d.sd_free_percent
            : total > 0 ? (free / total * 100) : 0;
        chips.push(statChip('stats.sd', Math.round(pct) + '%',
            pct < 20 ? 'tone-danger' : pct < 40 ? 'tone-warn' : 'tone-ok', 'sd', 'stats.sd.hint'));
    }
    chips.push(statChip('stats.uptime', fmtUptime(d.uptime), '', 'clock', 'stats.uptime.hint'));
    $('stats-strip').innerHTML = chips.join('');

    /* system card */
    $('sys-sub').textContent = d.firmware_version || '';
    $('sys-fw').textContent = d.firmware_version || '--';
    $('sys-uptime').textContent = fmtUptime(d.uptime);
    $('sys-heap').textContent = fmtKB(d.free_heap);
    if (d.min_heap !== undefined) { $('row-sys-min-heap').hidden = false; $('sys-min-heap').textContent = fmtKB(d.min_heap); }
    if (d.free_psram !== undefined) { $('row-sys-psram').hidden = false; $('sys-psram').textContent = fmtKB(d.free_psram); }
    if (d.chip_temp !== undefined) {
        $('row-sys-temp').hidden = false;
        $('sys-temp').textContent = (d.chip_temp.toFixed ? d.chip_temp.toFixed(1) : d.chip_temp) + '°C';
    }

    /* storage usage — 新固件报 sd_present/sd_*_bytes；旧固件只有 sd_*_mb */
    if (Caps.sd) {
        const box = $('storage-usage');
        if (sdIsPresent(d)) {
            const { total, free } = sdTotals(d);
            if (total > 0) {
                box.hidden = false;
                const freePct = d.sd_free_percent !== undefined ? d.sd_free_percent
                    : (free / total * 100);
                const usedPct = 100 - freePct;
                const fill = $('sd-usage-fill');
                fill.style.width = usedPct + '%';
                fill.className = 'usage-fill' + (usedPct > 85 ? ' danger' : usedPct > 70 ? ' warn' : '');
                $('sd-usage-text').textContent = window.i18n.t('storage.used', { pct: String(Math.round(usedPct)) });
                $('sd-usage-free').textContent = fmtKB(free) + ' / ' + fmtKB(total);
                $('storage-sub').textContent = window.i18n.t('storage.sd_free', { pct: String(Math.round(freePct)) });
            } else {
                box.hidden = true;
                $('storage-sub').textContent = window.i18n.t('storage.sd_ok');
            }
        } else {
            box.hidden = true;
            $('storage-sub').textContent = window.i18n.t('storage.no_sd');
        }
    }

    pollRecordFallback();
}

/* 录像状态：seeed 放在 status.recording；ai-thinker 等只有 /api/record。
 * 每 3 个轮询周期补取一次，把结果写回 lastStatus.recording 供其他逻辑使用 */
let recPollTick = 0;
let recPollBusy = false;
async function pollRecordFallback() {
    if (!Caps.recording || lastStatus === null) return;
    if (lastStatus.recording !== undefined) return;
    if (recPollTick++ % 3 !== 0 || recPollBusy) return;
    recPollBusy = true;
    try {
        const d = await api('/api/record', {}, { noAuthSheet: true });
        const s = String(d.state || d.status || '').toLowerCase();
        lastStatus.recording = s.includes('record') ? 'recording'
            : s.includes('pause') ? 'paused' : 'idle';
    } catch (e) { /* 设备重启/掉线窗口，下个周期再试 */ }
    recPollBusy = false;
}

/* ---------- 10. AI overlay + toggles ---------- */

async function pollAI() {
    let d;
    try { d = await api('/api/ai/status', {}, { noAuthSheet: true }); }
    catch (e) { return; }
    const canvas = $('stream-overlay');
    const ctx = canvas.getContext('2d');
    const img = $('stream-img');
    canvas.width = img.clientWidth || 640;
    canvas.height = img.clientHeight || 480;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    const face = d.face;
    if (face && face.boxes && face.boxes.length) {
        const sx = canvas.width / 640, sy = canvas.height / 480;
        ctx.strokeStyle = '#FFB020';
        ctx.lineWidth = 2.5;
        face.boxes.forEach(b => ctx.strokeRect(b.x * sx, b.y * sy, b.w * sx, b.h * sy));
    }
    const motion = d.motion;
    if (motion && motion.score > 0) {
        ctx.fillStyle = 'rgba(255, 176, 32, 0.9)';
        ctx.font = '600 13px -apple-system, sans-serif';
        ctx.fillText('M ' + motion.score.toFixed(0), 10, 18);
    }
    const qr = d.qr;
    if (qr && qr.count > 0) {
        ctx.fillStyle = 'rgba(76, 194, 255, 0.9)';
        ctx.font = '600 13px -apple-system, sans-serif';
        ctx.fillText('QR ' + qr.count, canvas.width - 52, 18);
    }
}

function vgaValue() {
    for (const opt of $('cam-framesize').options) {
        if ((opt.textContent || '').indexOf('VGA') === 0) return opt.value;
    }
    return null;
}

function updateAIVGACoupling() {
    if (!Caps.ai) return;
    const vga = vgaValue();
    const select = $('cam-framesize');
    const isVGA = vga !== null && select.value === vga;
    const anyAI = ['ai-face', 'ai-motion', 'ai-qr'].some(id => $(id).classList.contains('active'));
    ['ai-face', 'ai-motion', 'ai-qr'].forEach(id => $(id).disabled = !isVGA);
    Array.from(select.options).forEach(opt => { opt.disabled = anyAI && vga !== null && opt.value !== vga; });
}

async function saveAI() {
    try {
        await api('/api/ai', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
                face: $('ai-face').classList.contains('active'),
                motion: $('ai-motion').classList.contains('active'),
                qr: $('ai-qr').classList.contains('active'),
            })
        });
        updateAIVGACoupling();
    } catch (e) {
        toast(window.i18n.t('toast.ai_failed', { msg: e.message }), { type: 'error' });
    }
}

/* ---------- 11. Camera panel ---------- */

let cameraData = null;

async function loadCamera() {
    try { cameraData = await api('/api/camera', {}, { noAuthSheet: true }); }
    catch (e) { return; }
    const d = cameraData;

    const select = $('cam-framesize');
    select.innerHTML = '';
    const list = (d.supported_resolutions && d.supported_resolutions.length)
        ? d.supported_resolutions
        : [{ label: d.resolution || 'VGA', value: d.cam_framesize }];
    list.forEach(r => {
        const opt = document.createElement('option');
        opt.value = r.value; opt.textContent = r.label;
        select.appendChild(opt);
    });
    select.value = String(d.cam_framesize);

    const q = $('cam-quality');
    q.value = d.cam_quality ?? 12;
    $('cam-quality-val').textContent = q.value;
    paintSlider(q);

    ['brightness', 'contrast', 'saturation', 'sharpness'].forEach(k => {
        const row = $('row-cam-' + k), input = $('cam-' + k);
        if (d['cam_' + k] !== undefined) {
            row.hidden = false;
            input.value = d['cam_' + k];
            $('cam-' + k + '-val').textContent = input.value;
            paintSlider(input);
        } else row.hidden = true;
    });

    if (d.day_night_mode !== undefined) {
        $('row-cam-day-night').hidden = false;
        $('cam-day-night').querySelectorAll('button')
            .forEach(b => b.classList.toggle('active', b.value === String(d.day_night_mode)));
    }
    if (d.cam_hmirror !== undefined) { $('row-cam-hmirror').hidden = false; setToggle('cam-hmirror', d.cam_hmirror); }
    if (d.cam_vflip !== undefined) { $('row-cam-vflip').hidden = false; setToggle('cam-vflip', d.cam_vflip); }

    updateAIVGACoupling();
}

const saveCamera = debounce(async () => {
    const payload = {
        cam_framesize: parseInt($('cam-framesize').value),
        cam_quality: parseInt($('cam-quality').value),
    };
    if (!$('row-cam-brightness').hidden) payload.cam_brightness = parseInt($('cam-brightness').value);
    if (!$('row-cam-contrast').hidden) payload.cam_contrast = parseInt($('cam-contrast').value);
    if (!$('row-cam-saturation').hidden) payload.cam_saturation = parseInt($('cam-saturation').value);
    if (!$('row-cam-sharpness').hidden) payload.cam_sharpness = parseInt($('cam-sharpness').value);
    if (!$('row-cam-day-night').hidden) payload.day_night_mode = parseInt($('cam-day-night').querySelector('.active').value);
    if (!$('row-cam-hmirror').hidden) payload.cam_hmirror = $('cam-hmirror').classList.contains('active');
    if (!$('row-cam-vflip').hidden) payload.cam_vflip = $('cam-vflip').classList.contains('active');
    try {
        await api('/api/camera', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        await loadCamera();
    } catch (e) {
        toast(window.i18n.t('toast.error', { msg: e.message }), { type: 'error' });
    }
}, 450);

/* ---------- 12. Toggles ---------- */

function setToggle(id, active) {
    const el = $(id);
    el.classList.toggle('active', !!active);
    el.setAttribute('aria-checked', String(!!active));
}

function initToggle(id, onChange) {
    const el = $(id);
    if (!el) return;
    el.addEventListener('click', () => {
        const active = el.classList.toggle('active');
        el.setAttribute('aria-checked', String(active));
        if (onChange) onChange(active);
    });
    el.addEventListener('keydown', (e) => {
        if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); el.click(); }
    });
}

/* ---------- 13. Flash LED ---------- */

const saveLED = debounce(async () => {
    try {
        await api('/api/led', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ brightness: parseInt($('led-brightness').value) })
        });
    } catch (e) {
        toast(window.i18n.t('toast.led_failed', { msg: e.message }), { type: 'error' });
    }
}, 250);

/* ---------- 14. Record ---------- */

async function toggleRecord() {
    const btn = $('btn-record');
    await busy(btn, async () => {
        try {
            const recording = lastRecState === 'recording' || lastRecState === 'paused';
            const action = recording ? 'stop' : 'start';
            const d = await api('/api/record?action=' + action, { method: 'POST' });
            if (d.status === 'error') throw new Error('recorder');
            lastRecState = action === 'start' ? 'recording' : 'idle';
            toast(window.i18n.t(action === 'start' ? 'toast.record_started' : 'toast.record_stopped'),
                { type: 'success' });
            pollStatus();
        } catch (e) {
            toast(window.i18n.t('toast.record_failed', { msg: e.message }), { type: 'error' });
        }
    });
}

/* ---------- 15. Files（分页 + 批量 + 格式化） ---------- */

const Files = {
    type: 'all',            /* all | photos | recordings */
    limit: 50,
    offset: 0,
    total: null,            /* 后端 reports total（ai-thinker）；null = 未知 */
    hasMore: false,
    selected: new Set(),    /* 当前已勾选文件名 */
    anyPhoto: null,         /* null=未知；首屏后用于隐藏无意义的类型筛选 */
};

function fileRow(f) {
    const row = document.createElement('div');
    row.className = 'list-item';
    const checked = Files.selected.has(f.name);
    row.innerHTML = `
        <input type="checkbox" class="file-check" ${checked ? 'checked' : ''}>
        <span class="list-icon">${icon(f.type === 'recording' ? 'cast' : 'file', 'sm')}</span>
        <div class="list-body">
            <span class="list-title"></span>
            <span class="list-sub"></span>
        </div>
        <button class="icon-btn act-dl" data-i18n-title="storage.download">${icon('download', 'sm')}</button>
        <button class="icon-btn act-del" data-i18n-title="storage.delete" style="color:var(--danger)">${icon('trash', 'sm')}</button>`;
    row.querySelector('.list-title').textContent = f.name;
    row.querySelector('.list-sub').textContent =
        `${fmtKB(f.size)} · ${f.date || f.time || ''}`;
    const cb = row.querySelector('.file-check');
    cb.addEventListener('change', () => {
        if (cb.checked) Files.selected.add(f.name);
        else Files.selected.delete(f.name);
        updateBatchButtons();
    });
    row.querySelector('.act-dl').addEventListener('click', () => {
        let url = '/api/download?name=' + encodeURIComponent(f.name);
        if (f.type) url += '&type=' + f.type;
        window.open(url, '_blank');
    });
    row.querySelector('.act-del').addEventListener('click', async () => {
        const ok = await Modal.confirm({
            title: window.i18n.t('storage.delete'),
            message: window.i18n.t('storage.confirm_delete', { name: f.name }),
            danger: true,
            okText: window.i18n.t('storage.delete'),
        });
        if (!ok) return;
        try {
            await api('/api/files/batch', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ names: [f.name] })
            });
            Files.selected.delete(f.name);
            toast(window.i18n.t('toast.deleted'), { type: 'success' });
            loadFiles();
        } catch (e) {
            toast(window.i18n.t('toast.delete_failed', { msg: e.message }), { type: 'error' });
        }
    });
    return row;
}

function updateBatchButtons() {
    const n = Files.selected.size;
    const btn = $('btn-files-del-selected');
    btn.disabled = n === 0;
    const label = window.i18n.t('files.delete_selected');
    btn.textContent = n > 0 ? `${label} (${n})` : label;
}

async function batchDelete(payload, btn) {
    await busy(btn, async () => {
        try {
            const d = await api('/api/files/batch', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload)
            });
            const del = (d && d.deleted !== undefined) ? d.deleted : '?';
            const fail = (d && d.failed !== undefined) ? d.failed : 0;
            if (fail > 0) toast(window.i18n.t('toast.batch_partial', { ok: String(del), fail: String(fail) }), { type: 'error', duration: 4000 });
            else toast(window.i18n.t('toast.batch_done', { ok: String(del) }), { type: 'success' });
            Files.selected.clear();
            loadFiles();
        } catch (e) {
            toast(window.i18n.t('toast.delete_failed', { msg: e.message }), { type: 'error' });
        }
    });
}

async function loadFiles(reset = true) {
    const listEl = $('file-list');
    if (reset) {
        Files.offset = 0;
        Files.total = null;
        listEl.innerHTML = `<div class="empty"><div class="spinner"></div></div>`;
    }
    $('files-bar').hidden = true;
    $('btn-files-more').hidden = true;
    try {
        const d = await api(`/api/files?type=${Files.type}&offset=${Files.offset}&limit=${Files.limit}`, {}, { noAuthSheet: true });
        const files = d.files || [];
        if (reset) listEl.innerHTML = '';
        if (d.total !== undefined) Files.total = d.total;
        /* 只有后端回报 total（ai-thinker）才真正分页；不支持 offset 的板
         * （seeed 固定返回最近 64 条）禁止"加载更多"，否则翻页会重复追加 */
        Files.hasMore = Files.total !== null
            ? (Files.offset + files.length) < Files.total
            : false;

        if (Files.offset === 0 && !files.length) {
            listEl.innerHTML = `<div class="empty">${icon('folder', 'sm')}<span>${window.i18n.t('storage.empty')}</span></div>`;
        } else {
            files.forEach(f => {
                if (f.type === 'photo') Files.anyPhoto = true;
                listEl.appendChild(fileRow(f));
            });
        }

        /* 类型筛选：板上一张照片都没有时隐藏"照片"（seeed 只有录像） */
        if (Files.anyPhoto === null && files.length) {
            Files.anyPhoto = files.some(f => f.type === 'photo');
            document.querySelectorAll('#files-type-seg [data-type="photos"]')
                .forEach(b => b.hidden = Files.anyPhoto === false);
        }

        $('files-bar').hidden = false;
        $('btn-files-more').hidden = !Files.hasMore;
        updateBatchButtons();
    } catch (e) {
        if (reset) listEl.innerHTML =
            `<div class="empty">${icon('folder', 'sm')}<span>${window.i18n.t('storage.empty')}</span></div>`;
    }
}

async function loadMoreFiles() {
    const btn = $('btn-files-more');
    await busy(btn, async () => {
        Files.offset += Files.limit;
        await loadFiles(false);
    });
}

function initFilesUI() {
    $('files-type-seg').querySelectorAll('button').forEach(b => {
        b.addEventListener('click', () => {
            $('files-type-seg').querySelectorAll('button').forEach(x => x.classList.remove('active'));
            b.classList.add('active');
            Files.type = b.dataset.type;
            Files.selected.clear();
            loadFiles();
        });
    });
    $('btn-files-selectall').addEventListener('click', () => {
        const boxes = $('file-list').querySelectorAll('.file-check');
        const allChecked = Array.from(boxes).every(cb => cb.checked);
        boxes.forEach(cb => {
            cb.checked = !allChecked;
            cb.dispatchEvent(new Event('change'));
        });
    });
    $('btn-files-del-selected').addEventListener('click', async () => {
        const names = Array.from(Files.selected);
        const ok = await Modal.confirm({
            title: window.i18n.t('files.delete_selected'),
            message: window.i18n.t('files.confirm_batch', { n: String(names.length) }),
            danger: true,
            okText: window.i18n.t('storage.delete'),
        });
        if (ok) batchDelete({ names }, $('btn-files-del-selected'));
    });
    $('btn-files-clear').addEventListener('click', async () => {
        const t = window.i18n.t;
        const scope = Files.type === 'all' ? 'all' : Files.type;
        const ok = await Modal.confirm({
            title: t('files.clear_type'),
            message: t('files.confirm_clear', { type: t('files.type_' + scope) }),
            danger: true,
            okText: t('files.clear_type'),
        });
        if (ok) batchDelete({ scope }, $('btn-files-clear'));
    });
    $('btn-files-format').addEventListener('click', formatSD);
    $('btn-files-more').addEventListener('click', () => loadMoreFiles());
}

async function formatSD() {
    const t = window.i18n.t;
    const ok1 = await Modal.confirm({
        title: t('files.format'),
        message: t('files.format_warn'),
        danger: true,
        okText: t('files.format'),
    });
    if (!ok1) return;
    /* 格式化不可逆：第二道确认 */
    const ok2 = await Modal.confirm({
        title: t('files.format'),
        message: t('files.format_warn2'),
        danger: true,
        icon: 'alert',
        okText: t('files.format_sure'),
    });
    if (!ok2) return;
    const btn = $('btn-files-format');
    await busy(btn, async () => {
        try {
            const d = await api('/api/format', { method: 'POST' });
            toast(d && d.message ? d.message : t('toast.format_done'), { type: 'success', duration: 5000 });
            Files.selected.clear();
            loadFiles();
            pollStatus();
        } catch (e) {
            toast(window.i18n.t('toast.format_failed', { msg: e.message }), { type: 'error', duration: 5000 });
        }
    });
}

/* ---------- 16. WiFi scan ---------- */

async function wifiScan() {
    const btn = $('btn-wifi-scan');
    const listEl = $('scan-list');
    await busy(btn, async () => {
        try {
            const d = await api('/api/scan', {}, { noAuthSheet: true });
            listEl.innerHTML = '';
            const nets = d.networks || [];
            if (!nets.length) {
                listEl.innerHTML = `<div class="empty"><span>${window.i18n.t('network.scan_empty')}</span></div>`;
            }
            nets.forEach(n => {
                const item = document.createElement('button');
                item.className = 'list-item';
                item.style.width = '100%';
                item.style.textAlign = 'left';
                item.innerHTML = `
                    <span class="list-icon">${icon('wifi', 'sm')}</span>
                    <div class="list-body">
                        <span class="list-title"></span>
                        <span class="list-sub">${n.rssi} dBm</span>
                    </div>`;
                item.querySelector('.list-title').textContent = n.ssid || '(hidden)';
                item.addEventListener('click', () => {
                    $('wifi-ssid').value = n.ssid || '';
                    listEl.hidden = true;
                });
                listEl.appendChild(item);
            });
            listEl.hidden = false;
        } catch (e) {
            toast(window.i18n.t('toast.error', { msg: e.message }), { type: 'error' });
        }
    });
}

/* ---------- 17. Config load / save ---------- */

/* Board exposes dual-WiFi config only when GET /api/config returns wifi_ssid_2
 * (n16r8's POST validates against a known-key whitelist — never send it there). */
let hasWifi2 = false;

async function loadConfig() {
    let d;
    try { d = await api('/api/config', {}, { noAuthSheet: true }); }
    catch (e) { return; }
    $('wifi-ssid').value = d.wifi_ssid || '';
    if (d.wifi_ssid_2 !== undefined) {
        hasWifi2 = true;
        $('row-wifi2-ssid').hidden = false;
        $('row-wifi2-pass').hidden = false;
        $('wifi-ssid2').value = d.wifi_ssid_2 || '';
    }
    if (d.device_name !== undefined) $('device-name').value = d.device_name || '';
    if (d.timezone !== undefined) $('timezone').value = d.timezone || '';
    if (d.rtsp_user !== undefined) { $('row-rtsp-user').hidden = false; $('rtsp-user').value = d.rtsp_user || ''; }
    if (d.rtsp_pass !== undefined) { $('row-rtsp-pass').hidden = false; }
    if (d.onvif_enable !== undefined) { $('row-onvif-enable').hidden = false; setToggle('onvif-enable', d.onvif_enable); }
    /* 没有任何可编辑项时隐藏 Save（RTSP 凭据走 web_password 的板，该页只读展示） */
    const anyEditable = !$('row-rtsp-user').hidden || !$('row-rtsp-pass').hidden || !$('row-onvif-enable').hidden;
    $('btn-streaming-save').hidden = !anyEditable;
}

async function saveNetwork() {
    const btn = $('btn-network-save');
    await busy(btn, async () => {
        try {
            const payload = {};
            const ssid = $('wifi-ssid').value.trim();
            const pass = $('wifi-pass').value;
            const name = $('device-name').value.trim();
            const tz = $('timezone').value.trim();
            if (ssid) payload.wifi_ssid = ssid;
            if (pass) payload.wifi_pass = pass;
            if (hasWifi2) {
                payload.wifi_ssid_2 = $('wifi-ssid2').value.trim();  /* empty = disable backup */
                const pass2 = $('wifi-pass2').value;
                if (pass2) payload.wifi_pass_2 = pass2;
            }
            if (name) payload.device_name = name;
            if (tz) payload.timezone = tz;
            if (!Object.keys(payload).length) return;
            await api('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload)
            });
            const wifiChanged = payload.wifi_ssid || payload.wifi_pass ||
                (hasWifi2 && (payload.wifi_ssid_2 !== undefined || payload.wifi_pass_2));
            if (wifiChanged) {
                /* Credentials are only read at boot/failover — reboot to apply */
                toast(window.i18n.t('network.saving'), { type: 'success' });
                setTimeout(() => { api('/api/reboot', { method: 'POST' }).catch(() => {}); }, 1500);
            } else {
                toast(window.i18n.t('toast.saved'), { type: 'success' });
            }
        } catch (e) {
            toast(window.i18n.t('toast.save_failed', { msg: e.message }), { type: 'error' });
        }
    });
}

/* ---------- 17b. 修改密码（旧密码经 /api/auth 验证后写入新密码） ---------- */

function pwRow(id, placeholderKey) {
    return `
        <div class="pw-field">
            <input type="password" id="${id}" autocomplete="new-password"
                   placeholder="${window.i18n.t(placeholderKey)}">
            <button class="pw-eye" tabindex="-1">${icon('eye', 'sm')}</button>
        </div>`;
}

function openPasswordModal() {
    Modal._open((modal, done) => {
        const t = window.i18n.t;
        modal.innerHTML = `
            <div class="modal-icon">${icon('lock')}</div>
            <div class="modal-title">${t('pw.change')}</div>
            <p class="modal-msg">${t('pw.change_msg')}</p>
            ${pwRow('pw-old', 'pw.old')}
            ${pwRow('pw-new', 'pw.new')}
            ${pwRow('pw-confirm', 'pw.confirm')}
            <div class="field-error" id="pw-err"></div>
            <div class="modal-actions">
                <button class="btn btn-ghost act-cancel">${t('btn.cancel')}</button>
                <button class="btn btn-primary act-ok">${t('pw.ok')}</button>
            </div>`;
        modal.querySelectorAll('.pw-eye').forEach(eye => {
            eye.addEventListener('click', () => {
                const input = eye.parentElement.querySelector('input');
                const show = input.type === 'password';
                input.type = show ? 'text' : 'password';
                eye.innerHTML = icon(show ? 'eye-off' : 'eye', 'sm');
            });
        });
        const errEl = modal.querySelector('#pw-err');
        const okBtn = modal.querySelector('.act-ok');
        const finish = (r) => { done(r); };

        const submit = () => busy(okBtn, async () => {
            const oldPw = modal.querySelector('#pw-old').value;
            const newPw = modal.querySelector('#pw-new').value;
            const confirmPw = modal.querySelector('#pw-confirm').value;
            if (newPw.length < 6) { errEl.textContent = t('pw.too_short'); return; }
            if (newPw !== confirmPw) { errEl.textContent = t('pw.mismatch'); return; }
            /* 先用旧密码验证（改密后 /api/auth 依然可用） */
            try {
                const resp = await fetch('/api/auth', { headers: { 'X-Password': oldPw } });
                if (resp.status === 401) { errEl.textContent = t('pw.wrong_old'); return; }
            } catch (e) {
                errEl.textContent = t('toast.save_failed', { msg: e.message }); return;
            }
            try {
                await api('/api/config', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json', 'X-Password': oldPw },
                    body: JSON.stringify({ web_password: newPw })
                }, { noAuthSheet: true });
                Auth.set(newPw);
                toast(t('pw.changed'), { type: 'success' });
                finish(true);
            } catch (e) {
                errEl.textContent = t('pw.failed', { msg: e.message });
            }
        });
        okBtn.addEventListener('click', submit);
        modal.addEventListener('modal-cancel', () => finish(false));
        setTimeout(() => modal.querySelector('#pw-old').focus(), 120);
    });
}

async function saveStreaming() {
    const btn = $('btn-streaming-save');
    await busy(btn, async () => {
        try {
            const payload = {};
            if (!$('row-rtsp-user').hidden) {
                const u = $('rtsp-user').value, p = $('rtsp-pass').value;
                if (u) payload.rtsp_user = u;
                if (p) payload.rtsp_pass = p;
            }
            if (!$('row-onvif-enable').hidden) payload.onvif_enable = $('onvif-enable').classList.contains('active');
            if (!Object.keys(payload).length) return;
            await api('/api/config', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify(payload)
            });
            toast(window.i18n.t('toast.saved'), { type: 'success' });
        } catch (e) {
            toast(window.i18n.t('toast.save_failed', { msg: e.message }), { type: 'error' });
        }
    });
}

/* ---------- 18. Audio ---------- */

const MULAW_TABLE = (() => {
    const t = new Float32Array(256);
    for (let i = 0; i < 256; i++) {
        const u = ~i & 0xFF;
        const sign = u & 0x80;
        const exponent = (u >> 4) & 0x07;
        const mantissa = u & 0x0F;
        let sample = (((mantissa << 1) + 33) << exponent) - 33;
        if (sign) sample = -sample;
        t[i] = sample / 8159;
    }
    return t;
})();

const AudioMon = {
    active: false, ctx: null, controller: null,

    async toggle() {
        if (this.active) { this.stop(); return; }
        const btn = $('btn-audio');
        btn.classList.add('is-busy');
        try {
            this.controller = new AbortController();
            const resp = await fetch('/api/audio', { signal: this.controller.signal });
            if (!resp.ok || !resp.body) throw new Error('HTTP ' + resp.status);
            this.ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 8000 });
            this.active = true;
            btn.classList.remove('is-busy');
            btn.classList.add('is-on');
            btn.innerHTML = icon('speaker') + `<span>${window.i18n.t('stream.audio_stop')}</span>`;
            const reader = resp.body.getReader();
            let nextTime = this.ctx.currentTime + 0.15;
            const pump = async () => {
                while (this.active) {
                    const { done, value } = await reader.read();
                    if (done) break;
                    const now = this.ctx.currentTime;
                    if (nextTime < now) nextTime = now + 0.05;
                    const buf = this.ctx.createBuffer(1, value.length, 8000);
                    const ch = buf.getChannelData(0);
                    for (let i = 0; i < value.length; i++) ch[i] = MULAW_TABLE[value[i]];
                    const src = this.ctx.createBufferSource();
                    src.buffer = buf;
                    src.connect(this.ctx.destination);
                    src.start(nextTime);
                    nextTime += value.length / 8000;
                    if (nextTime - now > 0.25) {
                        await new Promise(r => setTimeout(r, (nextTime - now - 0.2) * 1000));
                    }
                }
                this.stop();
            };
            pump().catch(() => this.stop());
        } catch (e) {
            this.stop();
            toast(window.i18n.t('toast.audio_failed', { msg: e.message }), { type: 'error' });
        }
    },

    stop() {
        this.active = false;
        if (this.controller) { try { this.controller.abort(); } catch (_) {} this.controller = null; }
        if (this.ctx) { try { this.ctx.close(); } catch (_) {} this.ctx = null; }
        const btn = $('btn-audio');
        if (btn) {
            btn.classList.remove('is-busy', 'is-on');
            btn.innerHTML = icon('speaker') + `<span>${window.i18n.t('stream.audio')}</span>`;
        }
    }
};

/* ---------- 19. WebSocket ---------- */

const WS = {
    sock: null, retry: 1000,

    connect() {
        try { this.sock = new WebSocket('ws://' + window.location.host + '/ws'); }
        catch (e) { return; }
        this.sock.onmessage = (ev) => {
            let m;
            try { m = JSON.parse(ev.data); } catch (_) { return; }
            const t = window.i18n.t;
            switch (m.type) {
                case 'motion_started': toast(t('ws.motion_started', { score: String((m.data || {}).score ?? '') }), { type: 'info', duration: 2200 }); break;
                case 'motion_cleared': toast(t('ws.motion_cleared'), { duration: 1800 }); break;
                case 'recording_started':
                case 'recording_stopped': toast(t('ws.' + m.type), { type: 'success', duration: 2200 }); pollStatus(); break;
                case 'wifi_state_changed': toast(t('ws.wifi_state_changed', { state: t('wifi.state.' + ((m.data || {}).state || 'disconnected')) })); pollStatus(); break;
            }
        };
        this.sock.onclose = () => {
            setTimeout(() => this.connect(), this.retry);
            this.retry = Math.min(this.retry * 1.5, 15000);
        };
        this.sock.onopen = () => { this.retry = 1000; };
    }
};

/* ---------- 20. System actions ---------- */

async function doReboot() {
    const ok = await Modal.confirm({
        title: window.i18n.t('system.reboot'),
        message: window.i18n.t('system.confirm_reboot'),
        icon: 'refresh',
    });
    if (!ok) return;
    const btn = $('btn-reboot');
    await busy(btn, async () => {
        try { await api('/api/reboot', { method: 'POST' }); } catch (e) { /* 重启前断开属预期 */ }
        toast(window.i18n.t('toast.rebooting'), { duration: 6000 });
    });
}

async function doReset() {
    const ok = await Modal.confirm({
        title: window.i18n.t('system.factory_reset'),
        message: window.i18n.t('system.confirm_reset'),
        danger: true,
        okText: window.i18n.t('system.factory_reset'),
    });
    if (!ok) return;
    const btn = $('btn-reset');
    await busy(btn, async () => {
        try { await api('/api/reset', { method: 'POST' }); } catch (e) { /* 重启前断开属预期 */ }
        Auth.clear();
        toast(window.i18n.t('toast.rebooting'), { duration: 6000 });
    });
}

async function doOta(endpoint, file, btn) {
    if (!file) return;
    await busy(btn, async () => {
        try {
            await api(endpoint, { method: 'POST', body: file });
            toast(window.i18n.t('toast.upload_done'), { type: 'success', duration: 6000 });
        } catch (e) {
            if (e.status !== undefined && e.status !== null) {
                /* 服务器明确返回错误状态（含鉴权失败）— 走 api() 的鉴权重试后仍失败 */
                toast(window.i18n.t('toast.upload_failed', { msg: e.message }), { type: 'error', duration: 5000 });
            } else {
                /* 连接中断 = 烧写完成后设备重启断开，视为成功 */
                toast(window.i18n.t('toast.upload_done'), { type: 'success', duration: 6000 });
            }
        }
    });
}

/* ---------- 21. Stream ---------- */

/* ---------- 21. Stream（含死帧看门狗） ---------- */

const StreamWatch = {
    canvas: null,
    lastSig: null,
    frozenCount: 0,
    backoff: 0,          /* 重连退避：0 → 5s → 30s → 60s（封顶） */

    sample() {
        /* 把 <img> 当前帧缩到 24×18 取签名；连续 3 次不变判定死帧 */
        const img = $('stream-img');
        if (!img.complete || !img.naturalWidth) return;
        if (!this.canvas) {
            this.canvas = document.createElement('canvas');
            this.canvas.width = 24; this.canvas.height = 18;
        }
        const ctx = this.canvas.getContext('2d', { willReadFrequently: true });
        try {
            ctx.drawImage(img, 0, 0, 24, 18);
        } catch (e) { return; }
        const data = ctx.getImageData(0, 0, 24, 18).data;
        let sig = 0;
        for (let i = 0; i < data.length; i += 4) {
            sig = (sig * 31 + data[i] + data[i + 1] + data[i + 2]) | 0;
        }
        if (sig === this.lastSig) {
            this.frozenCount++;
        } else {
            this.frozenCount = 0;
            this.backoff = 0;
        }
        this.lastSig = sig;
        if (this.frozenCount >= 3) this.reconnect('frozen');
    },

    tick() {
        /* 服务端已无我们的连接（静默断开）→ 立即重连 */
        const sc = lastStatus && lastStatus.stream_clients;
        const stage = $('video-stage');
        if (stage.classList.contains('is-live') && sc === 0) {
            this.reconnect('no-client');
            return;
        }
        if (document.hidden) return;   /* 后台标签页节流，不采样 */
        this.sample();
    },

    reconnect(reason) {
        if (this.frozenCount === 0 && reason === 'frozen') return;
        this.frozenCount = 0;
        this.lastSig = null;
        const delay = this.backoff === 0 ? 0
            : this.backoff === 1 ? 5000
            : this.backoff === 2 ? 30000 : 60000;
        this.backoff = Math.min(this.backoff + 1, 3);
        setTimeout(() => {
            const img = $('stream-img');
            img.src = 'http://' + window.location.hostname + ':81/stream?watchdog=' + Date.now();
        }, delay);
    }
};

function initStream() {
    const img = $('stream-img');
    /* 流在 :81 跨源：不设 crossOrigin 时浏览器按 no-cors 加载，canvas 被污染，
     * StreamWatch 的 getImageData 每 3s 抛 SecurityError → 防死帧看门狗整体失效。
     * 服务端 :81 响应已带 Access-Control-Allow-Origin: *。 */
    img.crossOrigin = 'anonymous';
    const stage = $('video-stage');
    const hint = $('stage-hint');
    let delay = 1000;
    const MAX = 30000;
    let token = 0;

    const connect = () => {
        const my = ++token;
        img.src = 'http://' + window.location.hostname + ':81/stream' + (my > 1 ? '?' + Date.now() : '');
        /* MJPEG 是无限流：多数浏览器 load 事件只在断流时才触发，
         * 因此用"2.5s 无错误即视为已出图"的启发式隐藏骨架屏 */
        setTimeout(() => { if (my === token) stage.classList.add('is-live'); }, 2500);
    };

    img.addEventListener('load', () => {
        stage.classList.add('is-live');
        delay = 1000;
        /* 容器比例跟随流的自然比例（HD 16:9 / VGA 4:3 都满幅无黑边；
         * CSS 里的 4/3 只是首帧前的占位） */
        if (img.naturalWidth) stage.style.aspectRatio = img.naturalWidth + ' / ' + img.naturalHeight;
    });
    img.addEventListener('error', () => {
        token++;  /* 使旧的超时定时器失效，避免骨架屏被误隐藏 */
        stage.classList.remove('is-live');
        hint.textContent = window.i18n.t('stream.reconnecting');
        if (img._reconnecting) return;
        img._reconnecting = true;
        setTimeout(() => { img._reconnecting = false; connect(); }, delay);
        delay = Math.min(delay * 1.5, MAX);
    });

    connect();

    /* 看门狗：3s 采样一次；每 90s 无条件换新流（浏览器渲染器停滞型死帧的自愈兜底） */
    setInterval(() => { try { StreamWatch.tick(); } catch (_) { /* 防止看门狗异常刷屏 */ } }, 3000);
    setInterval(() => {
        if (!document.hidden && stage.classList.contains('is-live')) {
            img.src = 'http://' + window.location.hostname + ':81/stream?keepalive=' + Date.now();
        }
    }, 90000);
}

/* ---------- 23. Unload cleanup（尽快释放设备侧套接字） ---------- */

window.addEventListener('beforeunload', () => {
    if (WS.sock) { try { WS.sock.close(); } catch (_) {} }
    AudioMon.stop();
});

/* ---------- 22. Init ---------- */

document.addEventListener('DOMContentLoaded', async () => {
    Theme.init();
    initTabs();
    initStream();

    /* 状态轮询无条件先行启动：boot 链后续任一步抛异常（或首载撞上设备掉线窗口）
     * 都保证统计条/在线指示灯活起来并自愈 —— 2026-09-03 ai 弱射频板实测
     * 骨架屏死锁（chips 几分钟不填充，需手动刷新）由此根治 */
    pollStatus();
    setInterval(pollStatus, 1000);

    /* header actions */
    $('btn-theme').addEventListener('click', () => Theme.toggle());
    $('btn-lang').addEventListener('click', () => {
        const lang = window.i18n.getLang() === 'en' ? 'zh' : 'en';
        window.i18n.setLang(lang);
        $('lang-label').textContent = lang === 'zh' ? '中' : 'EN';
        pollStatus();
        if (Caps.sd) loadFiles();
    });
    $('lang-label').textContent = window.i18n.getLang() === 'zh' ? '中' : 'EN';
    $('btn-lock').addEventListener('click', async () => {
        if (Auth._open) return;
        try {
            const a = await api('/api/auth', {}, { noAuthSheet: true });
            await Auth.ensure(a.password_set ? 'unlock' : 'set');
        } catch (e) {
            /* 401 = 会话里存的旧密码不对（或设备已设密）→ 解锁模式 */
            await Auth.ensure('unlock');
        }
    });

    /* capabilities：失败自动重试（最多 10 次×5s，覆盖掉线窗口），成功后应用能力
     * 并拉取板级数据。原来的单次 try/catch 失败后所有能力驱动面板永久缺失 */
    const loadCapsRetry = async (n) => {
        try { Object.assign(Caps, await api('/api/capabilities', {}, { noAuthSheet: true })); }
        catch (e) {
            if (n > 0) return setTimeout(() => loadCapsRetry(n - 1), 5000);
            console.error('capabilities:', e);
            return;
        }
        applyCapabilities();
        fillStreamUrls();
        loadConfig();
        loadCamera();
        if (Caps.sd) loadFiles();
        if (Caps.ai) { pollAI(); setInterval(pollAI, 500); }
    };
    loadCapsRetry(10);

    /* toggles */
    initToggle('cam-hmirror', () => saveCamera());
    initToggle('cam-vflip', () => saveCamera());
    initToggle('ai-face', () => saveAI());
    initToggle('ai-motion', () => saveAI());
    initToggle('ai-qr', () => saveAI());
    initToggle('onvif-enable', () => saveStreaming());

    /* sliders */
    ['cam-quality', 'cam-brightness', 'cam-contrast', 'cam-saturation', 'cam-sharpness', 'led-brightness']
        .forEach(id => {
            const input = $(id);
            if (!input) return;
            paintSlider(input);
            input.addEventListener('input', () => {
                $(id + '-val').textContent = input.value;
                paintSlider(input);
                if (id.startsWith('cam-')) saveCamera();
                if (id === 'led-brightness') saveLED();
            });
        });

    $('cam-framesize').addEventListener('change', () => { updateAIVGACoupling(); saveCamera(); });
    $('cam-day-night').querySelectorAll('button').forEach(b =>
        b.addEventListener('click', () => {
            $('cam-day-night').querySelectorAll('button').forEach(x => x.classList.remove('active'));
            b.classList.add('active');
            saveCamera();
        }));

    /* buttons */
    $('btn-snapshot').addEventListener('click', () => window.open('/api/capture', '_blank'));
    $('btn-audio').addEventListener('click', () => AudioMon.toggle());
    $('btn-record').addEventListener('click', toggleRecord);
    $('btn-fullscreen').addEventListener('click', () => {
        const stage = $('video-stage');
        if (document.fullscreenElement) document.exitFullscreen();
        else stage.requestFullscreen && stage.requestFullscreen();
    });
    $('btn-files-refresh').addEventListener('click', () => busy($('btn-files-refresh'), loadFiles));
    initFilesUI();
    $('btn-wifi-scan').addEventListener('click', wifiScan);
    $('btn-network-save').addEventListener('click', saveNetwork);
    $('btn-change-pw').addEventListener('click', openPasswordModal);
    $('btn-streaming-save').addEventListener('click', saveStreaming);
    $('btn-reboot').addEventListener('click', doReboot);
    $('btn-reset').addEventListener('click', doReset);
    $('btn-ota-fw').addEventListener('click', () =>
        doOta('/api/ota/upload', $('ota-fw-file').files[0], $('btn-ota-fw')));
    $('btn-ota-spiffs').addEventListener('click', () =>
        doOta('/api/ota/spiffs', $('ota-spiffs-file').files[0], $('btn-ota-spiffs')));
});

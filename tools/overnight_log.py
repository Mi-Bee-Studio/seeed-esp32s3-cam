#!/usr/bin/env python3
"""整夜串口采集器 — 排查 seeed 周期性重启（2026-09-02）

用法: python3 overnight_log.py [串口设备]
输出:
  overnight_serial.log  — 全量串口（带时间戳）
  overnight_reboots.log — 只记关键事件：重启 banner / esp_restart 决策 / 错误行

停止: Ctrl-C 或 kill。重启检测基于 "rst:0x" bootloader banner。
"""
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else '/dev/ttyACM0'
BAUD = 115200

serial_log = open('overnight_serial.log', 'a', buffering=1)
event_log = open('overnight_reboots.log', 'a', buffering=1)

def stamp():
    return time.strftime('%Y-%m-%d %H:%M:%S')

event_log.write(f'\n===== collector started {stamp()} on {PORT} =====\n')

while True:
    try:
        ser = serial.Serial(PORT, BAUD, timeout=0.2)
        # CH340(ai-thinker) open 时默认断言 RTS 会把 EN 按在复位态 → 0 字节输入；
        # open 后立即释放（CH343/原生 USB 板上无副作用）
        ser.rts = False
        ser.dtr = False
        event_log.write(f'[{stamp()}] serial opened\n')
    except serial.SerialException as e:
        event_log.write(f'[{stamp()}] serial open failed: {e} — retry in 10s\n')
        time.sleep(10)
        continue

    tail = ''
    last_rx = time.time()
    try:
        while True:
            try:
                data = ser.read(4096)
            except Exception as e:
                # USB 重新枚举会抛 OSError/TermiosError（非 SerialException），
                # 2026-09-03 10:43 事故：采集器阻塞在死 fd 上错过重启原因
                raise serial.SerialException(f'read failed: {e!r}')
            if not data:
                # ESP32 最吵的周期日志（health 30s + onvif 30s）>90s 一条必到；
                # 静默 120s 视为 fd 已死（read 空转不抛错的挂死形态），强制重开
                if time.time() - last_rx > 120:
                    raise serial.SerialException('silent >120s — forcing reopen')
                continue
            last_rx = time.time()
            text = data.decode('utf-8', errors='replace')
            serial_log.write(f'[{stamp()}] {text}')

            # 按行分割（跨读取块保留半行）
            tail += text
            lines = tail.split('\n')
            tail = lines.pop()
            for line in lines:
                clean = line.strip()
                if not clean:
                    continue
                if ('rst:0x' in clean or 'boot:' in clean or
                        'reboot' in clean.lower() or 'esp_restart' in clean or
                        'probe failed' in clean.lower() or 'pausing' in clean.lower() or
                        'E (' in clean or 'panic' in clean.lower() or 'WDT' in clean):
                    event_log.write(f'[{stamp()}] {clean[:200]}\n')
    except serial.SerialException as e:
        event_log.write(f'[{stamp()}] serial lost: {e} — reopening\n')
        try:
            ser.close()
        except Exception:
            pass
        time.sleep(5)
    except KeyboardInterrupt:
        event_log.write(f'[{stamp()}] collector stopped\n')
        break

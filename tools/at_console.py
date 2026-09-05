#!/usr/bin/env python3
"""at_console.py — MiBee Cam 家族 AT 指令常驻注入器（PIT-003 感知）

用法:
    python3 at_console.py -p /dev/ttyUSB0 [-b 115200] [--wait-boot SECS]
                          [--cmds "AT","AT+GMR"] [--script file.txt] [--json]

纪律（PITFALLS PIT-003/PIT-026）:
  - CH340/CH343 板（ai-thinker ttyUSB*、luatos ttyACM1）**开串口即复位**：
    本工具默认单次常驻连接，开串口后等待启动横幅再发指令；请勿与
    overnight_log.py 等采集器同时占用同一端口。
  - seeed 原生 USB-JTAG（ttyACM0）无开复位，--wait-boot 可设 0。
  - 交互接受“开串口即重启”的现实（契约 at-command.md §1）。

脚本文件格式: 每行一条指令；# 开头为注释；空行忽略。
"""

import argparse
import json
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial 未安装: pip install pyserial")


BOOT_BANNER_KEYS = (b"MiBee", b"boot:0x", b"esp32", b"ESP32", b"entry")
READY_KEY = b"+READY: MiBee Cam AT console"


def open_port(port: str, baud: int) -> serial.Serial:
    """打开串口（CH340/CH343 开复位无法阻止——PIT-003；常驻单连接是我们的纪律）"""
    return serial.Serial(
        port=port,
        baudrate=baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.2,
    )


def wait_boot(ser: serial.Serial, wait_secs: float, detect: bool) -> None:
    """等待板子启动完成：主动检测横幅，兜底 wait_secs（块读取版）"""
    if not detect:
        time.sleep(wait_secs)
        return

    def scan(seconds: float) -> bool:
        """读到 +READY 返回 True；顺带记录横幅"""
        nonlocal saw_banner
        deadline = time.time() + seconds
        while time.time() < deadline:
            chunk = safe_read(ser)
            if chunk:
                if any(k in chunk for k in BOOT_BANNER_KEYS):
                    saw_banner = True
                if READY_KEY in chunk:
                    return True
        return False

    saw_banner = False
    if scan(max(wait_secs, 1.0)):
        print("[boot] AT console ready", file=sys.stderr)
        time.sleep(0.2)
        return
    if saw_banner and scan(4.0):
        print("[boot] AT console ready (grace)", file=sys.stderr)
        return
    print("[boot] banner wait done (continuing)", file=sys.stderr)


def safe_read(ser: serial.Serial, size: int = 4096) -> bytes:
    """块读取；USB-JTAG CDC 会抛 'readiness but no data' 假异常，吞掉。
    注意不要用 readline()——它逐字节读，在此类 CDC 上每字节都可能触发
    假异常，一行永远凑不齐（seeed 实测）。"""
    try:
        return ser.read(size)
    except (serial.SerialException, OSError):
        time.sleep(0.05)
        return b""


def _split_lines(buf: bytes):
    """把累积缓冲切成完整行，返回 (lines, remainder)"""
    lines = buf.split(b"\n")
    remainder = lines.pop() if lines else b""
    return [l for l in lines], remainder


def read_for(ser: serial.Serial, seconds: float) -> list:
    """读满 seconds 秒，返回完整行列表（块读取 + 自行分行）"""
    end = time.time() + seconds
    acc = b""
    while time.time() < end:
        acc += safe_read(ser)
    lines, _ = _split_lines(acc)
    return lines


def drain(ser: serial.Serial, quiet_secs: float = 0.5) -> list:
    """读到静默 quiet_secs 为止，返回全部行"""
    lines = []
    remainder = b""
    last = time.time()
    while time.time() - last < quiet_secs:
        chunk = safe_read(ser)
        if chunk:
            remainder += chunk
            new_lines, remainder = _split_lines(remainder)
            lines += new_lines
            last = time.time()
    return lines


def send_cmd(ser: serial.Serial, cmd: str, settle: float = 0.15,
             total_wait: float = 10.0) -> dict:
    # 注意：不用 reset_input_buffer()——USB-JTAG CDC（seeed ttyACM0）实测
    # tcflush 后读写会假死；残留旧数据靠 drain 排掉。
    drain(ser, quiet_secs=0.15)
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    time.sleep(settle)
    # USB-JTAG CDC（seeed）应答可被共口 ESP_LOG 拖慢到秒级：
    # 连续轮询读满 total_wait 或提前见到 OK/ERROR 行
    deadline = time.time() + total_wait
    acc = b""
    text_lines = []
    while time.time() < deadline:
        chunk = safe_read(ser)
        if chunk:
            acc += chunk
        lines, acc_pending = _split_lines(acc)
        acc = acc_pending
        text_lines += [l.decode(errors="replace").rstrip("\r") for l in lines]
        if any(l == "OK" or l.startswith("ERROR") for l in text_lines[-3:]):
            # 收到终行：再短排一次补尾行
            text_lines += [l.decode(errors="replace").rstrip("\r")
                           for l in drain(ser, quiet_secs=0.3)]
            break
    # 过滤 ESP_LOG 噪声行（与控制台共口）：只保留 AT 框架行（OK/ERROR/+X:）
    at_lines = [l for l in text_lines if l == "OK" or l.startswith("ERROR") or l.startswith("+")]
    status = "ok" if any(l == "OK" for l in at_lines) else (
        "error" if any(l.startswith("ERROR") for l in at_lines) else "no-reply")
    return {"cmd": cmd, "status": status, "lines": at_lines, "raw": text_lines}


def main() -> int:
    ap = argparse.ArgumentParser(description="MiBee Cam 家族 AT 常驻注入器")
    ap.add_argument("-p", "--port", required=True, help="串口设备，如 /dev/ttyUSB0")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--wait-boot", type=float, default=12.0,
                    help="开串口后等待启动的秒数（CH340/CH343 开复位；seeed 可设 0）")
    ap.add_argument("--no-detect", action="store_true",
                    help="不做横幅检测，纯睡 wait-boot 秒")
    ap.add_argument("--cmds", default="",
                    help='分号分隔指令列表，如 "AT;AT+GMR;AT+STATUS"'
                         '（注意：带逗号参数的指令请用 --script，逗号在此不安全）')
    ap.add_argument("--script", default="", help="脚本文件（每行一条指令）")
    ap.add_argument("--json", action="store_true", help="结果输出为 JSON")
    ap.add_argument("--default-suite", action="store_true",
                    help="跑家族冒烟套件（不碰 AT+RESTORE 等破坏性指令）")
    args = ap.parse_args()

    cmds = []
    if args.cmds:
        cmds += [c.strip() for c in args.cmds.split(";") if c.strip()]
    if args.script:
        with open(args.script, encoding="utf-8") as f:
            cmds += [l.strip() for l in f if l.strip() and not l.strip().startswith("#")]
    if args.default_suite:
        cmds += ["AT", "AT+HELP", "AT+GMR", "AT+WIFI?", "AT+IP?",
                 "AT+CAMRES?", "AT+CAMQUAL?", "AT+STATUS", "AT+WIFISCAN"]
    if not cmds:
        ap.error("需要 --cmds / --script / --default-suite 之一")

    ser = open_port(args.port, args.baud)
    print(f"[open] {args.port} @ {args.baud}（CH340/CH343 板此刻已被复位——PIT-003）",
          file=sys.stderr)
    wait_boot(ser, args.wait_boot, detect=not args.no_detect)
    # USB-JTAG CDC（seeed）实测：开 port 后立刻发首条指令可能被设备端
    # 丢弃；先静默预读 ~3s（等效手动流程）让通道稳定。
    print("[pre-read] settling CDC channel 3s", file=sys.stderr)
    t0 = time.time()
    while time.time() - t0 < 3.0:
        safe_read(ser)

    results = []
    for cmd in cmds:
        r = send_cmd(ser, cmd)
        results.append(r)
        if args.json:
            continue
        print(f">>> {cmd}")
        for l in r["lines"]:
            print(f"    {l}")
        print(f"<<< [{r['status']}]")

    ser.close()

    if args.json:
        print(json.dumps(results, ensure_ascii=False, indent=1))

    bad = [r for r in results if r["status"] != "ok"]
    if bad:
        print(f"[result] {len(bad)}/{len(results)} 条未获 OK: "
              + ", ".join(r["cmd"] for r in bad), file=sys.stderr)
        return 1
    print(f"[result] 全部 {len(results)} 条指令 OK", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

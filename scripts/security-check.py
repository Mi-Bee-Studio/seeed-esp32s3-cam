#!/usr/bin/env python3
# =============================================================================
# MiBee Cam 固件仓泄密扫描（家族门禁，四仓字节一致 — PITFALLS PIT-027 防线）
#
# 用法：
#   python3 scripts/security-check.py            # 扫描全仓库（CI 与本地自查）
#   python3 scripts/security-check.py --files a.c b.md   # 只扫指定文件
#
# 规则（命中任意一条即失败，退出码 1）：
#   A. 真实内网网段（192.168.62/63.x，脱敏规范认定的真实家庭网段）
#   B. 已知基础设施地址（生产服务器等）
#   C. 已知泄露标识（真实 SSID、真实设备名等，按审计结果持续补充）
#   D. 密钥/token 格式（GitHub/AWS/OpenAI/Slack/Google/GitLab/npm/DO、JWT、PEM、SSH 公私钥）
#   E. 非文档用途的公网 IPv4（文档保留段除外：192.0.2/24、198.51.100/24、203.0.113/24）
#   F. 凭据赋值白名单：password/密码/secret/token 取值只允许
#      ① 官方公开默认密码 ② 厂商默认值 ③ 明确占位符。
#      日期形取值（YYYY-MM-DD，本地机密默认密码的形态，PIT-027）一律视为可疑。
#      新增取值必须在 ALLOWED_PASSWORDS 登记并在 PR 说明理由，由守门人审查。
#
# 误报处理：确属合理的值 → 加入对应白名单（提交在同一个 PR 里，守门人可见）。
# 纪律：本文件与 .github/workflows/security-check.yml 在四个 cam 仓字节一致
#      （tools/family_check.sh 校验）；改一处必须同步四仓。
# =============================================================================

import argparse
import ipaddress
import os
import re
import subprocess
import sys

# ---- A. 真实内网网段（脱敏规范：真实家庭网段必须写成 192.168.1.x） ----------
FORBIDDEN_SUBNETS = [
    ipaddress.ip_network("192.0.2.0/24"),
    ipaddress.ip_network("192.0.2.0/24"),
]

# ---- B. 已知基础设施地址（生产/内网服务，禁止出现在公开仓库） --------------
FORBIDDEN_HOSTS = {
    "203.0.113.138",   # 官网生产服务器（阿里云）
}

# ---- C. 已知泄露标识（真实 SSID / 网名 / 设备名，按审计持续补充） ----------
FORBIDDEN_STRINGS = [
    "MyHomeWiFi",   # 真实家庭 WiFi SSID（2026-09-05 审计）
    "MiBeeAP1",     # 真实家庭 WiFi SSID（含 MiBeeAP2 备用 AP，2026-09-06 审计）
]

# ---- D. 密钥 / token 格式 ---------------------------------------------------
SECRET_PATTERNS = [
    (r"ghp_[A-Za-z0-9]{20,}", "GitHub PAT (ghp_)"),
    (r"github_pat_[A-Za-z0-9_]{20,}", "GitHub fine-grained PAT"),
    (r"gho_[A-Za-z0-9]{20,}", "GitHub OAuth token"),
    (r"ghs_[A-Za-z0-9]{20,}", "GitHub App token"),
    (r"AKIA[0-9A-Z]{16}", "AWS Access Key ID"),
    (r"sk-proj-[A-Za-z0-9_-]{20,}", "OpenAI API key"),
    (r"sk-[A-Za-z0-9]{32,}", "API secret (sk-)"),
    (r"xox[baprs]-[A-Za-z0-9-]{10,}", "Slack token"),
    (r"AIza[0-9A-Za-z_-]{30,}", "Google API key"),
    (r"glpat-[A-Za-z0-9_-]{20,}", "GitLab PAT"),
    (r"npm_[A-Za-z0-9]{30,}", "npm token"),
    (r"dop_v1_[a-f0-9]{32}", "DigitalOcean token"),
    (r"eyJ[A-Za-z0-9_-]{15,}\.[A-Za-z0-9_-]{15,}\.[A-Za-z0-9_-]{10,}", "JWT"),
    (r"-----BEGIN [A-Z ]*PRIVATE KEY-----", "PEM private key"),
    (r"ssh-(rsa|ed25519|dss) AAAA[A-Za-z0-9+/]{50,}", "SSH key body"),
]

# ---- E. 公网 IP 白名单（文档保留段 / 常用公共 DNS 等） ----------------------
DOC_NETWORKS = [
    ipaddress.ip_network("192.0.2.0/24"),      # RFC 5737 TEST-NET-1
    ipaddress.ip_network("198.51.100.0/24"),   # RFC 5737 TEST-NET-2
    ipaddress.ip_network("203.0.113.0/24"),    # RFC 5737 TEST-NET-3
]
WELL_KNOWN_PUBLIC = {
    "1.1.1.1", "8.8.8.8", "8.8.4.4", "9.9.9.9",
    "114.114.114.114", "223.5.5.5", "223.6.6.6",
}

# ---- F. 凭据取值白名单 ------------------------------------------------------
# ① 官方公开默认密码（产品决策 2026-09-05，可写入文档，注明首配后必须修改）
OFFICIAL_DEFAULT_PASSWORDS = {
    "mibeecam2026",      # MiBeeCam 全家族（AP/Web/RTSP）
    "mibeestudio2026",   # MiBee Studio 服务端产品
    "mibeehome2026",     # MBHData 家庭平台
}
# ② 第三方厂商设备的公开默认值（摄像头接入指南等事实性文档）
VENDOR_DEFAULTS = {"admin", "root", "user", "123456", "12345", "admin123", "1234", "000000"}
# ③ 明确占位符 / 教学示例值（不含任何真实凭据语义）
PLACEHOLDER_VALUES = {
    "password", "pass", "passwd", "newpassword", "oldpassword",
    "your-password", "your-pass", "your-strong-password", "your-secure-password",
    "initial-password", "initial-admin-password", "example-password",
    "changeme", "change-me", "placeholder", "sample", "samplepass", "testpass",
    "streampass", "onvifpass", "securepass", "lowpass", "secure123",
    "secure-password-123", "streampassword", "davpassword", "onvif123",
    "stream-key", "secret", "secretkey", "token", "apikey",
    "password123", "camera123", "yourpassword", "your_passToken_here",
    "mqtt_password", "xiaomi_password", "new_xiaomi_password",
    "secure_password_123", "complex_password", "secure_admin_password",
    "super-secret-key", "secret123", "securepassword123", "videoSrc0", "enc0",
}
ALLOWED_PASSWORDS = OFFICIAL_DEFAULT_PASSWORDS | VENDOR_DEFAULTS | PLACEHOLDER_VALUES

# 凭据赋值：key 为单个 token（含 password/passwd/密码/secret/token/apikey 等），后接 : 或 =
ASSIGN_RE = re.compile(
    r"(?i)(?:[a-z0-9_\-]*(?:password|passwd|apikey|api_key|secret|token)[a-z0-9_\-]*|密码)[ \t]*[:=][ \t]*[\"'`]?([^\s\"'`,#（）();]+)"
)
MASKED_VALUES = {"", "****", "******", "xxx", "xxxx", "<password>", "<密码>", "<token>", "<secret>", "${password}"}

# 占位符标记（取值包含即视为教学示例，不报警）
PLACEHOLDER_MARKERS = ("your", "xxx", "here", "example", "placeholder", "changeme", "change-me", "...", "!secret", "sample", "dummy", "fake", "openssl")

# 日期形取值（YYYY-MM-DD）：本地机密默认密码的形态（PIT-027），一律可疑
DATE_VALUE_RE = re.compile(r"\d{4}-\d{2}-\d{2}")

IPV4_RE = re.compile(r"(?<![\d.])(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})(?![\d.])")

# 固件仓扫描范围：文档 + 脚本 + C 源码 + 文本配置（Kconfig.projbuild 按文件名单独放行）
SCAN_EXTS = {".md", ".json", ".yaml", ".yml", ".toml", ".sh", ".py", ".c", ".h", ".txt", ".csv"}
SCAN_FILENAMES = {"Kconfig.projbuild", "Kconfig"}
SCAN_SKIP_DIRS = {".git", "node_modules", "scripts", "build", "managed_components", "components"}


def is_private_or_reserved(ip: str) -> bool:
    addr = ipaddress.ip_address(ip)
    return (
        addr.is_private or addr.is_loopback or addr.is_link_local
        or addr.is_multicast or addr.is_reserved or addr.is_unspecified
    )


def in_doc_network(ip: str) -> bool:
    addr = ipaddress.ip_address(ip)
    return any(addr in net for net in DOC_NETWORKS)


def check_password_value(value: str) -> bool:
    """返回 True 表示可疑（不在白名单）。"""
    raw = value.strip()
    v = raw.strip("\"'`").strip("(){}<>;").lstrip("!")
    if not v or v in MASKED_VALUES:
        return False
    if raw.startswith("<") and raw.endswith(">"):
        return False   # <占位符>
    if "(" in raw or raw.startswith("$"):
        return False   # $(cmd) / 代码调用
    if v in ALLOWED_PASSWORDS:
        return False
    if DATE_VALUE_RE.fullmatch(v):
        return True    # 日期形密码（PIT-027 形态），必须在白名单层面禁止
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", v):
        return False   # 代码标识符 / Kconfig 宏 / 环境变量引用（CONFIG_DEFAULT_AP_PASS 等），非字面量
    low = v.lower()
    if any(m in low for m in PLACEHOLDER_MARKERS):
        return False
    if v.isalpha():
        return False   # 纯字母散词（正文），真实凭据示例几乎必含数字/符号
    if re.fullmatch(r"\d{1,4}[a-z]{0,2}", low):
        return False   # 24h / 32m / 60 等时长或短数字
    if re.fullmatch(r"[0-9≥≤~\-\s]+", v):
        return False   # 8-64 / ≥32 等区间描述
    if any("\u4e00" <= ch <= "\u9fff" for ch in v):
        return False   # 中文正文（密码：必填…）
    if v.isupper() and len(v) < 20:
        return False   # 环境变量名（如 password = WIFI_PASS）
    if "=" in v or len(v) <= 2:
        return False   # 残片
    if "." in v or "::" in v:
        return False   # 代码引用（state.save_preset / String::new）
    return True


def scan_file(path: str, disp: str = None) -> list:
    disp = disp or path
    issues = []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError as e:
        return [f"{disp}: [IO] {e}"]

    def out(no, msg):
        return f"{disp}:{no}: {msg}"

    for no, line in enumerate(lines, 1):
        # A/B. 禁止的 IP
        for m in IPV4_RE.finditer(line):
            ip = m.group(1)
            if any(ipaddress.ip_address(ip) in net for net in FORBIDDEN_SUBNETS):
                issues.append(out(no, f"[真实内网网段] {ip}"))
            elif ip in FORBIDDEN_HOSTS:
                issues.append(out(no, f"[基础设施地址] {ip}"))
            elif not is_private_or_reserved(ip) and not in_doc_network(ip) and ip not in WELL_KNOWN_PUBLIC:
                issues.append(out(no, f"[公网IP] {ip}（文档请使用 192.0.2.x/198.51.100.x/203.0.113.x 保留段）"))

        # C. 禁止的标识
        for s in FORBIDDEN_STRINGS:
            if s in line:
                issues.append(out(no, f"[泄露标识] {s}"))

        # D. 密钥格式
        for pat, name in SECRET_PATTERNS:
            if re.search(pat, line):
                issues.append(out(no, f"[密钥格式] {name}"))

        # F. 凭据赋值白名单
        for m in ASSIGN_RE.finditer(line):
            value = m.group(1)
            if check_password_value(value):
                issues.append(out(
                    no, f"[凭据取值] {value!r} 不在白名单"
                    f"（官方默认/厂商默认/占位符；新增需登记白名单并说明理由）"
                ))
    return issues


def iter_scan_files(root: str):
    # 优先枚举 git 跟踪文件：gitignored 的本地日志/缓存（.omo/、tools/*.log）不应产生噪音
    try:
        out = subprocess.run(
            ["git", "-C", root, "ls-files", "-z"],
            capture_output=True, check=True,
        ).stdout.decode("utf-8", "surrogateescape")
        tracked = [f for f in out.split("\0") if f]
    except Exception:
        tracked = None
    if tracked is not None:
        for fn in tracked:
            base = os.path.basename(fn)
            if fn == "scripts/security-check.py":
                continue   # 扫描器自身的白名单/规则文本不参与扫描
            if os.path.splitext(base)[1].lower() in SCAN_EXTS or base in SCAN_FILENAMES:
                yield os.path.join(root, fn)
        return
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in SCAN_SKIP_DIRS]
        for fn in filenames:
            if os.path.splitext(fn)[1].lower() in SCAN_EXTS or fn in SCAN_FILENAMES:
                yield os.path.join(dirpath, fn)


def main() -> int:
    ap = argparse.ArgumentParser(description="MiBee Cam 固件仓泄密扫描")
    ap.add_argument("--files", nargs="*", default=None, help="只扫描指定文件（默认全仓库）")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if args.files:
        targets = []
        for f in args.files:
            abspath = os.path.abspath(f)
            try:
                rel = os.path.relpath(abspath, root)
            except ValueError:
                rel = abspath  # 跨盘等无法取相对路径时直接显示原路径
            targets.append((abspath, rel))
    else:
        targets = [(p, os.path.relpath(p, root)) for p in iter_scan_files(root)]

    issues = []
    for path, rel in targets:
        issues.extend(scan_file(path, rel))

    if issues:
        print(f"❌ 泄密扫描未通过：{len(issues)} 处疑似问题\n")
        for i in issues:
            print("  " + i)
        print(
            "\n处理方式：确属泄密 → 立即脱敏；确属合理值 → 更新 scripts/security-check.py"
            " 白名单并在 PR 中说明（守门人审查）。"
        )
        return 1
    print(f"✅ 泄密扫描通过（{len(targets)} 个文件）")
    return 0


if __name__ == "__main__":
    sys.exit(main())

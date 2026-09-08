#!/usr/bin/env bash
# 安装本仓 git 钩子（泄密闸等）：git config core.hooksPath .githooks
# 新克隆后跑一次即可；远端闸（分支保护必需检查 + 全分支 CI）不依赖此步骤。
set -e
cd "$(dirname "$0")/.."
git config core.hooksPath .githooks
echo "✓ hooks 已安装（core.hooksPath=.githooks）：pre-commit 将对 staged 文件跑泄密扫描"
echo "  提示：导出 MIBEE_SECURITY_DENYLIST（条目 subnet:<CIDR>|host:<IP>|str:<子串>，"
echo "  取值见根工作区 AGENTS.md 机密清单）可启用真实网段/SSID 检查"

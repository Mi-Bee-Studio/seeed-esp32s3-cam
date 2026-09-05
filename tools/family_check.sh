#!/usr/bin/env bash
# =============================================================================
# MiBee Cam 家族共享文件 md5 审计（四仓字节一致纪律）
#
# 用法：在任一家族仓内执行 tools/family_check.sh
#       （家族根默认 = 本仓的上级目录；可用 MIBEE_FAMILY_ROOT=... 覆盖）
# 校验对象：SPA 五件套 / AT 共享核心三件 / 契约三文档 / 家族工具 / 泄密扫描门禁。
# 任一分歧即退出码 1 —— 改一处必须同步四仓。
# =============================================================================
set -u

SELF=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$SELF/.." && pwd)
FAMILY_ROOT=${MIBEE_FAMILY_ROOT:-$(cd "$REPO/.." && pwd)}

# 家族仓识别标志：持有 docs/api-contract.md 的兄弟目录
REPOS=()
for d in "$FAMILY_ROOT"/*/; do
  d="${d%/}"
  [ -f "$d/docs/api-contract.md" ] && REPOS+=("$d")
done
if [ "${#REPOS[@]}" -lt 2 ]; then
  echo "✗ 只发现 ${#REPOS[@]} 个家族仓（$FAMILY_ROOT 下），预期 4 个；用 MIBEE_FAMILY_ROOT 指定家族根"
  exit 2
fi
echo "家族仓（${#REPOS[@]}）：$(for r in "${REPOS[@]}"; do basename "$r"; done | paste -sd' ')"

SHARED_FILES=(
  main/web_ui/app.js
  main/web_ui/i18n.js
  main/web_ui/index.html
  main/web_ui/style.css
  main/web_ui/favicon.svg
  main/at_command.c
  main/at_command.h
  main/at_port.h
  docs/api-contract.md
  docs/config-contract.md
  docs/at-command.md
  tools/overnight_log.py
  tools/at_console.py
  tools/mjpeg_probe.py
  tools/family_check.sh
  scripts/security-check.py
  .github/workflows/security-check.yml
)

fail=0
for f in "${SHARED_FILES[@]}"; do
  ref=""; ref_repo=""; bad=""
  for r in "${REPOS[@]}"; do
    p="$r/$f"
    if [ ! -f "$p" ]; then
      bad="$bad 缺失:$(basename "$r")"
      continue
    fi
    h=$(md5sum "$p" | cut -d' ' -f1)
    if [ -z "$ref" ]; then
      ref="$h"; ref_repo=$(basename "$r")
    elif [ "$h" != "$ref" ]; then
      bad="$bad 分歧:$(basename "$r")=${h:0:8}"
    fi
  done
  if [ -n "$bad" ]; then
    printf '✗ %-44s%s\n' "$f" "$bad"
    fail=1
  else
    printf '✓ %-44s %s（基准 %s）\n' "$f" "${ref:0:8}" "$ref_repo"
  fi
done

if [ "$fail" -eq 0 ]; then
  echo "✅ 家族共享文件全部字节一致"
else
  echo "❌ 存在分歧 —— SPA/AT 核心/契约/工具/CI 是同一套 md5 纪律，修一处必须同步四仓"
fi
exit $fail

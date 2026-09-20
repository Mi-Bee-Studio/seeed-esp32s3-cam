#!/bin/sh
# Style gate: every tracked .c/.h must be clang-format clean.
# Uses the same pinned version as CI when clang-format is on PATH.
set -e
cd "$(git rev-parse --show-toplevel)"
if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format not found (pip install clang-format==22.1.8)" >&2
    exit 2
fi
files=$(git ls-files '*.c' '*.h')
# shellcheck disable=SC2086
bad=$(clang-format --dry-run -Werror $files 2>&1 | \
      grep -oE '^[^:]+' | sort -u || true)
if [ -n "$bad" ]; then
    echo "style gate FAILED — run tools/format.sh and re-commit:"
    echo "$bad"
    exit 1
fi
count=$(echo "$files" | wc -l)
echo "style gate passed ($count files clang-format clean)"

#!/bin/sh
# Apply the project code style (see .clang-format) to every tracked C file.
set -e
cd "$(git rev-parse --show-toplevel)"
git ls-files '*.c' '*.h' | xargs clang-format -i
echo "formatted: $(git ls-files '*.c' '*.h' | wc -l) files"

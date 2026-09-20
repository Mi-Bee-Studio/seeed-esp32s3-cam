#!/bin/sh
# Install this repo's git hooks (once per clone).
set -e
cd "$(git rev-parse --show-toplevel)"
git config core.hooksPath .githooks
echo "hooks -> $(git config core.hooksPath)"

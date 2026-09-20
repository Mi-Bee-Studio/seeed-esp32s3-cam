#!/bin/sh
# Junk/secret files must never be tracked.
set -e
cd "$(git rev-parse --show-toplevel)"
status=0
for pat in '*.o' '*.pyc' '__pycache__' 'sdkconfig' 'dependencies.lock' '.DS_Store' '*~'; do
    if git ls-files --error-unmatch "$pat" >/dev/null 2>&1; then
        echo "tracked junk file matching $pat"; status=1
    fi
done
if git ls-files | grep -qE '(^|/)(build|managed_components)/'; then
    echo "tracked build directory"; status=1
fi
[ "$status" -eq 0 ] && echo "repo hygiene OK"
exit "$status"

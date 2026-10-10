#!/bin/sh
# Line-coverage gate for onvif-c (core/ + esp_idf/, the shipped sources).
#
#   tests/coverage.sh              gate at 80% (default)
#   tests/coverage.sh 85           gate at 85%
#   COVERAGE_MIN=85 tests/coverage.sh
#
# Uses gcc --coverage; runs BOTH host suites (core goldens + port layer
# via the ESP-IDF stubs) so gcda data accumulates across every test path.
set -e
cd "$(dirname "$0")"
MIN="${1:-${COVERAGE_MIN:-80}}"
CC="${CC:-gcc}"
OUT=build-cov
rm -rf "$OUT" *.gcov
mkdir -p "$OUT"

CORE_SRCS="../core/onvif_xml.c ../core/onvif_time.c ../core/onvif_wsse.c ../core/onvif_probe.c ../core/onvif_events_ring.c"
PORT_SRCS="../esp_idf/onvif_c_service.c ../esp_idf/onvif_c_events.c ../esp_idf/onvif_c_discovery.c"
CFLAGS="-std=c99 -Wall -Wextra -Werror -D_DEFAULT_SOURCE -g -O0 --coverage -Ihost_stubs"

for s in $CORE_SRCS $PORT_SRCS; do
    $CC $CFLAGS -c "$s" -o "$OUT/$(basename "${s%.c}").o"
done
for s in test_core.c test_port_main.c test_service.c test_events.c \
         test_discovery.c host_stubs/onvif_fake.c; do
    $CC $CFLAGS -c "$s" -o "$OUT/$(basename "${s%.c}").o"
done

$CC --coverage -pthread -Wl,--wrap=time -o "$OUT/run_port" \
    "$OUT/test_port_main.o" "$OUT/test_service.o" "$OUT/test_events.o" \
    "$OUT/test_discovery.o" "$OUT/onvif_fake.o" \
    "$OUT/onvif_xml.o" "$OUT/onvif_time.o" "$OUT/onvif_wsse.o" "$OUT/onvif_probe.o" "$OUT/onvif_events_ring.o" \
    "$OUT/onvif_c_service.o" "$OUT/onvif_c_events.o" "$OUT/onvif_c_discovery.o"
$CC --coverage -o "$OUT/run_core" \
    "$OUT/test_core.o" \
    "$OUT/onvif_xml.o" "$OUT/onvif_time.o" "$OUT/onvif_wsse.o" "$OUT/onvif_probe.o" "$OUT/onvif_events_ring.o"

"$OUT/run_core"
"$OUT/run_port"

echo ""
echo "line coverage (gate: ${MIN}%)"
for o in onvif_xml onvif_time onvif_probe onvif_events_ring \
         onvif_wsse onvif_c_service onvif_c_events onvif_c_discovery; do
    gcov -b "$OUT/$o.o" >/dev/null
done

status=0
total_hit=0
total_lines=0
for f in ../core/onvif_xml.c ../core/onvif_time.c ../core/onvif_wsse.c ../core/onvif_probe.c ../core/onvif_events_ring.c \
         ../esp_idf/onvif_c_service.c ../esp_idf/onvif_c_events.c \
         ../esp_idf/onvif_c_discovery.c; do
    g=$(basename "$f").gcov
    # gcov line format: "<count>:<lineno>:<source>" — ##### = never run,
    # "-" = non-executable. Everything else counts as a covered line.
    stats=$(awk -F: '
        /^[ ]*(-|#####|[0-9]+[.,]?[a-zA-Z]*)[ ]*:[ ]*[0-9]+[ ]*:/ {
            c = $1; gsub(/[ ]/, "", c);
            if (c == "-") next;
            total++;
            if (c != "#####") hit++;
        }
        END { printf "%d %d", hit + 0, total + 0 }' "$g")
    hit=${stats% *}
    lines=${stats#* }
    pct=0
    [ "$lines" -gt 0 ] && pct=$((hit * 100 / lines))
    total_hit=$((total_hit + hit))
    total_lines=$((total_lines + lines))
    printf '  %-28s %3d%%  (%d/%d)\n' "$(basename "$f")" "$pct" "$hit" "$lines"
done

pct=0
[ "$total_lines" -gt 0 ] && pct=$((total_hit * 100 / total_lines))
printf '  %-28s %3d%%  (%d/%d)\n' "TOTAL" "$pct" "$total_hit" "$total_lines"

if [ "$pct" -lt "$MIN" ]; then
    echo "FAIL: total line coverage ${pct}% < ${MIN}% — add tests (see *.gcov for gaps)"
    status=1
else
    echo "coverage gate passed (${pct}% >= ${MIN}%)"
fi
rm -f *.gcov
exit $status

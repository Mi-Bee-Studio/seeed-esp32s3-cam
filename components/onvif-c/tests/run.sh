#!/bin/sh
# onvif-c host tests — zero dependencies beyond a C compiler + pthreads.
#
#   tests/run.sh                              build & run both suites
#   CC=clang tests/run.sh                     alternate compiler
#   EXTRA_CFLAGS="-fsanitize=address,undefined" tests/run.sh   opt-in sanitizers
#
# Suite 1: core goldens (pure C, no stubs) — pins the byte-stability contract.
# Suite 2: port layer — the real esp_idf/ handlers driven through the
#          ESP-IDF stubs in tests/host_stubs/ (fake httpd, fake clock via
#          -Wl,--wrap=time, pthread tasks, virtual UDP network).
set -e
cd "$(dirname "$0")"
CC="${CC:-cc}"
CFLAGS="-std=c99 -Wall -Wextra -Werror -O1 -g -D_DEFAULT_SOURCE ${EXTRA_CFLAGS:-}"

CORE_SRCS="../core/onvif_xml.c ../core/onvif_time.c ../core/onvif_wsse.c ../core/onvif_probe.c ../core/onvif_events_ring.c"
PORT_SRCS="../esp_idf/onvif_c_service.c ../esp_idf/onvif_c_events.c ../esp_idf/onvif_c_discovery.c"

echo "== core goldens =="
$CC $CFLAGS -o /tmp/onvif_c_core test_core.c $CORE_SRCS
/tmp/onvif_c_core

echo "== port layer (ESP-IDF stubs) =="
$CC $CFLAGS -Ihost_stubs -pthread -Wl,--wrap=time \
    -o /tmp/onvif_c_port \
    test_port_main.c test_service.c test_events.c test_discovery.c \
    host_stubs/onvif_fake.c \
    $CORE_SRCS $PORT_SRCS
/tmp/onvif_c_port

echo "host tests OK"

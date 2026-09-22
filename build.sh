#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2 -Wall -Wextra -pthread}"

echo "[1/3] userfaultfd probe"
$CC $CFLAGS userfaultfd_probe.c -o userfaultfd_probe

echo "[2/3] StateRAM engine"
$CC $CFLAGS -fPIC -shared stateram9.c -o libstateram9.so

echo "[3/3] Real 4->12 benchmark"
$CC $CFLAGS stateram12_real_bench.c stateram9.c -o stateram12_real_bench

echo "Build complete."

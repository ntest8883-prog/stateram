#!/usr/bin/env bash
set -euo pipefail
CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2 -Wall -Wextra -pthread}"

echo "[1/2] userfaultfd probe"
$CC $CFLAGS userfaultfd_probe.c -o userfaultfd_probe

echo "[2/2] StateRAM-13 final benchmark"
$CC $CFLAGS stateram13_final_bench.c stateram9.c -o stateram13_final_bench

echo "Build complete."

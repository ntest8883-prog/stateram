#!/usr/bin/env bash
set -euo pipefail

cat <<'EOF'
This is the large-RAM reference run.

Run it only on a Linux machine with comfortably more than 12 GiB available
RAM (16 GiB+ recommended), because RAW mode pre-touches the full 12 GiB
logical state before measurement.
EOF

./stateram12_real_bench \
  --mode raw \
  --logical-gb 12 \
  --objects 24 \
  --segment-mb 8 \
  --profile normal \
  --steps 2000 \
  --csv raw_12g_reference.csv

./analyze.py raw_12g_reference.csv

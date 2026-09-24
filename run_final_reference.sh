#!/usr/bin/env bash
set -euo pipefail

./stateram13_final_bench \
  --mode raw \
  --logical-gb 12 \
  --objects 24 \
  --segment-kb 256 \
  --raw-target-mb 3000 \
  --shadow-depth 0 \
  --profile normal \
  --steps 2000 \
  --csv stateram13_raw_12g_reference.csv

python3 analyze.py stateram13_raw_12g_reference.csv

#!/usr/bin/env bash
set -euo pipefail

sudo ./run_in_cgroup.py --memory 4G --swap 0 -- \
  ./stateram13_final_bench \
    --mode stateram \
    --logical-gb 12 \
    --objects 24 \
    --segment-kb 256 \
    --raw-target-mb 3000 \
    --shadow-depth 32 \
    --profile normal \
    --steps 2000 \
    --csv stateram13_final_4to12.csv

python3 analyze.py stateram13_final_4to12.csv

#!/usr/bin/env bash
set -euo pipefail

# StateRAM real 4 GiB cap, 12 GiB logical state.
# Requires: cgroup v2, root for cgroup creation, and userfaultfd support.

./userfaultfd_probe
echo
echo "Starting StateRAM 4->12 candidate under a hard 4 GiB cgroup..."
sudo ./run_in_cgroup.py --memory 4G --swap 0 -- \
  ./stateram12_real_bench \
    --mode stateram \
    --logical-gb 12 \
    --objects 24 \
    --segment-mb 8 \
    --raw-target-mb 3000 \
    --shadow-depth 2 \
    --profile normal \
    --steps 2000 \
    --csv stateram_4g_12g.csv

./analyze.py stateram_4g_12g.csv

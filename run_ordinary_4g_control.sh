#!/usr/bin/env bash
set -euo pipefail

cat <<'EOF'
Optional control: ordinary RAW workload under 4 GiB.

This requires the host to have swap enabled. We do NOT set memory.swap.max=0,
so the cgroup may use the host's configured swap. If no swap exists, the run
may be OOM-killed; that is a host limitation, not a benchmark result.
EOF

sudo ./run_in_cgroup.py --memory 4G --swap max -- \
  ./stateram12_real_bench \
    --mode raw \
    --logical-gb 12 \
    --objects 24 \
    --segment-mb 8 \
    --profile normal \
    --steps 2000 \
    --csv ordinary_4g_12g.csv

./analyze.py ordinary_4g_12g.csv

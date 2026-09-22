#!/usr/bin/env bash
set -u
echo "=== kernel ==="
uname -a
echo
echo "=== memory ==="
free -h || true
echo
echo "=== swap ==="
swapon --show || true
echo
echo "=== cgroup v2 ==="
if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
  echo "cgroup v2: YES"
  cat /sys/fs/cgroup/cgroup.controllers
else
  echo "cgroup v2: NO"
fi
echo
echo "=== storage ==="
lsblk -o NAME,TYPE,SIZE,ROTA,MODEL,MOUNTPOINTS 2>/dev/null || true

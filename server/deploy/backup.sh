#!/bin/sh
set -eu
OUT="${1:-$HOME/baby-monitor-backups}"
mkdir -p "$OUT"
STAMP="$(date +%Y%m%d_%H%M%S)"
docker run --rm \
  -v baby_monitor_server_baby_monitor_data:/source:ro \
  -v "$OUT:/backup" \
  alpine tar czf "/backup/baby_monitor_${STAMP}.tgz" -C /source .
echo "$OUT/baby_monitor_${STAMP}.tgz"

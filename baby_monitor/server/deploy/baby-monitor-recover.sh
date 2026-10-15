#!/bin/sh
set -eu

SERVER_DIR="${BABY_MONITOR_SERVER_DIR:-/home/zyx/baby_monitor/server}"
cd "$SERVER_DIR"

compose() {
    docker compose -f compose.yaml -f compose.pi.yaml "$@"
}

healthy() {
    container_id="$(compose ps -q app 2>/dev/null || true)"
    [ -n "$container_id" ] || return 1
    [ "$(docker inspect --format '{{if .State.Health}}{{.State.Health.Status}}{{else}}{{.State.Status}}{{end}}' "$container_id" 2>/dev/null)" = "healthy" ] || return 1
    curl -fsS --max-time 5 http://127.0.0.1:8088/health >/dev/null
}

if healthy; then
    exit 0
fi

sleep 5
if healthy; then
    exit 0
fi

compose up -d --no-build --force-recreate app

attempt=0
while [ "$attempt" -lt 12 ]; do
    if healthy; then
        exit 0
    fi
    attempt=$((attempt + 1))
    sleep 5
done

echo "Baby monitor recovery failed: app did not become healthy." >&2
exit 1

#!/bin/sh
set -eu

HOST_NAME="${HOST_NAME:-100.81.192.84}"
USER_NAME="${USER_NAME:-zyx}"
REMOTE_DIR="${REMOTE_DIR:-/home/zyx/baby_monitor/server}"
PUBLIC_URL="${PUBLIC_URL:-https://renesas.zyxserver.online}"
CHECK_ONLY=0

if [ "${1:-}" = "--check-only" ]; then
    CHECK_ONLY=1
fi

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
SERVER_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

for required in Dockerfile compose.yaml compose.pi.yaml requirements.txt app/main.py; do
    test -f "$SERVER_DIR/$required" || {
        echo "Server deployment input is missing: $required" >&2
        exit 1
    }
done
for command in tar scp ssh curl; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "Required command is not available: $command" >&2
        exit 1
    }
done

if [ "$CHECK_ONLY" -eq 1 ]; then
    echo "Deployment preflight OK. No archive was uploaded and no remote service was changed."
    exit 0
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT HUP INT TERM
ARCHIVE="$WORK_DIR/baby-monitor-server.tgz"
CHECKSUM="$ARCHIVE.sha256"

tar -czf "$ARCHIVE" --exclude=.venv --exclude=data --exclude=__pycache__ \
    --exclude=.pytest_cache --exclude='*.pyc' -C "$SERVER_DIR" .
if command -v sha256sum >/dev/null 2>&1; then
    HASH="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
else
    HASH="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
fi
printf '%s  baby-monitor-server.tgz\n' "$HASH" > "$CHECKSUM"

scp "$ARCHIVE" "$CHECKSUM" "$USER_NAME@$HOST_NAME:/tmp/"
ssh "$USER_NAME@$HOST_NAME" "set -e; cd /tmp; sha256sum -c baby-monitor-server.tgz.sha256; mkdir -p '$REMOTE_DIR'; cd '$REMOTE_DIR'; test -f .env && cp .env /tmp/baby-monitor-server.env || true; tar xzf /tmp/baby-monitor-server.tgz; test -f /tmp/baby-monitor-server.env && mv /tmp/baby-monitor-server.env .env; test -f .env; docker compose -f compose.yaml -f compose.pi.yaml up -d --build --remove-orphans --wait --wait-timeout 120 app; docker compose -f compose.yaml -f compose.pi.yaml ps; curl -fsS --max-time 10 http://127.0.0.1:8088/health >/dev/null"

attempt=1
while [ "$attempt" -le 12 ]; do
    if curl -fsS --max-time 10 "${PUBLIC_URL%/}/health" >/dev/null; then
        echo "Deployment verified on the Pi and at ${PUBLIC_URL%/}/health"
        exit 0
    fi
    sleep 5
    attempt=$((attempt + 1))
done

echo "Public server health check did not become ready" >&2
exit 1

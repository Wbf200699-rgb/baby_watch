#!/bin/sh
set -eu

REPO_DIR="${REPO_DIR:-$HOME/baby_monitor}"
BRANCH="${BRANCH:-main}"
BABY_MONITOR_BIND_IP="${BABY_MONITOR_BIND_IP:-0.0.0.0}"

if ! command -v docker >/dev/null 2>&1; then
    curl -fsSL https://get.docker.com | sh
    sudo usermod -aG docker "$USER"
    echo "Docker installed. Log out and back in, then run this script again."
    exit 0
fi

if [ ! -d "$REPO_DIR/.git" ]; then
    git clone https://github.com/suchen2333/baby_monitor.git "$REPO_DIR"
fi

cd "$REPO_DIR"
git fetch origin "$BRANCH"
git checkout "$BRANCH"
git pull --ff-only origin "$BRANCH"
cd server

if [ ! -f .env ]; then
    cp deploy/.env.example .env
    echo "Created server/.env. Set secrets before starting."
    exit 1
fi

export BABY_MONITOR_BIND_IP

compose() {
    docker compose -f compose.yaml -f compose.pi.yaml "$@"
}

compose build --pull
compose up -d --remove-orphans --wait --wait-timeout 60 app
compose ps

container_id="$(compose ps -q app)"
published_port="$(docker port "$container_id" 8088/tcp)"
case "$published_port" in
    *"${BABY_MONITOR_BIND_IP}:8088"*) ;;
    *)
        echo "Expected ${BABY_MONITOR_BIND_IP}:8088 to publish the app, got: ${published_port:-none}"
        exit 1
        ;;
esac

if ! curl -fsS --max-time 10 "http://127.0.0.1:8088/health" >/dev/null; then
    echo "Baby monitor health check failed on 127.0.0.1:8088."
    exit 1
fi

echo "Baby monitor is healthy on 127.0.0.1:8088."

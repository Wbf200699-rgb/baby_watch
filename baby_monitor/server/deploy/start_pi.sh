#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
: "${BABY_MONITOR_BIND_IP:=0.0.0.0}"
export BABY_MONITOR_BIND_IP

if [ ! -f .env ]; then
    cp deploy/.env.example .env
    echo "Edit server/.env before starting."
    exit 1
fi

docker compose -f compose.yaml -f compose.pi.yaml up -d --build app
docker compose -f compose.yaml -f compose.pi.yaml ps

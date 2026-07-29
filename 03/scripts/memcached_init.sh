#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="${1:-$BASE_DIR/../config.txt}"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Configuration file not found at $CONFIG_FILE"
    exit 1
fi

MEMCACHED_HOST=$(grep "^MEMCACHED_HOST=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
MEMCACHED_PORT=$(grep "^MEMCACHED_PORT=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')

if ! systemctl is-active --quiet memcached; then
    echo "ERROR: Memcached is not running."
    echo "Run once: sudo systemctl enable --now memcached"
    exit 1
fi

printf "flush_all\r\n" | nc -q 1 "$MEMCACHED_HOST" "$MEMCACHED_PORT" >/dev/null
echo "[Memcached] Cache flushed successfully."
#!/bin/bash
set -e

# Script: memcached_init.sh
# Description: Starts and flushes the local Memcached instance.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="${1:-$BASE_DIR/../config.txt}"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Configuration file not found at $CONFIG_FILE"
    exit 1
fi

MEMCACHED_HOST=$(grep "^MEMCACHED_HOST=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
MEMCACHED_PORT=$(grep "^MEMCACHED_PORT=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')

if [ -z "$MEMCACHED_HOST" ] || [ -z "$MEMCACHED_PORT" ]; then
    echo "ERROR: MEMCACHED_HOST or MEMCACHED_PORT is missing."
    exit 1
fi

if ! command -v memcached >/dev/null 2>&1; then
    echo "ERROR: Memcached is not installed."
    echo "Install it with: sudo apt install -y memcached libmemcached-dev netcat-openbsd"
    exit 1
fi

echo "[Memcached] Starting service at $MEMCACHED_HOST:$MEMCACHED_PORT..."
sudo systemctl start memcached

sleep 1

if ! echo -e "version\r" | nc -q 1 "$MEMCACHED_HOST" "$MEMCACHED_PORT" | grep -q VERSION; then
    echo "ERROR: Cannot connect to Memcached at $MEMCACHED_HOST:$MEMCACHED_PORT"
    exit 1
fi

echo -e "flush_all\r" | nc -q 1 "$MEMCACHED_HOST" "$MEMCACHED_PORT" >/dev/null
echo "[Memcached] Cache flushed successfully."
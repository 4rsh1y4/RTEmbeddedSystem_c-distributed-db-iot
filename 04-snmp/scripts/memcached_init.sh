#!/bin/bash
set -euo pipefail

MEMCACHED_PORT=${1:-11211}

echo "[Memcached] Initializing Memcached service on localhost:$MEMCACHED_PORT..."

# بررسی نصب بودن پیش‌نیازها
if ! command -v memcached >/dev/null 2>&1; then
    echo "Error: memcached is not installed. Run: sudo apt install -y memcached libmemcached-dev"
    exit 1
fi

sudo systemctl start memcached || true

# پاک‌سازی کامل کش برای شروع یک تست تمیز
if command -v nc >/dev/null 2>&1; then
    echo -e "flush_all\r" | nc -q 1 127.0.0.1 "$MEMCACHED_PORT" >/dev/null 2>&1 || true
    echo "[Memcached] Cache flushed successfully."
else
    echo "[Memcached] Warning: netcat (nc) not installed. Restarting service to flush..."
    sudo systemctl restart memcached
fi

echo "[Memcached] Active and ready!"
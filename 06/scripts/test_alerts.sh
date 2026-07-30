#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_DIR="$(cd "$BASE_DIR/.." && pwd)"
CONFIG_FILE="${CONFIG_FILE:-$PROJECT_DIR/config.txt}"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Configuration file not found at $CONFIG_FILE"
    exit 1
fi

ALERT_DB=$(
    grep '^ALERT_DB=' "$CONFIG_FILE" |
    cut -d '=' -f2- |
    tr -d '\r'
)

if [[ "$ALERT_DB" = /* ]]; then
    DB_PATH="$ALERT_DB"
else
    DB_PATH="$PROJECT_DIR/$ALERT_DB"
fi

if [ ! -x "$BASE_DIR/daemon/alert_daemon" ]; then
    echo "ERROR: Alert daemon is not compiled."
    echo "Run ./06/scripts/build_and_run.sh first."
    exit 1
fi

cd "$PROJECT_DIR"

echo "====================================================="
echo "          Distributed IoT Alert Test"
echo "====================================================="

echo
echo "[1/2] Running one immediate sensor check..."

"$BASE_DIR/daemon/alert_daemon" \
    "$CONFIG_FILE" \
    --once

echo
echo "[2/2] Alerts stored in SQLite:"
echo

sqlite3 \
    -header \
    -column \
    "$DB_PATH" \
    "SELECT
        id,
        sensor_id,
        sensor_name,
        alert_type,
        sensor_value,
        created_at,
        status
     FROM alerts
     ORDER BY id DESC;"

echo
echo "====================================================="
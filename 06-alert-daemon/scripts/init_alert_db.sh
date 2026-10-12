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

if [ -z "$ALERT_DB" ]; then
    echo "ERROR: ALERT_DB is missing from $CONFIG_FILE"
    exit 1
fi

if [[ "$ALERT_DB" = /* ]]; then
    DB_PATH="$ALERT_DB"
else
    DB_PATH="$PROJECT_DIR/$ALERT_DB"
fi

mkdir -p "$(dirname "$DB_PATH")"

sqlite3 "$DB_PATH" <<'SQL'
CREATE TABLE IF NOT EXISTS alerts(
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sensor_id TEXT,
    sensor_name TEXT,
    alert_type TEXT,
    sensor_value TEXT,
    created_at TEXT,
    status TEXT
);
SQL

echo "[Alert DB] Ready at $DB_PATH"
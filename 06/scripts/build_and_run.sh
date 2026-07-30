#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PROJECT_DIR="$(cd "$BASE_DIR/.." && pwd)"
CONFIG_FILE="${CONFIG_FILE:-$PROJECT_DIR/config.txt}"

SERVICE_TEMPLATE="$BASE_DIR/service/iot-alert.service"
SERVICE_FILE="/etc/systemd/system/iot-alert.service"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Configuration file not found at $CONFIG_FILE"
    exit 1
fi

MASTER_IP=$(
    grep '^MASTER_IP=' "$CONFIG_FILE" |
    cut -d '=' -f2- |
    tr -d ' \r'
)

LOCAL_IPS=$(hostname -I)

if ! grep -qw "$MASTER_IP" <<< "$LOCAL_IPS"; then
    echo "ERROR: Alert daemon must be installed on the Master node."
    echo "Expected Master IP: $MASTER_IP"
    exit 1
fi

for command in g++ make sqlite3; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "ERROR: Missing dependency: $command"
        echo "Run once:"
        echo "sudo apt install -y build-essential sqlite3 libsqlite3-dev libcurl4-openssl-dev"
        exit 1
    fi
done

if [ ! -f "$SERVICE_TEMPLATE" ]; then
    echo "ERROR: Service template not found at $SERVICE_TEMPLATE"
    exit 1
fi

echo "=========================================="
echo "       IoT Alert Daemon Bootstrapper"
echo "=========================================="

echo -e "\n[1/3] Compiling Alert Daemon..."

cd "$BASE_DIR/daemon"

make clean >/dev/null 2>&1 || true
make

echo -e "\n[2/3] Initializing Alert Database..."

chmod +x "$SCRIPT_DIR/init_alert_db.sh"

CONFIG_FILE="$CONFIG_FILE" \
"$SCRIPT_DIR/init_alert_db.sh"

echo -e "\n[3/3] Installing systemd service..."

TMP_SERVICE=$(mktemp)

trap 'rm -f "$TMP_SERVICE"' EXIT

sed \
    -e "s|__PROJECT_DIR__|$PROJECT_DIR|g" \
    -e "s|__DAEMON_PATH__|$BASE_DIR/daemon/alert_daemon|g" \
    -e "s|__CONFIG_FILE__|$CONFIG_FILE|g" \
    "$SERVICE_TEMPLATE" > "$TMP_SERVICE"

sudo install \
    -m 644 \
    "$TMP_SERVICE" \
    "$SERVICE_FILE"

sudo systemctl daemon-reload
sudo systemctl enable --now iot-alert.service
sudo systemctl restart iot-alert.service

echo
systemctl \
    --no-pager \
    --full \
    status iot-alert.service |
sed -n '1,12p'
#!/bin/bash

# Script: test_requests.sh
# Author: Arshia Noroozi (402170562)
# Description: Automated test runner for IoT Distributed Database queries.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="$BASE_DIR/master/config.txt"

if [ -f "$CONFIG_FILE" ]; then
    MASTER_IP=$(grep -E "^MASTER_IP=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d '\r' | tr -d ' ')
    MASTER_PORT=$(grep -E "^MASTER_PORT=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d '\r' | tr -d ' ')
fi

MASTER_IP=${MASTER_IP:-"127.0.0.1"}
MASTER_PORT=${MASTER_PORT:-"8080"}
TARGET_URL="http://${MASTER_IP}:${MASTER_PORT}/api/sensor"

echo "=========================================="
echo " Running Automated Tests against Master   "
echo " Target: $TARGET_URL                      "
echo "=========================================="

run_test() {
    local title="$1"
    local sensor_id="$2"
    
    echo -e "\n------------------------------------------"
    echo "Test: $title"
    echo "Command: curl -s \"${TARGET_URL}?id=${sensor_id}\""
    echo "Response:"
    
    # گرفتن پاسخ و اطمینان از چاپ در یک خط جدید
    RESPONSE=$(curl -s "${TARGET_URL}?id=${sensor_id}")
    
    if [ -z "$RESPONSE" ]; then
        echo "[No response or empty body returned]"
    else
        echo "$RESPONSE"
    fi
    echo "------------------------------------------"
    sleep 1
}

run_test "1. Local Read (Master DB) - Sensor 101" "101"
run_test "2. Async Network Route - Sensor 204" "204"
run_test "3. Async Network Route - Sensor 304" "304"
run_test "4. Not Found Test (404) - Sensor 999" "999"

echo -e "\nAll tests completed!"
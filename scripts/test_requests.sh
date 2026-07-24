#!/bin/bash

# Script: test_requests.sh
# Author: Arshia Noroozi (402170562)
# Description: Automated test runner for IoT Distributed Database queries.

# خواندن IP و Port مستر از کانفیگ یا ورودی (پیش‌فرض localhost:8080)
CONFIG_FILE="../master/config.txt"

if [ -f "$CONFIG_FILE" ]; then
    echo "nigger"
    MASTER_IP=$(grep -E "^MASTER_IP=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d '\r')
    MASTER_PORT=$(grep -E "^MASTER_PORT=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d '\r')
fi

MASTER_IP=${MASTER_IP:-"192.168.54.136"}
MASTER_PORT=${MASTER_PORT:-"8080"}
BASE_URL="http://${MASTER_IP}:${MASTER_PORT}/api/sensor"

echo "=========================================="
echo " Running Automated Tests against Master   "
echo " Target: $BASE_URL                        "
echo "=========================================="

# تابع کمکی برای ارسال تست
run_test() {
    local title="$1"
    local sensor_id="$2"
    
    echo -e "\n------------------------------------------"
    echo "Test: $title"
    echo "Command: curl -s \"$BASE_URL?id=$sensor_id\""
    echo "Response:"
    curl -s "$BASE_URL?id=$sensor_id"
    echo "------------------------------------------"
    sleep 1
}

# اجرای ۴ سناریوی اصلی
run_test "1. Local Read (Master DB) - Sensor 101" "101"
run_test "2. Async Network Route - Sensor 204" "204"
run_test "3. Async Network Route - Sensor 304" "304"
run_test "4. Not Found Test (404) - Sensor 999" "999"

echo -e "\nAll tests completed!"
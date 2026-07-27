#!/bin/bash
set -e

# Script: test_requests.sh
# Author: Arshia Noroozi (402170562)
# Description: Automated test runner for IoT Distributed Database queries.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

CONFIG_FILE="${CONFIG_FILE:-$BASE_DIR/../config.txt}"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Configuration file not found at $CONFIG_FILE"
    exit 1
fi

MASTER_IP=$(grep "^MASTER_IP=" "$CONFIG_FILE" | cut -d '=' -f2)
MASTER_PORT=$(grep "^MASTER_PORT=" "$CONFIG_FILE" | cut -d '=' -f2)

SLAVE1_IP=$(grep "^SLAVE1_IP=" "$CONFIG_FILE" | cut -d '=' -f2)
SLAVE1_PORT=$(grep "^SLAVE1_PORT=" "$CONFIG_FILE" | cut -d '=' -f2)

MASTER_URL="http://$MASTER_IP:$MASTER_PORT/api/sensor"
SLAVE1_URL="http://$SLAVE1_IP:$SLAVE1_PORT/api/sensor"

run_test() {
    TEST_NAME="$1"
    URL="$2"
    EXPECTED_STATUS="$3"

    RESPONSE_FILE=$(mktemp)

    ACTUAL_STATUS=$(curl -s -o "$RESPONSE_FILE" -w "%{http_code}" "$URL")

    echo "------------------------------------------"
    echo "Test: $TEST_NAME"
    echo "Expected status: $EXPECTED_STATUS"
    echo "Actual status:   $ACTUAL_STATUS"
    echo "Response:"
    cat "$RESPONSE_FILE"
    echo

    rm -f "$RESPONSE_FILE"

    if [ "$ACTUAL_STATUS" = "$EXPECTED_STATUS" ]; then
        echo "Result: PASS"
    else
        echo "Result: FAIL"
        return 1
    fi
}

echo "=========================================="
echo "         Part 01 API Tests"
echo "=========================================="

run_test "Sensor stored in Master" "$MASTER_URL?type=temperature&id=101" "200"

run_test "Sensor routed to Slave 1" "$MASTER_URL?type=co2&id=204" "200"

run_test "Sensor routed to Slave 2" "$MASTER_URL?type=smoke&id=304" "200"

run_test "Correct ID with wrong sensor type" "$MASTER_URL?type=humidity&id=101" "404"

run_test "Unknown sensor" "$MASTER_URL?type=temperature&id=999" "404"

run_test "Missing sensor type" "$MASTER_URL?id=101" "400"

run_test "Direct operator request to Slave" "$SLAVE1_URL?type=co2&id=204" "403"

echo "=========================================="
echo "Tests completed."
echo "=========================================="
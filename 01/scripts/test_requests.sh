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

MASTER_CSV="$BASE_DIR/data/master_sensors.csv"
SLAVE1_CSV="$BASE_DIR/data/slave1_sensors.csv"
SLAVE2_CSV="$BASE_DIR/data/slave2_sensors.csv"

for file in "$MASTER_CSV" "$SLAVE1_CSV" "$SLAVE2_CSV"; do
    if [ ! -f "$file" ]; then
        echo "ERROR: CSV file not found at $file"
        exit 1
    fi
done

# Read the first sensor ID and type from each CSV file
IFS=, read -r MASTER_ID MASTER_TYPE _ < <(sed -n '2{s/\r$//;p;}' "$MASTER_CSV")
IFS=, read -r SLAVE1_ID SLAVE1_TYPE _ < <(sed -n '2{s/\r$//;p;}' "$SLAVE1_CSV")
IFS=, read -r SLAVE2_ID SLAVE2_TYPE _ < <(sed -n '2{s/\r$//;p;}' "$SLAVE2_CSV")

# Find a sensor type different from the selected Master sensor type
WRONG_TYPE=$(awk -F',' -v type="$MASTER_TYPE" 'FNR>1 {gsub(/\r/,"",$2); if ($2 != type) {print $2; exit}}' "$MASTER_CSV" "$SLAVE1_CSV" "$SLAVE2_CSV")
[ -z "$WRONG_TYPE" ] && WRONG_TYPE="invalid_type"

# Generate a numeric ID that does not exist in any CSV file
UNKNOWN_ID=$(date +%s)
while grep -h -v '^sensor_id,' "$MASTER_CSV" "$SLAVE1_CSV" "$SLAVE2_CSV" | cut -d',' -f1 | grep -qx "$UNKNOWN_ID"; do
    UNKNOWN_ID=$((UNKNOWN_ID + 1))
done

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
echo "Master sensor : type=$MASTER_TYPE, id=$MASTER_ID"
echo "Slave 1 sensor: type=$SLAVE1_TYPE, id=$SLAVE1_ID"
echo "Slave 2 sensor: type=$SLAVE2_TYPE, id=$SLAVE2_ID"
echo "=========================================="

run_test "Sensor stored in Master" "$MASTER_URL?type=$MASTER_TYPE&id=$MASTER_ID" "200"
run_test "Sensor routed to Slave 1" "$MASTER_URL?type=$SLAVE1_TYPE&id=$SLAVE1_ID" "200"
run_test "Sensor routed to Slave 2" "$MASTER_URL?type=$SLAVE2_TYPE&id=$SLAVE2_ID" "200"
run_test "Correct ID with wrong sensor type" "$MASTER_URL?type=$WRONG_TYPE&id=$MASTER_ID" "404"
run_test "Unknown sensor" "$MASTER_URL?type=$MASTER_TYPE&id=$UNKNOWN_ID" "404"
run_test "Missing sensor type" "$MASTER_URL?id=$MASTER_ID" "400"
run_test "Direct operator request to Slave" "$SLAVE1_URL?type=$SLAVE1_TYPE&id=$SLAVE1_ID" "403"

echo "=========================================="
echo "All tests passed."
echo "=========================================="
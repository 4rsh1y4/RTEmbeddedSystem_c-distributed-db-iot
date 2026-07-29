#!/bin/bash
set -e

# Script: test_snmp.sh
# Author: Arshia Noroozi (402170562)
# Description: Reads and verifies all distributed sensors through SNMP.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="${CONFIG_FILE:-$BASE_DIR/../config.txt}"

get_config() { grep "^$1=" "$CONFIG_FILE" | cut -d '=' -f2- | tr -d '\r'; }

if [ ! -f "$CONFIG_FILE" ]; then
    echo "[!] Error: Config file '$CONFIG_FILE' not found."
    exit 1
fi

SNMP_HOST=$(get_config SNMP_HOST | tr -d ' ')
SNMP_PORT=$(get_config SNMP_PORT | tr -d ' ')
SNMP_COMMUNITY=$(get_config SNMP_COMMUNITY | tr -d ' ')
SNMP_BASE_OID=$(get_config SNMP_BASE_OID | tr -d ' ')

if [ -z "$SNMP_HOST" ] || [ -z "$SNMP_PORT" ] || [ -z "$SNMP_COMMUNITY" ] || [ -z "$SNMP_BASE_OID" ]; then
    echo "[!] Error: SNMP configuration is incomplete in '$CONFIG_FILE'."
    exit 1
fi

if ! command -v snmpwalk >/dev/null 2>&1; then
    echo "[!] Error: snmpwalk is not installed."
    echo "Run once: sudo apt install -y snmp"
    exit 1
fi

if ! systemctl is-active --quiet snmpd; then
    echo "[!] Error: snmpd service is not running."
    exit 1
fi

CSV_FILES=(
    "$BASE_DIR/data/master_sensors.csv"
    "$BASE_DIR/data/slave1_sensors.csv"
    "$BASE_DIR/data/slave2_sensors.csv"
)

for file in "${CSV_FILES[@]}"; do
    if [ ! -f "$file" ]; then
        echo "[!] Error: CSV file '$file' not found."
        exit 1
    fi
done

SENSOR_COUNT=$(awk -F',' 'FNR>1 {
    gsub(/\r/,"",$1)
    if ($1~/^[0-9]+$/) seen[$1]=1
} END {
    for (id in seen) count++
    print count+0
}' "${CSV_FILES[@]}")

EXPECTED_OIDS=$((SENSOR_COUNT * 3))
TMP_FILE=$(mktemp)
trap 'rm -f "$TMP_FILE"' EXIT

echo "====================================================="
echo "        Reading All Sensors via SNMP"
echo "====================================================="
echo "[*] Agent: $SNMP_HOST:$SNMP_PORT"
echo "[*] Community: $SNMP_COMMUNITY"
echo "[*] Enterprise OID: $SNMP_BASE_OID"
echo "[*] Sensors: $SENSOR_COUNT | Expected OIDs: $EXPECTED_OIDS"
echo "====================================================="
echo

if ! snmpwalk -v2c -c "$SNMP_COMMUNITY" -On -t 3 -r 1 "udp:$SNMP_HOST:$SNMP_PORT" "$SNMP_BASE_OID" | tee "$TMP_FILE"; then
    echo
    echo "[!] SNMP walk failed."
    exit 1
fi

ACTUAL_OIDS=$(grep -cF "$SNMP_BASE_OID." "$TMP_FILE" || true)

echo
echo "====================================================="
echo "[*] Expected OIDs : $EXPECTED_OIDS"
echo "[*] Received OIDs : $ACTUAL_OIDS"

if [ "$ACTUAL_OIDS" -eq "$EXPECTED_OIDS" ]; then
    echo "[+] SNMP test passed. All sensor attributes were received."
else
    echo "[!] SNMP test incomplete. Some sensor attributes are missing."
    exit 1
fi

echo "====================================================="
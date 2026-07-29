#!/bin/bash
set -e

# Script: benchmark_read.sh
# Author: Arshia Noroozi (402170562)
# Description: Compares SQLite cache misses with Memcached cache hits.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="${CONFIG_FILE:-$BASE_DIR/../config.txt}"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "[!] Error: Config file '$CONFIG_FILE' not found!"
    exit 1
fi

MASTER_IP=$(grep "^MASTER_IP=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
MASTER_PORT=$(grep "^MASTER_PORT=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
SLAVE1_IP=$(grep "^SLAVE1_IP=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
SLAVE1_PORT=$(grep "^SLAVE1_PORT=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
SLAVE2_IP=$(grep "^SLAVE2_IP=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
SLAVE2_PORT=$(grep "^SLAVE2_PORT=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
MASTER_SLAVE_TOKEN=$(grep "^MASTER_SLAVE_TOKEN=" "$CONFIG_FILE" | cut -d '=' -f2- | tr -d '\r')

if [ -z "$MASTER_IP" ] || [ -z "$MASTER_PORT" ] || [ -z "$SLAVE1_IP" ] || [ -z "$SLAVE1_PORT" ] || [ -z "$SLAVE2_IP" ] || [ -z "$SLAVE2_PORT" ] || [ -z "$MASTER_SLAVE_TOKEN" ]; then
    echo "[!] Error: Required network configuration is missing in '$CONFIG_FILE'"
    exit 1
fi

MASTER_URL="http://${MASTER_IP}:${MASTER_PORT}/api/sensor"
MASTER_CSV="$BASE_DIR/data/master_sensors.csv"
SLAVE1_CSV="$BASE_DIR/data/slave1_sensors.csv"
SLAVE2_CSV="$BASE_DIR/data/slave2_sensors.csv"

for file in "$MASTER_CSV" "$SLAVE1_CSV" "$SLAVE2_CSV"; do
    if [ ! -f "$file" ]; then
        echo "[!] Error: CSV file '$file' not found!"
        exit 1
    fi
done

# خواندن تمام سنسورهای یکتا از فایل‌های CSV با فرمت type|id
mapfile -t SENSORS < <(awk -F',' 'FNR>1 {gsub(/\r/,"",$1); gsub(/\r/,"",$2); key=$1"|"$2; if ($1!="" && $2!="" && !seen[key]++) print $2"|"$1}' "$MASTER_CSV" "$SLAVE1_CSV" "$SLAVE2_CSV")

if [ "${#SENSORS[@]}" -eq 0 ]; then
    echo "[!] Error: No sensors found in CSV files."
    exit 1
fi

echo "====================================================="
echo "        Memcached Benchmark Test (Read Speed)"
echo "====================================================="
echo "[*] Loaded Configuration from: $CONFIG_FILE"
echo "[*] Targeting API Gateway: $MASTER_URL"
echo "[*] Number of sensors: ${#SENSORS[@]}"
echo "====================================================="

echo "[*] Flushing Memcached on all nodes..."

curl -fsS -H "X-Master-Token: $MASTER_SLAVE_TOKEN" "http://$MASTER_IP:$MASTER_PORT/internal/cache/flush" >/dev/null
curl -fsS -H "X-Master-Token: $MASTER_SLAVE_TOKEN" "http://$SLAVE1_IP:$SLAVE1_PORT/internal/cache/flush" >/dev/null
curl -fsS -H "X-Master-Token: $MASTER_SLAVE_TOKEN" "http://$SLAVE2_IP:$SLAVE2_PORT/internal/cache/flush" >/dev/null

echo "[*] All caches are empty. Starting benchmark..."
echo

run_round() {
    local round_name="$1"
    local total_time=0
    local count=0

    echo "-----------------------------------------------------"
    echo " >>> $round_name <<<"
    echo "-----------------------------------------------------"

    for sensor in "${SENSORS[@]}"; do
        IFS='|' read -r type id <<< "$sensor"
        printf "Querying %-12s Sensor %-5s " "$type" "$id"

        RESPONSE=$(curl -sS --max-time 10 "$MASTER_URL?type=$type&id=$id")
        TIME_MS=$(echo "$RESPONSE" | grep -o '"response_time_ms":[ 0-9.]*' | cut -d':' -f2 | tr -d ' }')

        if [ -z "$TIME_MS" ]; then
            echo "[Failed] Data not found or server error."
        else
            printf -- "-> Response Time: %8s ms\n" "$TIME_MS"
            total_time=$(awk "BEGIN {print $total_time + $TIME_MS}")
            count=$((count + 1))
        fi
    done

    if [ "$count" -gt 0 ]; then
        avg_time=$(awk "BEGIN {printf \"%.3f\", $total_time / $count}")
        echo "-----------------------------------------------------"
        echo " => Average Time for $round_name: $avg_time ms"
        echo "-----------------------------------------------------"
    fi

    echo
}

run_round "Round 1 (Expected Cache Miss / SQLite DB)"
run_round "Round 2 (Expected Cache Hit / Memcached)"

echo "====================================================="
echo "                Benchmark Complete."
echo "====================================================="
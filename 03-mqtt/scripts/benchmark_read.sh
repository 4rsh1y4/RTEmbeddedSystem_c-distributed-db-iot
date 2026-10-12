#!/bin/bash

# Script: benchmark_read.sh
# Author: Arshia Noroozi (402170562)
# Description: Benchmarks sensor read times comparing Cache Misses (Round 1) and Cache Hits (Round 2).

CONFIG_FILE=${1:-"../master/config.txt"}

if [ ! -f "$CONFIG_FILE" ]; then
    echo "[!] Error: Config file '$CONFIG_FILE' not found!"
    exit 1
fi

MASTER_IP=$(grep -E '^MASTER_IP=' "$CONFIG_FILE" | cut -d '=' -f 2 | tr -d ' \r')
MASTER_PORT=$(grep -E '^MASTER_PORT=' "$CONFIG_FILE" | cut -d '=' -f 2 | tr -d ' \r')

if [ -z "$MASTER_IP" ] || [ -z "$MASTER_PORT" ]; then
    echo "[!] Error: MASTER_IP or MASTER_PORT is missing in '$CONFIG_FILE'"
    exit 1
fi

MASTER_URL="http://${MASTER_IP}:${MASTER_PORT}/api/sensor"

SENSOR_IDS=(101 102 201 204 301 304)

echo "====================================================="
echo "        Memcached Benchmark Test (Read Speed)        "
echo "====================================================="
echo "[*] Loaded Configuration from: $CONFIG_FILE"
echo "[*] Targeting API Gateway: $MASTER_URL"
echo "====================================================="

echo "[*] Flushing Memcached to ensure a fair test..."
if command -v nc >/dev/null 2>&1; then
    echo -e "flush_all\r" | nc -q 1 127.0.0.1 11211 >/dev/null 2>&1 || true
else
    sudo systemctl restart memcached
fi
echo "[*] Cache is empty. Starting benchmark..."
echo ""

run_round() {
    local round_name=$1
    echo "-----------------------------------------------------"
    echo " >>> $round_name <<<"
    echo "-----------------------------------------------------"
    
    local total_time=0
    local count=0

    for id in "${SENSOR_IDS[@]}"; do
        printf "Querying Sensor %-5s " "$id"
        
        RESPONSE=$(curl -s "$MASTER_URL?id=$id")
        
        TIME_MS=$(echo "$RESPONSE" | grep -o '"response_time_ms":[ 0-9.]*' | cut -d':' -f2 | tr -d ' }')
        
        if [ -z "$TIME_MS" ]; then
            echo "[Failed] Data not found or server error."
        else
            printf -- "-> Response Time: %6s ms\n" "$TIME_MS"
            total_time=$(echo "$total_time + $TIME_MS" | bc -l 2>/dev/null || awk "BEGIN {print $total_time + $TIME_MS}")
            count=$((count + 1))
        fi
    done

    if [ "$count" -gt 0 ]; then
        local avg_time=$(awk "BEGIN {printf \"%.3f\", $total_time / $count}")
        echo "-----------------------------------------------------"
        echo " => Average Time for $round_name: $avg_time ms"
        echo "-----------------------------------------------------"
    fi
    echo ""
}

run_round "Round 1 (Expected Cache Miss / SQLite DB)"

run_round "Round 2 (Expected Cache Hit / Memcached)"

echo "====================================================="
echo "                Benchmark Complete.                  "
echo "====================================================="
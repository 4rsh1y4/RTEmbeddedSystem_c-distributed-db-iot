#!/bin/bash

# Script: benchmark_mqtt.sh
# Author: Arshia Noroozi (402170562)
# Description: Benchmarks sensor read times over MQTT (QoS 1) comparing Cache Misses and Cache Hits.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="${CONFIG_FILE:-$BASE_DIR/../config.txt}"

if [ ! -f "$CONFIG_FILE" ]; then
    echo "[!] Error: Config file '$CONFIG_FILE' not found!"
    exit 1
fi

BROKER=$(grep "^MQTT_BROKER_HOST=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
BROKER_PORT=$(grep "^MQTT_BROKER_PORT=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
REQ_TOPIC=$(grep "^MQTT_REQUEST_TOPIC=" "$CONFIG_FILE" | cut -d '=' -f2- | tr -d '\r')
RES_TOPIC=$(grep "^MQTT_RESPONSE_TOPIC=" "$CONFIG_FILE" | cut -d '=' -f2- | tr -d '\r')
MQTT_QOS=$(grep "^MQTT_QOS=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')

MASTER_IP=$(grep "^MASTER_IP=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
MASTER_PORT=$(grep "^MASTER_PORT=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
SLAVE1_IP=$(grep "^SLAVE1_IP=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
SLAVE1_PORT=$(grep "^SLAVE1_PORT=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
SLAVE2_IP=$(grep "^SLAVE2_IP=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
SLAVE2_PORT=$(grep "^SLAVE2_PORT=" "$CONFIG_FILE" | cut -d '=' -f2 | tr -d ' \r')
MASTER_SLAVE_TOKEN=$(grep "^MASTER_SLAVE_TOKEN=" "$CONFIG_FILE" | cut -d '=' -f2- | tr -d '\r')

if [ -z "$BROKER" ] || [ -z "$BROKER_PORT" ] || [ -z "$REQ_TOPIC" ] || [ -z "$RES_TOPIC" ] || [ -z "$MQTT_QOS" ]; then
    echo "[!] Error: MQTT configuration is incomplete in '$CONFIG_FILE'"
    exit 1
fi

MASTER_CSV="$BASE_DIR/data/master_sensors.csv"
SLAVE1_CSV="$BASE_DIR/data/slave1_sensors.csv"
SLAVE2_CSV="$BASE_DIR/data/slave2_sensors.csv"

for file in "$MASTER_CSV" "$SLAVE1_CSV" "$SLAVE2_CSV"; do
    if [ ! -f "$file" ]; then
        echo "[!] Error: CSV file '$file' not found!"
        exit 1
    fi
done

# خروجی هر عضو آرایه به شکل type|id است.
mapfile -t SENSORS < <(awk -F',' 'FNR>1 {gsub(/\r/,"",$1); gsub(/\r/,"",$2); key=$1"|"$2; if ($1!="" && $2!="" && !seen[key]++) print $2"|"$1}' "$MASTER_CSV" "$SLAVE1_CSV" "$SLAVE2_CSV")

if [ "${#SENSORS[@]}" -eq 0 ]; then
    echo "[!] Error: No sensors found in CSV files."
    exit 1
fi

if ! command -v mosquitto_pub >/dev/null 2>&1 || ! command -v mosquitto_sub >/dev/null 2>&1; then
    echo "[!] Error: mosquitto_pub or mosquitto_sub is not installed."
    exit 1
fi

echo "====================================================="
echo "        MQTT Benchmark Test (QoS $MQTT_QOS)"
echo "====================================================="
echo "[*] Broker: $BROKER:$BROKER_PORT"
echo "[*] Publish Topic: $REQ_TOPIC"
echo "[*] Subscribe Topic: $RES_TOPIC"
echo "[*] Number of sensors: ${#SENSORS[@]}"
echo "====================================================="

echo "[*] Flushing Memcached on all nodes..."

curl -fsS -H "X-Master-Token: $MASTER_SLAVE_TOKEN" "http://$MASTER_IP:$MASTER_PORT/internal/cache/flush" >/dev/null || {
    echo "[!] Failed to flush Master cache."
    exit 1
}

curl -fsS -H "X-Master-Token: $MASTER_SLAVE_TOKEN" "http://$SLAVE1_IP:$SLAVE1_PORT/internal/cache/flush" >/dev/null || {
    echo "[!] Failed to flush Slave 1 cache."
    exit 1
}

curl -fsS -H "X-Master-Token: $MASTER_SLAVE_TOKEN" "http://$SLAVE2_IP:$SLAVE2_PORT/internal/cache/flush" >/dev/null || {
    echo "[!] Failed to flush Slave 2 cache."
    exit 1
}

echo "[*] All caches are empty. Starting MQTT benchmark..."
echo

run_mqtt_round() {
    local round_name="$1"
    local total_time=0
    local count=0

    echo "-----------------------------------------------------"
    echo " >>> $round_name <<<"
    echo "-----------------------------------------------------"

    for sensor in "${SENSORS[@]}"; do
        IFS='|' read -r type id <<< "$sensor"
        printf "MQTT Querying %-12s Sensor %-5s " "$type" "$id"

        TMP_FILE=$(mktemp)

        timeout 10 mosquitto_sub -h "$BROKER" -p "$BROKER_PORT" -t "$RES_TOPIC" -q "$MQTT_QOS" -C 1 > "$TMP_FILE" &
        SUB_PID=$!

        sleep 0.2

        mosquitto_pub -h "$BROKER" -p "$BROKER_PORT" -t "$REQ_TOPIC" -m "$type|$id" -q "$MQTT_QOS"

        wait "$SUB_PID" 2>/dev/null || true

        RESPONSE=$(cat "$TMP_FILE")
        rm -f "$TMP_FILE"

        TIME_MS=$(echo "$RESPONSE" | grep -o '"response_time_ms":[ 0-9.]*' | cut -d ':' -f2 | tr -d ' }')

        if [ -z "$TIME_MS" ]; then
            echo "[Failed] No valid response received via MQTT."
        else
            printf -- "-> Response Time: %8s ms\n" "$TIME_MS"
            total_time=$(awk "BEGIN {print $total_time + $TIME_MS}")
            count=$((count + 1))
        fi
    done

    if [ "$count" -gt 0 ]; then
        local avg_time
        avg_time=$(awk "BEGIN {printf \"%.3f\", $total_time / $count}")
        echo "-----------------------------------------------------"
        echo " => Average Time for $round_name: $avg_time ms"
        echo "-----------------------------------------------------"
    fi

    echo
}

run_mqtt_round "Round 1 (Expected Cache Miss / SQLite DB)"
run_mqtt_round "Round 2 (Expected Cache Hit / Memcached)"

echo "====================================================="
echo "                MQTT Benchmark Complete."
echo "====================================================="
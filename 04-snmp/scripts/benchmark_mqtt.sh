#!/bin/bash

# Script: benchmark_mqtt.sh
# Author: Arshia Noroozi (402170562)
# Description: Benchmarks sensor read times over MQTT (QoS 1) comparing Cache Misses and Cache Hits.

BROKER="localhost"
REQ_TOPIC="sensor/request"
RES_TOPIC="sensor/response"
SENSOR_IDS=(101 102 201 204 301 304)

echo "====================================================="
echo "        MQTT Benchmark Test (QoS 1)                  "
echo "====================================================="
echo "[*] Broker: $BROKER"
echo "[*] Publish Topic: $REQ_TOPIC | Subscribe Topic: $RES_TOPIC"
echo "====================================================="

# پاک‌سازی کش قبل از تست
echo "[*] Flushing Memcached to ensure a fair test..."
if command -v nc >/dev/null 2>&1; then
    echo -e "flush_all\r" | nc -q 1 127.0.0.1 11211 >/dev/null 2>&1 || true
else
    sudo systemctl restart memcached
fi
echo "[*] Cache is empty. Starting MQTT benchmark..."
echo ""

run_mqtt_round() {
    local round_name=$1
    echo "-----------------------------------------------------"
    echo " >>> $round_name <<<"
    echo "-----------------------------------------------------"
    
    local total_time=0
    local count=0

    for id in "${SENSOR_IDS[@]}"; do
        printf "MQTT Querying Sensor %-5s " "$id"
        
        # یک فایل موقت برای ذخیره خروجی سابسکرایبر
        TMP_FILE=$(mktemp)
        
        # ۱. گوش دادن به تاپیک پاسخ در پس‌زمینه
        # سوییچ C 1- باعث می‌شود بعد از دریافت دقیقاً ۱ پیام، به صورت خودکار بسته شود
        mosquitto_sub -h $BROKER -t "$RES_TOPIC" -q 1 -C 1 > "$TMP_FILE" &
        SUB_PID=$!
        
        # یک مکث بسیار کوتاه تا مطمئن شویم سابسکرایبر به بروکر متصل شده است
        sleep 0.2
        
        # ۲. ارسال درخواست به مستر با QoS 1
        mosquitto_pub -h $BROKER -t "$REQ_TOPIC" -m "$id" -q 1
        
        # ۳. صبر کردن تا سابسکرایبر جواب را دریافت کند و بسته شود
        wait $SUB_PID
        
        RESPONSE=$(cat "$TMP_FILE")
        rm -f "$TMP_FILE"
        
        # استخراج زمان از فایل JSON
        TIME_MS=$(echo "$RESPONSE" | grep -o '"response_time_ms":[ 0-9.]*' | cut -d':' -f2 | tr -d ' }')
        
        if [ -z "$TIME_MS" ]; then
            echo "[Failed] No response received via MQTT."
        else
            printf -- "-> Response Time: %6s ms\n" "$TIME_MS"
            total_time=$(echo "$total_time + $TIME_MS" | bc -l 2>/dev/null || awk "BEGIN {print $total_time + $TIME_MS}")
            count=$((count + 1))
        fi
    done

    # محاسبه میانگین
    if [ "$count" -gt 0 ]; then
        local avg_time=$(awk "BEGIN {printf \"%.3f\", $total_time / $count}")
        echo "-----------------------------------------------------"
        echo " => Average MQTT Time: $avg_time ms"
        echo "-----------------------------------------------------"
    fi
    echo ""
}

# اجرای دو دور بنچمارک
run_mqtt_round "Round 1 (Expected Cache Miss / SQLite DB)"
run_mqtt_round "Round 2 (Expected Cache Hit / Memcached)"

echo "====================================================="
echo "                MQTT Benchmark Complete.             "
echo "====================================================="
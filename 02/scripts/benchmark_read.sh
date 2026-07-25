#!/bin/bash

# Script: benchmark_read.sh
# Author: Arshia Noroozi (402170562)
# Description: Benchmarks sensor read times comparing Cache Misses (Round 1) and Cache Hits (Round 2).

MASTER_URL="http://127.0.0.1:8080/api/sensor"
# مجموعه‌ای از IDهای سنسورها (ترکیبی از Master و Slaveها برای تست جامع شبکه)
SENSOR_IDS=(101 102 201 204 301 304)

echo "====================================================="
echo "        Memcached Benchmark Test (Read Speed)        "
echo "====================================================="

# پاک‌سازی کش قبل از تست برای اطمینان از صحت دور اول
echo "[*] Flushing Memcached to ensure a fair test..."
if command -v nc >/dev/null 2>&1; then
    echo -e "flush_all\r" | nc -q 1 127.0.0.1 11211 >/dev/null 2>&1 || true
else
    sudo systemctl restart memcached
fi
echo "[*] Cache is empty. Starting benchmark..."
echo ""

# تابع اجرای یک دور تست
run_round() {
    local round_name=$1
    echo "-----------------------------------------------------"
    echo " >>> $round_name <<<"
    echo "-----------------------------------------------------"
    
    local total_time=0
    local count=0

    for id in "${SENSOR_IDS[@]}"; do
        printf "Querying Sensor %-5s " "$id"
        
        # ارسال درخواست و دریافت خروجی JSON
        RESPONSE=$(curl -s "$MASTER_URL?id=$id")
        
        # استخراج مقدار زمان از داخل رشته JSON
        TIME_MS=$(echo "$RESPONSE" | grep -o '"response_time_ms":[ 0-9.]*' | cut -d':' -f2 | tr -d ' }')
        
        if [ -z "$TIME_MS" ]; then
            echo "[Failed] Data not found or server error."
        else
            printf "-> Response Time: %6s ms\n" "$TIME_MS"
            total_time=$(echo "$total_time + $TIME_MS" | bc -l 2>/dev/null || awk "BEGIN {print $total_time + $TIME_MS}")
            count=$((count + 1))
        fi
    done

    # محاسبه میانگین سرعت این دور
    if [ "$count" -gt 0 ]; then
        local avg_time=$(awk "BEGIN {printf \"%.3f\", $total_time / $count}")
        echo "-----------------------------------------------------"
        echo " => Average Time for $round_name: $avg_time ms"
        echo "-----------------------------------------------------"
    fi
    echo ""
}

# اجرای دور اول (توقع می‌رود از SQLite خوانده شود - Cache Miss)
run_round "Round 1 (Expected Cache Miss / SQLite DB)"

# اجرای دور دوم (توقع می‌رود از Memcached خوانده شود - Cache Hit)
run_round "Round 2 (Expected Cache Hit / Memcached)"

echo "====================================================="
echo "                Benchmark Complete.                  "
echo "====================================================="
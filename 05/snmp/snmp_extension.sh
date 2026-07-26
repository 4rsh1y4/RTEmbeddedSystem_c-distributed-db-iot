#!/bin/bash
# Script: snmp_extension.sh

MODE=$1
REQ_OID=$2
BASE_OID=".1.3.6.1.4.1.402170562"

# این مقدار توسط اسکریپت راه‌انداز با آی‌پی واقعی سیستم جایگزین خواهد شد
MASTER_IP="REPLACE_ME_IP"

VALID_OIDS=(
    "$BASE_OID.101.1" "$BASE_OID.101.2" "$BASE_OID.101.3"
    "$BASE_OID.102.1" "$BASE_OID.102.2" "$BASE_OID.102.3"
    "$BASE_OID.201.1" "$BASE_OID.201.2" "$BASE_OID.201.3"
    "$BASE_OID.204.1" "$BASE_OID.204.2" "$BASE_OID.204.3"
    "$BASE_OID.301.1" "$BASE_OID.301.2" "$BASE_OID.301.3"
    "$BASE_OID.304.1" "$BASE_OID.304.2" "$BASE_OID.304.3"
)

TARGET_OID=""

if [ "$MODE" == "-g" ]; then
    for oid in "${VALID_OIDS[@]}"; do
        if [ "$oid" == "$REQ_OID" ]; then
            TARGET_OID=$REQ_OID
            break
        fi
    done
elif [ "$MODE" == "-n" ]; then
    TARGET_OID=$( (echo "$REQ_OID"; printf "%s\n" "${VALID_OIDS[@]}") | sort -V | awk -v req="$REQ_OID" '
        $0 == req { found=1; next }
        found == 1 && $0 != req { print $0; exit }
    ' )
    
    IS_VALID=0
    for oid in "${VALID_OIDS[@]}"; do
        if [ "$oid" == "$TARGET_OID" ]; then
            IS_VALID=1
            break
        fi
    done
    if [ "$IS_VALID" -eq 0 ]; then
        exit 0
    fi
fi

if [ -z "$TARGET_OID" ]; then
    exit 0
fi

SENSOR_ID=$(echo "$TARGET_OID" | awk -F'.' '{print $(NF-1)}')
ATTR_ID=$(echo "$TARGET_OID" | awk -F'.' '{print $NF}')

JSON_RES=$(curl -s --max-time 2 "http://${MASTER_IP}:8080/api/sensor?id=$SENSOR_ID")

if [ -z "$JSON_RES" ] || echo "$JSON_RES" | grep -q '"error"'; then
    exit 0
fi

NAME=$(echo "$JSON_RES" | grep -o '"sensor_name": *"[^"]*"' | cut -d'"' -f4)
UNIT=$(echo "$JSON_RES" | grep -o '"unit": *"[^"]*"' | cut -d'"' -f4)
VALUE=$(echo "$JSON_RES" | grep -o '"value": *[-0-9.]*' | cut -d':' -f2 | tr -d ' ')

echo "$TARGET_OID"
echo "string"

if [ "$ATTR_ID" == "1" ]; then
    echo "$NAME"
elif [ "$ATTR_ID" == "2" ]; then
    echo "IoT Sensor | Unit: $UNIT"
elif [ "$ATTR_ID" == "3" ]; then
    echo "$VALUE"
fi
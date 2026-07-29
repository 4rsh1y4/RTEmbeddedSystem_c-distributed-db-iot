#!/bin/bash

# Script: snmp_extension.sh
# Description: Net-SNMP pass extension for all distributed sensors.

MODE="$1"
REQ_OID="$2"
CONFIG_FILE="${SNMP_RUNTIME_CONFIG:-/opt/iot_project/snmp_runtime.conf}"

[ -f "$CONFIG_FILE" ] || exit 0

get_config() { grep "^$1=" "$CONFIG_FILE" | cut -d '=' -f2- | tr -d '\r'; }

MASTER_IP=$(get_config MASTER_IP | tr -d ' ')
MASTER_PORT=$(get_config MASTER_PORT | tr -d ' ')
BASE_OID=$(get_config SNMP_BASE_OID | tr -d ' ')
DATA_DIR=$(get_config SNMP_DATA_DIR)

CSV_FILES=(
    "$DATA_DIR/master_sensors.csv"
    "$DATA_DIR/slave1_sensors.csv"
    "$DATA_DIR/slave2_sensors.csv"
)

[ -n "$MASTER_IP" ] && [ -n "$MASTER_PORT" ] && [ -n "$BASE_OID" ] || exit 0
[[ "$BASE_OID" == .* ]] || BASE_OID=".$BASE_OID"
[[ "$REQ_OID" == .* ]] || REQ_OID=".$REQ_OID"

for file in "${CSV_FILES[@]}"; do
    [ -f "$file" ] || exit 0
done

mapfile -t SENSOR_IDS < <(
    awk -F',' 'FNR>1 {
        gsub(/\r/,"",$1)
        if ($1~/^[0-9]+$/ && !seen[$1]++) print $1
    }' "${CSV_FILES[@]}" | sort -n
)

VALID_OIDS=()

for id in "${SENSOR_IDS[@]}"; do
    VALID_OIDS+=(
        "$BASE_OID.$id.1"
        "$BASE_OID.$id.2"
        "$BASE_OID.$id.3"
    )
done

TARGET_OID=""

if [ "$MODE" = "-g" ]; then
    for oid in "${VALID_OIDS[@]}"; do
        [ "$oid" = "$REQ_OID" ] && TARGET_OID="$oid" && break
    done
elif [ "$MODE" = "-n" ]; then
    TARGET_OID=$(
        printf '%s\n' "$REQ_OID" "${VALID_OIDS[@]}" |
        sort -Vu |
        awk -v req="$REQ_OID" '$0==req {found=1; next} found {print; exit}'
    )
else
    exit 0
fi

[ -n "$TARGET_OID" ] || exit 0

SENSOR_ID=$(awk -F'.' '{print $(NF-1)}' <<< "$TARGET_OID")
ATTR_ID=$(awk -F'.' '{print $NF}' <<< "$TARGET_OID")

META=$(
    awk -F',' -v id="$SENSOR_ID" 'FNR>1 {
        gsub(/\r/,"",$0)
        if ($1==id) {
            print $2 "|" $4 "|" $6
            exit
        }
    }' "${CSV_FILES[@]}"
)

[ -n "$META" ] || exit 0
IFS='|' read -r SENSOR_TYPE LOCATION CSV_UNIT <<< "$META"

JSON_RES=$(
    curl -fsS --max-time 3 -G \
        "http://$MASTER_IP:$MASTER_PORT/api/sensor" \
        --data-urlencode "type=$SENSOR_TYPE" \
        --data-urlencode "id=$SENSOR_ID" 2>/dev/null
) || exit 0

echo "$JSON_RES" | grep -q '"error"' && exit 0

NAME=$(echo "$JSON_RES" | grep -o '"sensor_name": *"[^"]*"' | cut -d'"' -f4)
UNIT=$(echo "$JSON_RES" | grep -o '"unit": *"[^"]*"' | cut -d'"' -f4)
VALUE=$(echo "$JSON_RES" | grep -o '"value": *[-0-9.]*' | cut -d':' -f2 | tr -d ' ')

[ -n "$UNIT" ] || UNIT="$CSV_UNIT"

case "$ATTR_ID" in
    1) RESULT="$NAME" ;;
    2) RESULT="Type: $SENSOR_TYPE | Location: $LOCATION | Unit: $UNIT" ;;
    3) RESULT="$VALUE" ;;
    *) exit 0 ;;
esac

printf '%s\nstring\n%s\n' "$TARGET_OID" "$RESULT"
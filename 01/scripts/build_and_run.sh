#!/bin/bash

# Script: build_and_run.sh
# Author: Arshia Noroozi (402170562)
# Description: Auto-detects node identity via IP, initializes DB, compiles & launches service.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# پیدا کردن فایل کانفیگ از روت پروژه یا از طریق متغیر محیطی
CONFIG_FILE="${CONFIG_FILE:-$BASE_DIR/../config.txt}"

echo "=========================================="
echo "    Distributed IoT System Bootstrapper   "
echo "=========================================="

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Configuration file not found at $CONFIG_FILE"
    exit 1
fi

MASTER_IP_CFG=$(grep "^MASTER_IP=" "$CONFIG_FILE" | cut -d '=' -f2)

SLAVE1_IP_CFG=$(grep "^SLAVE1_IP=" "$CONFIG_FILE" | cut -d '=' -f2)
SLAVE1_PORT=$(grep "^SLAVE1_PORT=" "$CONFIG_FILE" | cut -d '=' -f2)
SLAVE1_DB=$(grep "^SLAVE1_DB=" "$CONFIG_FILE" | cut -d '=' -f2)

SLAVE2_IP_CFG=$(grep "^SLAVE2_IP=" "$CONFIG_FILE" | cut -d '=' -f2)
SLAVE2_PORT=$(grep "^SLAVE2_PORT=" "$CONFIG_FILE" | cut -d '=' -f2)
SLAVE2_DB=$(grep "^SLAVE2_DB=" "$CONFIG_FILE" | cut -d '=' -f2)

LOCAL_IPS=$(hostname -I)
NODE_ROLE="unknown"

for ip in $LOCAL_IPS; do
    if [[ "$ip" == "$MASTER_IP_CFG" ]]; then
        NODE_ROLE="master"
        break

    elif [[ "$ip" == "$SLAVE1_IP_CFG" ]]; then
        NODE_ROLE="slave1"
        break

    elif [[ "$ip" == "$SLAVE2_IP_CFG" ]]; then
        NODE_ROLE="slave2"
        break
    fi
done

echo " Detected Local IP : $LOCAL_IPS"
echo " Identified Node   : [ $NODE_ROLE ]"
echo "=========================================="

case "$NODE_ROLE" in
    "master")
        echo -e "\n[1/3] Initializing Master Database..."
        chmod +x "$SCRIPT_DIR/master_init_db.sh"
        "$SCRIPT_DIR/master_init_db.sh"

        echo -e "\n[2/3] Compiling Master executable..."
        cd "$BASE_DIR/master" || exit 1
        make clean > /dev/null 2>&1
        make

        echo -e "\n[3/3] Starting Master Service..."
        ./master_node "$CONFIG_FILE"
        ;;

    "slave1")
        echo -e "\n[1/3] Initializing Slave 1 Database..."
        chmod +x "$SCRIPT_DIR/slave1_init_db.sh"
        "$SCRIPT_DIR/slave1_init_db.sh"

        echo -e "\n[2/3] Compiling Slave executable..."
        cd "$BASE_DIR/slave" || exit 1
        make clean > /dev/null 2>&1
        make

        echo -e "\n[3/3] Starting Slave 1 Service on $SLAVE1_IP_CFG:$SLAVE1_PORT..."
        ./slave_node \
            "$SLAVE1_IP_CFG" \
            "$SLAVE1_PORT" \
            "$SLAVE1_DB" \
            "$CONFIG_FILE"
        ;;

    "slave2")
        echo -e "\n[1/3] Initializing Slave 2 Database..."
        chmod +x "$SCRIPT_DIR/slave2_init_db.sh"
        "$SCRIPT_DIR/slave2_init_db.sh"

        echo -e "\n[2/3] Compiling Slave executable..."
        cd "$BASE_DIR/slave" || exit 1
        make clean > /dev/null 2>&1
        make

        echo -e "\n[3/3] Starting Slave 2 Service on $SLAVE2_IP_CFG:$SLAVE2_PORT..."
        ./slave_node \
            "$SLAVE2_IP_CFG" \
            "$SLAVE2_PORT" \
            "$SLAVE2_DB" \
            "$CONFIG_FILE"
        ;;

    *)
        echo "ERROR: Machine IP does not match MASTER_IP, SLAVE1_IP, or SLAVE2_IP in config.txt!"
        echo "Please verify $CONFIG_FILE IP addresses."
        exit 1
        ;;
esac
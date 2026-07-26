#!/bin/bash

# Script: build_and_run.sh
# Author: Arshia Noroozi (402170562)
# Description: Auto-detects node identity, initializes DB, Memcached, MQTT/SNMP config, compiles & launches.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="$BASE_DIR/master/config.txt"

echo "=========================================="
echo "    Distributed IoT System Bootstrapper   "
echo "=========================================="

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Configuration file not found at $CONFIG_FILE"
    exit 1
fi

MASTER_IP_CFG=$(grep -E "^MASTER_IP=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d '\r' | tr -d ' ')
SLAVE1_URL_CFG=$(grep -E "^SLAVE1_URL=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d '\r' | tr -d ' ')
SLAVE2_URL_CFG=$(grep -E "^SLAVE2_URL=" "$CONFIG_FILE" | cut -d'=' -f2 | tr -d '\r' | tr -d ' ')

SLAVE1_IP_CFG=$(echo "$SLAVE1_URL_CFG" | sed -e 's/http:\/\///' -e 's/:.*//')
SLAVE2_IP_CFG=$(echo "$SLAVE2_URL_CFG" | sed -e 's/http:\/\///' -e 's/:.*//')

LOCAL_IPS=$(hostname -I)
NODE_ROLE=""

for ip in $LOCAL_IPS; do
    if [ "$ip" == "$MASTER_IP_CFG" ]; then
        NODE_ROLE="master"
        break
    elif [ "$ip" == "$SLAVE1_IP_CFG" ]; then
        NODE_ROLE="slave1"
        break
    elif [ "$ip" == "$SLAVE2_IP_CFG" ]; then
        NODE_ROLE="slave2"
        break
    fi
done

echo " Detected Local IP : $LOCAL_IPS"
echo " Identified Node   : [ ${NODE_ROLE:-UNKNOWN} ]"
echo "=========================================="

case "$NODE_ROLE" in
    "master")
        echo -e "\n[1/6] Initializing Master Database..."
        chmod +x "$SCRIPT_DIR/master_init_db.sh"
        "$SCRIPT_DIR/master_init_db.sh"

        echo -e "\n[2/6] Initializing Memcached Cache Layer..."
        chmod +x "$SCRIPT_DIR/memcached_init.sh"
        "$SCRIPT_DIR/memcached_init.sh"

        echo -e "\n[3/6] Applying MQTT (Mosquitto) Configuration..."
        # این خط اضافه شد تا پکیج‌های جاافتاده MQTT نصب شوند
        sudo apt update > /dev/null 2>&1
        sudo apt install -y mosquitto mosquitto-clients libmosquitto-dev > /dev/null 2>&1
        
        if [ -f "$BASE_DIR/mosquitto.conf" ]; then
            sudo cp "$BASE_DIR/mosquitto.conf" /etc/mosquitto/conf.d/iot_project.conf
            sudo systemctl restart mosquitto
        fi

        echo -e "\n[4/6] Configuring and Restarting SNMP Daemon (snmpd)..."
        sudo apt install -y snmpd snmp > /dev/null 2>&1
        
        # پیدا کردن مسیر دقیق اسکریپت برای قابلیت pass
        BASH_SCRIPT_PATH="$(cd "$BASE_DIR/snmp" && pwd)/snmp_extension.sh"
        
        sudo bash -c "cat > /etc/snmp/snmpd.conf" << EOL
rocommunity public
pass .1.3.6.1.4.1.402170562 /bin/bash $BASH_SCRIPT_PATH
EOL
        sudo systemctl restart snmpd
        echo "SNMP Service configured with Enterprise OID successfully."

        echo -e "\n[5/6] Compiling Master executable via Makefile..."
        cd "$BASE_DIR/master" || exit 1
        make clean > /dev/null 2>&1
        make

        echo -e "\n[6/6] Starting Master Service..."
        ./master_node config.txt
        ;;

    "slave1")
        echo -e "\n[1/4] Initializing Slave 1 Database..."
        chmod +x "$SCRIPT_DIR/slave1_init_db.sh"
        "$SCRIPT_DIR/slave1_init_db.sh"

        echo -e "\n[2/4] Initializing Memcached Cache Layer..."
        chmod +x "$SCRIPT_DIR/memcached_init.sh"
        "$SCRIPT_DIR/memcached_init.sh"

        echo -e "\n[3/4] Compiling Slave executable via Makefile..."
        cd "$BASE_DIR/slave" || exit 1
        make clean > /dev/null 2>&1
        make

        echo -e "\n[4/4] Starting Slave 1 Service on $SLAVE1_IP_CFG:8001..."
        ./slave_node "$SLAVE1_IP_CFG" 8001 slave1.db
        ;;

    "slave2")
        echo -e "\n[1/4] Initializing Slave 2 Database..."
        chmod +x "$SCRIPT_DIR/slave2_init_db.sh"
        "$SCRIPT_DIR/slave2_init_db.sh"

        echo -e "\n[2/4] Initializing Memcached Cache Layer..."
        chmod +x "$SCRIPT_DIR/memcached_init.sh"
        "$SCRIPT_DIR/memcached_init.sh"

        echo -e "\n[3/4] Compiling Slave executable via Makefile..."
        cd "$BASE_DIR/slave" || exit 1
        make clean > /dev/null 2>&1
        make

        echo -e "\n[4/4] Starting Slave 2 Service on $SLAVE2_IP_CFG:8002..."
        ./slave_node "$SLAVE2_IP_CFG" 8002 slave2.db
        ;;

    *)
        echo "ERROR: Machine IP does not match MASTER_IP, SLAVE1_IP, or SLAVE2_IP in config.txt!"
        echo "Please verify master/config.txt IP addresses."
        exit 1
        ;;
esac
#!/bin/bash
set -e

# Script: build_and_run.sh
# Author: Arshia Noroozi (402170562)
# Description: Auto-detects node identity, initializes services, configures SNMP, compiles and launches.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
CONFIG_FILE="${CONFIG_FILE:-$BASE_DIR/../config.txt}"

get_config() { grep "^$1=" "$CONFIG_FILE" | cut -d '=' -f2- | tr -d '\r'; }

if [ ! -f "$CONFIG_FILE" ]; then
    echo "ERROR: Configuration file not found at $CONFIG_FILE"
    exit 1
fi

MASTER_IP_CFG=$(get_config MASTER_IP | tr -d ' ')
MASTER_PORT=$(get_config MASTER_PORT | tr -d ' ')
MASTER_DB=$(get_config MASTER_DB | tr -d ' ')

SLAVE1_IP_CFG=$(get_config SLAVE1_IP | tr -d ' ')
SLAVE1_PORT=$(get_config SLAVE1_PORT | tr -d ' ')
SLAVE1_DB=$(get_config SLAVE1_DB | tr -d ' ')

SLAVE2_IP_CFG=$(get_config SLAVE2_IP | tr -d ' ')
SLAVE2_PORT=$(get_config SLAVE2_PORT | tr -d ' ')
SLAVE2_DB=$(get_config SLAVE2_DB | tr -d ' ')

LOCAL_IPS=$(hostname -I)
NODE_ROLE="unknown"

for ip in $LOCAL_IPS; do
    if [ "$ip" = "$MASTER_IP_CFG" ]; then NODE_ROLE="master"; break
    elif [ "$ip" = "$SLAVE1_IP_CFG" ]; then NODE_ROLE="slave1"; break
    elif [ "$ip" = "$SLAVE2_IP_CFG" ]; then NODE_ROLE="slave2"; break
    fi
done

echo "=========================================="
echo " Distributed IoT Bootstrapper (SNMP Layer)"
echo "=========================================="
echo " Detected Local IP : $LOCAL_IPS"
echo " Identified Node   : [ $NODE_ROLE ]"
echo "=========================================="

configure_snmp() {
    local snmp_host snmp_port snmp_community snmp_base_oid snmp_allowed
    local runtime_dir extension_path data_dir runtime_config snmpd_config
    local tmp_dir setup_needed=0

    snmp_host=$(get_config SNMP_HOST | tr -d ' ')
    snmp_port=$(get_config SNMP_PORT | tr -d ' ')
    snmp_community=$(get_config SNMP_COMMUNITY | tr -d ' ')
    snmp_base_oid=$(get_config SNMP_BASE_OID | tr -d ' ')
    snmp_allowed=$(get_config SNMP_ALLOWED_SOURCE | tr -d ' ')
    runtime_dir=$(get_config SNMP_RUNTIME_DIR)

    if [ -z "$snmp_host" ] || [ -z "$snmp_port" ] || [ -z "$snmp_community" ] || [ -z "$snmp_base_oid" ] || [ -z "$snmp_allowed" ] || [ -z "$runtime_dir" ]; then
        echo "ERROR: SNMP configuration is incomplete in $CONFIG_FILE"
        exit 1
    fi

    if ! command -v snmpwalk >/dev/null 2>&1 || ! systemctl cat snmpd.service >/dev/null 2>&1; then
        echo "ERROR: SNMP packages are not installed."
        echo "Run once: sudo apt install -y snmp snmpd"
        exit 1
    fi

    extension_path="$runtime_dir/snmp_extension.sh"
    data_dir="$runtime_dir/data"
    runtime_config="$runtime_dir/snmp_runtime.conf"
    snmpd_config="/etc/snmp/snmpd.conf"
    tmp_dir=$(mktemp -d)

    cat > "$tmp_dir/snmp_runtime.conf" <<CFG
MASTER_IP=$MASTER_IP_CFG
MASTER_PORT=$MASTER_PORT
SNMP_BASE_OID=$snmp_base_oid
SNMP_DATA_DIR=$data_dir
CFG

    cat > "$tmp_dir/snmpd.conf" <<CFG
agentAddress udp:$snmp_host:$snmp_port
rocommunity $snmp_community $snmp_allowed
pass $snmp_base_oid $extension_path
CFG

    [ -f "$extension_path" ] && cmp -s "$BASE_DIR/snmp/snmp_extension.sh" "$extension_path" || setup_needed=1
    [ -f "$runtime_config" ] && cmp -s "$tmp_dir/snmp_runtime.conf" "$runtime_config" || setup_needed=1
    [ -f "$snmpd_config" ] && cmp -s "$tmp_dir/snmpd.conf" "$snmpd_config" || setup_needed=1

    for file in master_sensors.csv slave1_sensors.csv slave2_sensors.csv; do
        [ -f "$data_dir/$file" ] && cmp -s "$BASE_DIR/data/$file" "$data_dir/$file" || setup_needed=1
    done

    systemctl is-active --quiet snmpd || setup_needed=1

    if [ "$setup_needed" -eq 1 ]; then
        echo "[SNMP] Installing or updating runtime files..."
        sudo install -d -m 755 "$runtime_dir" "$data_dir"
        sudo install -m 755 "$BASE_DIR/snmp/snmp_extension.sh" "$extension_path"
        sudo install -m 644 "$tmp_dir/snmp_runtime.conf" "$runtime_config"
        sudo install -m 644 "$BASE_DIR/data/"*.csv "$data_dir/"
        sudo install -m 644 "$tmp_dir/snmpd.conf" "$snmpd_config"
        sudo systemctl enable --now snmpd >/dev/null
        sudo systemctl restart snmpd
    fi

    rm -rf "$tmp_dir"
    echo "[SNMP] snmpd is active on $snmp_host:$snmp_port with base OID $snmp_base_oid."
}

case "$NODE_ROLE" in
    "master")
        echo -e "\n[1/6] Initializing Master Database..."
        chmod +x "$SCRIPT_DIR/master_init_db.sh"
        "$SCRIPT_DIR/master_init_db.sh"

        echo -e "\n[2/6] Initializing Memcached Cache Layer..."
        chmod +x "$SCRIPT_DIR/memcached_init.sh"
        "$SCRIPT_DIR/memcached_init.sh" "$CONFIG_FILE"

        echo -e "\n[3/6] Checking MQTT Broker..."
        if ! systemctl is-active --quiet mosquitto; then
            echo "ERROR: Mosquitto is not running."
            echo "Run once: sudo systemctl enable --now mosquitto"
            exit 1
        fi
        echo "[MQTT] Mosquitto service is active."

        echo -e "\n[4/6] Configuring SNMP Agent..."
        configure_snmp

        echo -e "\n[5/6] Compiling Master executable via Makefile..."
        cd "$BASE_DIR/master"
        make clean >/dev/null 2>&1
        make

        echo -e "\n[6/6] Starting Master Service on $MASTER_IP_CFG:$MASTER_PORT..."
        ./master_node "$CONFIG_FILE"
        ;;

    "slave1")
        echo -e "\n[1/4] Initializing Slave 1 Database..."
        chmod +x "$SCRIPT_DIR/slave1_init_db.sh"
        "$SCRIPT_DIR/slave1_init_db.sh"

        echo -e "\n[2/4] Initializing Memcached Cache Layer..."
        chmod +x "$SCRIPT_DIR/memcached_init.sh"
        "$SCRIPT_DIR/memcached_init.sh" "$CONFIG_FILE"

        echo -e "\n[3/4] Compiling Slave executable via Makefile..."
        cd "$BASE_DIR/slave"
        make clean >/dev/null 2>&1
        make

        echo -e "\n[4/4] Starting Slave 1 Service on $SLAVE1_IP_CFG:$SLAVE1_PORT..."
        ./slave_node "$SLAVE1_IP_CFG" "$SLAVE1_PORT" "$SLAVE1_DB" "$CONFIG_FILE"
        ;;

    "slave2")
        echo -e "\n[1/4] Initializing Slave 2 Database..."
        chmod +x "$SCRIPT_DIR/slave2_init_db.sh"
        "$SCRIPT_DIR/slave2_init_db.sh"

        echo -e "\n[2/4] Initializing Memcached Cache Layer..."
        chmod +x "$SCRIPT_DIR/memcached_init.sh"
        "$SCRIPT_DIR/memcached_init.sh" "$CONFIG_FILE"

        echo -e "\n[3/4] Compiling Slave executable via Makefile..."
        cd "$BASE_DIR/slave"
        make clean >/dev/null 2>&1
        make

        echo -e "\n[4/4] Starting Slave 2 Service on $SLAVE2_IP_CFG:$SLAVE2_PORT..."
        ./slave_node "$SLAVE2_IP_CFG" "$SLAVE2_PORT" "$SLAVE2_DB" "$CONFIG_FILE"
        ;;

    *)
        echo "ERROR: Machine IP does not match MASTER_IP, SLAVE1_IP, or SLAVE2_IP."
        echo "Please verify $CONFIG_FILE"
        exit 1
        ;;
esac
#!/bin/bash
# Script: test_snmp.sh

echo "====================================================="
echo "        Reading All Sensors via SNMP (snmpwalk)      "
echo "====================================================="
echo " Target Enterprise OID: .1.3.6.1.4.1.402170562"
echo "====================================================="
echo ""

snmpwalk -v2c -c public localhost .1.3.6.1.4.1.402170562

echo ""
echo "====================================================="
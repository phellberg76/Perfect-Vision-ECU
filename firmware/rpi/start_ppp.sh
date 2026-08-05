#!/bin/bash
#
# Start PPP connection to ESP32 for internet access
#
# Prerequisites:
# 1. ESP32 must be flashed with unified firmware
# 2. ESP32 must be connected to WiFi
# 3. PPP mode must be started on ESP32
#
# Usage:
#   ./start_ppp.sh
#

set -e

SERIAL_PORT="/dev/ttyAMA0"
BAUD_RATE="115200"
LOCAL_IP="10.0.0.2"
REMOTE_IP="10.0.0.1"

echo "=== ESP32 PPP Connection ==="
echo ""

# Check if serial port exists
if [ ! -e "$SERIAL_PORT" ]; then
    echo "Error: $SERIAL_PORT not found"
    exit 1
fi

# Check if already running
if pgrep -x pppd > /dev/null; then
    echo "pppd is already running. Stop it first with: sudo killall pppd"
    exit 1
fi

# Start PPP mode on ESP32 (if not already)
echo "[1/3] Requesting PPP mode on ESP32..."
stty -F "$SERIAL_PORT" "$BAUD_RATE" cs8 -cstopb -parenb
echo "PPP_START" > "$SERIAL_PORT"
sleep 1

# Read any response
timeout 1 cat "$SERIAL_PORT" || true
echo ""

# Start pppd
echo "[2/3] Starting pppd..."
sudo pppd "$SERIAL_PORT" "$BAUD_RATE" \
    "$LOCAL_IP:$REMOTE_IP" \
    noauth \
    local \
    nocrtscts \
    persist \
    maxfail 0 \
    holdoff 5 \
    nodetach \
    debug \
    defaultroute &

PPPD_PID=$!
echo "pppd started with PID $PPPD_PID"

# Wait for interface to come up
echo "[3/3] Waiting for ppp0 interface..."
for i in {1..30}; do
    if ip link show ppp0 2>/dev/null | grep -q "UP"; then
        echo ""
        echo "=== PPP Connection Established ==="
        ip addr show ppp0
        echo ""
        echo "Testing connectivity..."
        if ping -c 1 -W 2 "$REMOTE_IP" > /dev/null 2>&1; then
            echo "✓ Can reach ESP32 at $REMOTE_IP"
        else
            echo "✗ Cannot reach ESP32"
        fi
        if ping -c 1 -W 2 8.8.8.8 > /dev/null 2>&1; then
            echo "✓ Internet connectivity working"
        else
            echo "✗ No internet connectivity (check ESP32 WiFi)"
        fi
        echo ""
        echo "To stop PPP: sudo killall pppd"
        exit 0
    fi
    echo -n "."
    sleep 1
done

echo ""
echo "Error: ppp0 interface did not come up"
echo "Check ESP32 WiFi connection and PPP mode"
sudo killall pppd 2>/dev/null || true
exit 1

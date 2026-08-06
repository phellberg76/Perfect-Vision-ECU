#!/bin/bash
#
# PPP Test Script - Manual testing of ESP32 PPP
#
# Run this to test PPP step by step.
# Requires two terminals:
#   Terminal 1: Run this script
#   Terminal 2: Watch ESP32 debug output (minicom or screen)
#

SERIAL_PORT="/dev/ttyAMA0"
BAUD_RATE="115200"

echo "=== ESP32 PPP Test ==="
echo ""
echo "1. Configure serial port..."
stty -F "$SERIAL_PORT" "$BAUD_RATE" cs8 -cstopb -parenb raw -echo

echo "2. Sending PPP_START command..."
echo "PPP_START" > "$SERIAL_PORT"

echo "3. Waiting 2 seconds for ESP32 to enter PPP mode..."
sleep 2

echo "4. Starting pppd with debug..."
echo ""
echo "Press Ctrl+C to stop"
echo ""

# Run pppd in foreground with debug output
sudo pppd "$SERIAL_PORT" "$BAUD_RATE" \
    10.0.0.2:10.0.0.1 \
    noauth \
    local \
    nocrtscts \
    debug \
    nodetach \
    defaultroute

echo ""
echo "pppd exited"

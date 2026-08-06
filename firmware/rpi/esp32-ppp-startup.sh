#!/bin/bash
#
# ESP32 PPP Startup Script
#
# Configures WiFi on ESP32 and starts PPP for network sharing.
# Designed to run from rc.local or systemd.
#
# Environment variables (set in rc.local or systemd unit):
#   ESP32_WIFI_SSID     - WiFi network name
#   ESP32_WIFI_PASS     - WiFi password
#   ESP32_PPP_ENABLE    - Set to "1" to start PPP (optional)
#
# Usage:
#   ESP32_WIFI_SSID="MyNetwork" ESP32_WIFI_PASS="MyPassword" ./esp32-ppp-startup.sh
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_FILE="/var/log/esp32-ppp.log"
SERIAL_PORT="/dev/ttyAMA0"
BAUD_RATE="115200"

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') [ESP32-PPP] $1" | tee -a "$LOG_FILE"
}

send_cmd() {
    echo "$1" > "$SERIAL_PORT"
    sleep 0.5
}

read_response() {
    # Read available data with short timeout
    local response=""
    response=$(timeout 1 head -c 1000 < "$SERIAL_PORT" 2>/dev/null) || true
    echo "$response"
}

wait_for_esp32() {
    log "Waiting for ESP32 to boot..."
    for i in {1..30}; do
        # Read any available data (heartbeats come every 5 seconds)
        response=$(timeout 2 head -c 500 < "$SERIAL_PORT" 2>/dev/null) || true
        # Check for any valid JSON response (heartbeat, status, etc.)
        if echo "$response" | grep -qE '"type":"(heartbeat|status|startup)"'; then
            log "ESP32 is responding"
            log "Response: $(echo "$response" | head -1)"
            return 0
        fi
        sleep 1
    done
    log "ERROR: ESP32 not responding after 30 seconds"
    return 1
}

configure_wifi() {
    if [ -z "$ESP32_WIFI_SSID" ]; then
        log "ERROR: ESP32_WIFI_SSID not set"
        return 1
    fi

    log "Configuring WiFi: SSID=$ESP32_WIFI_SSID"

    # Configure serial port
    stty -F "$SERIAL_PORT" "$BAUD_RATE" cs8 -cstopb -parenb raw -echo 2>/dev/null

    # Set credentials
    send_cmd "WIFI_SSID $ESP32_WIFI_SSID"
    send_cmd "WIFI_PASS $ESP32_WIFI_PASS"
    send_cmd "WIFI_CONNECT"

    # Wait for connection - check heartbeat messages for wifi status
    log "Waiting for WiFi connection..."
    for i in {1..60}; do
        # Read heartbeat messages (come every 5 seconds)
        response=$(timeout 6 head -c 500 < "$SERIAL_PORT" 2>/dev/null) || true

        # Check for wifi:true in heartbeat
        if echo "$response" | grep -q '"wifi":true'; then
            log "WiFi connected!"
            # Try to get IP from WIFI command
            send_cmd "WIFI"
            sleep 0.5
            wifi_response=$(timeout 2 head -c 300 < "$SERIAL_PORT" 2>/dev/null) || true
            if echo "$wifi_response" | grep -q '"ip"'; then
                ip=$(echo "$wifi_response" | grep -o '"ip":"[^"]*"' | cut -d'"' -f4)
                log "ESP32 IP: $ip"
            fi
            return 0
        fi
        log "Waiting... (attempt $i/60)"
    done

    log "ERROR: WiFi connection failed after timeout"
    return 1
}

start_ppp() {
    log "Starting PPP mode on ESP32..."
    send_cmd "PPP_START"
    sleep 1

    log "Starting pppd..."
    pppd "$SERIAL_PORT" "$BAUD_RATE" \
        10.0.0.2:10.0.0.1 \
        noauth \
        local \
        nocrtscts \
        persist \
        maxfail 0 \
        holdoff 5 \
        defaultroute \
        usepeerdns \
        logfile /var/log/pppd.log &

    PPPD_PID=$!
    log "pppd started with PID $PPPD_PID"

    # Wait for ppp0 interface
    for i in {1..30}; do
        if ip link show ppp0 2>/dev/null | grep -q "UP"; then
            log "PPP interface ppp0 is UP"
            ip addr show ppp0 | tee -a "$LOG_FILE"

            # Enable IP forwarding and NAT for hotspot
            log "Enabling IP forwarding and NAT..."
            sysctl -w net.ipv4.ip_forward=1 > /dev/null
            iptables -t nat -C POSTROUTING -o ppp0 -j MASQUERADE 2>/dev/null || \
                iptables -t nat -A POSTROUTING -o ppp0 -j MASQUERADE

            log "PPP setup complete!"
            return 0
        fi
        sleep 1
    done

    log "ERROR: ppp0 interface did not come up"
    return 1
}

# Main
log "========================================="
log "ESP32 PPP Startup Script"
log "========================================="

# Wait for serial port
while [ ! -e "$SERIAL_PORT" ]; do
    log "Waiting for $SERIAL_PORT..."
    sleep 1
done

# Configure serial
stty -F "$SERIAL_PORT" "$BAUD_RATE" cs8 -cstopb -parenb raw -echo

# Wait for ESP32
if ! wait_for_esp32; then
    exit 1
fi

# Configure WiFi
if ! configure_wifi; then
    exit 1
fi

# Start PPP if enabled
if [ "$ESP32_PPP_ENABLE" = "1" ]; then
    if ! start_ppp; then
        exit 1
    fi
else
    log "PPP not enabled (set ESP32_PPP_ENABLE=1 to enable)"
fi

log "Startup complete!"

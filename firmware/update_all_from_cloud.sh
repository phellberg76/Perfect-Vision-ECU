#!/bin/bash
#
# Update all Planter/Implement app components from cloud server
#
# Updates:
#   1. ESP32 firmware (flash via UART)
#   2. Rust BLE bridge (copy and restart service)
#   3. Java webapp (copy WAR and restart Tomcat)
#
# Usage:
#   ./update_all_from_cloud.sh           - Update all components
#   ./update_all_from_cloud.sh esp32     - Update ESP32 only
#   ./update_all_from_cloud.sh rust      - Update Rust only
#   ./update_all_from_cloud.sh java      - Update Java only
#
# Prerequisites:
#   - Run compile_all.sh on cloud server first
#   - SSH access to cloud server configured
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"
TMP_DIR="/tmp/planter-update"

# Cloud server settings
CLOUD_HOST="peter@cloud"
CLOUD_PORT="3591"
CLOUD_BASE="/Source/Raspberry-Pi-Backup-power-and-GPS-hat-v4/firmware"
CLOUD_ESP32_DIR="$CLOUD_BASE/.pio/build/esp32s3"
CLOUD_RUST_BIN="$CLOUD_BASE/rpi-bridge/target/aarch64-unknown-linux-gnu/release/planter-ble-bridge"
CLOUD_JAVA_DIR="/Source/Perfect-Precision-Implement-app/target"

# Local paths
RUST_BIN_DEST="/usr/local/bin/planter-ble-bridge"
JAVA_WAR_DEST="/opt/tomcat/webapps/planter_app.war"

# Hardware connections for ESP32
UART_DEV="/dev/ttyAMA0"
GPIO_EN=17
GPIO_IO0=27

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[UPDATE]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Set GPIO using pinctrl
gpio_set() {
    local pin=$1
    local value=$2

    if command -v pinctrl &> /dev/null; then
        pinctrl set $pin op
        if [ "$value" = "1" ]; then
            pinctrl set $pin dh
        else
            pinctrl set $pin dl
        fi
    elif command -v gpioset &> /dev/null; then
        gpioset gpiochip0 $pin=$value
    else
        if [ ! -d "/sys/class/gpio/gpio$pin" ]; then
            echo $pin > /sys/class/gpio/export 2>/dev/null || true
            sleep 0.1
        fi
        echo out > /sys/class/gpio/gpio$pin/direction 2>/dev/null || true
        echo $value > /sys/class/gpio/gpio$pin/value
    fi
}

enter_bootloader() {
    log "Entering bootloader mode..."
    gpio_set $GPIO_IO0 0
    sleep 0.1
    gpio_set $GPIO_EN 0
    sleep 0.1
    gpio_set $GPIO_EN 1
    sleep 0.3
}

reset_esp32() {
    log "Resetting ESP32..."
    gpio_set $GPIO_IO0 1
    sleep 0.1
    gpio_set $GPIO_EN 0
    sleep 0.1
    gpio_set $GPIO_EN 1
}

update_esp32() {
    log "=== Updating ESP32 Firmware ==="

    mkdir -p "$TMP_DIR"

    log "Fetching ESP32 firmware from cloud..."
    scp -P "$CLOUD_PORT" \
        "$CLOUD_HOST:$CLOUD_ESP32_DIR/bootloader.bin" \
        "$CLOUD_HOST:$CLOUD_ESP32_DIR/partitions.bin" \
        "$CLOUD_HOST:$CLOUD_ESP32_DIR/firmware.bin" \
        "$TMP_DIR/" || error "Failed to fetch ESP32 firmware"

    # Stop services using serial port
    if systemctl is-active --quiet planter-ble-bridge.service 2>/dev/null; then
        log "Stopping planter-ble-bridge.service..."
        sudo systemctl stop planter-ble-bridge.service
    fi
    if pgrep -x pppd > /dev/null; then
        sudo killall pppd 2>/dev/null || true
        sleep 1
    fi

    # Clean up serial port
    if command -v fuser &> /dev/null; then
        sudo fuser -k "$UART_DEV" 2>/dev/null || true
    fi

    enter_bootloader

    log "Flashing ESP32..."
    "$VENV_DIR/bin/python" -m esptool --chip esp32s3 \
        --port "$UART_DEV" \
        --baud 460800 \
        --before no-reset \
        --after no-reset \
        write-flash \
        -z \
        --flash-mode dio \
        --flash-freq 80m \
        --flash-size 8MB \
        0x0000 "$TMP_DIR/bootloader.bin" \
        0x8000 "$TMP_DIR/partitions.bin" \
        0x10000 "$TMP_DIR/firmware.bin"

    reset_esp32
    log "ESP32 update complete!"
}

update_rust() {
    log "=== Updating Rust BLE Bridge ==="

    mkdir -p "$TMP_DIR"

    log "Fetching Rust binary from cloud..."
    scp -P "$CLOUD_PORT" "$CLOUD_HOST:$CLOUD_RUST_BIN" "$TMP_DIR/planter-ble-bridge" \
        || error "Failed to fetch Rust binary"

    log "Stopping planter-ble-bridge service..."
    sudo systemctl stop planter-ble-bridge.service 2>/dev/null || true

    log "Installing new binary..."
    sudo cp "$TMP_DIR/planter-ble-bridge" "$RUST_BIN_DEST"
    sudo chmod +x "$RUST_BIN_DEST"

    log "Starting planter-ble-bridge service..."
    sudo systemctl start planter-ble-bridge.service

    log "Rust update complete!"
}

update_java() {
    log "=== Updating Java Webapp ==="

    mkdir -p "$TMP_DIR"

    log "Fetching WAR file from cloud..."
    scp -P "$CLOUD_PORT" "$CLOUD_HOST:$CLOUD_JAVA_DIR/planter_app*.war" "$TMP_DIR/" \
        || error "Failed to fetch WAR file"

    WAR_FILE=$(ls "$TMP_DIR"/*.war 2>/dev/null | head -1)
    if [ -z "$WAR_FILE" ]; then
        error "No WAR file found"
    fi

    log "Deploying to Tomcat..."
    sudo mv "$WAR_FILE" "$JAVA_WAR_DEST"

    log "Restarting Tomcat..."
    sudo service tomcat restart

    log "Java update complete!"
}

cleanup() {
    rm -rf "$TMP_DIR"
}

# Main
trap cleanup EXIT

case "${1:-all}" in
    esp32)
        update_esp32
        ;;
    rust)
        update_rust
        ;;
    java)
        update_java
        ;;
    all|*)
        log "=== Updating All Components from Cloud ==="
        echo ""
        update_esp32
        echo ""
        update_rust
        echo ""
        update_java
        echo ""
        log "=== All updates complete! ==="
        ;;
esac

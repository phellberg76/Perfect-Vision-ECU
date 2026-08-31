#!/bin/bash
#
# Update script for RPi Backup Power & GPS Hat v4 - ESP32 Firmware
# Fetches pre-compiled binaries from cloud server and flashes via UART
#
# Usage:
#   ./update_from_cloud.sh
#
# Prerequisites:
#   - Firmware compiled on cloud server with compile.sh
#   - SSH access to cloud server configured
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"
TMP_DIR="/tmp/gps-hat-firmware"

# Cloud server settings - adjust as needed
CLOUD_HOST="peter@cloud"
CLOUD_PORT="3591"
CLOUD_FIRMWARE_DIR="/Source/Raspberry-Pi-Backup-power-and-GPS-hat-v4/firmware/.pio/build/esp32s3"

# Hardware connections
UART_DEV="/dev/ttyAMA0"
GPIO_EN=17      # ESP32 EN (reset)
GPIO_IO0=27     # ESP32 IO0 (boot mode select)

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
        # Set as output first, then set value (required on some CM4s)
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

# Stop services that use the serial port
stop_services() {
    log "Stopping services that use serial port..."
    if systemctl is-active --quiet http-proxy.service 2>/dev/null; then
        log "Stopping http-proxy.service..."
        sudo systemctl stop http-proxy.service
    fi
    if pgrep -x pppd > /dev/null; then
        log "Stopping pppd..."
        sudo killall pppd 2>/dev/null || true
        sleep 1
    fi
}

# Restart services after flashing
restart_services() {
    log "Restarting services..."
    sleep 2
    if systemctl is-enabled --quiet http-proxy.service 2>/dev/null; then
        log "Starting http-proxy.service..."
        sudo systemctl start http-proxy.service
    fi
}

# Enter bootloader mode via GPIO
enter_bootloader() {
    log "Entering bootloader mode via GPIO..."
    gpio_set $GPIO_IO0 0   # IO0 low (boot mode)
    sleep 0.1
    gpio_set $GPIO_EN 0    # EN low (reset)
    sleep 0.1
    gpio_set $GPIO_EN 1    # EN high (release reset)
    sleep 0.3
    log "ESP32 in bootloader mode"
}

# Reset ESP32 for normal boot
reset_esp32() {
    log "Resetting ESP32 via GPIO..."
    gpio_set $GPIO_IO0 1   # IO0 high (normal boot)
    sleep 0.1
    gpio_set $GPIO_EN 0    # EN low
    sleep 0.1
    gpio_set $GPIO_EN 1    # EN high
    log "ESP32 reset complete"
}

# Main
log "=== GPS Hat Firmware Update from Cloud ==="

# Create temp directory
mkdir -p "$TMP_DIR"

# Fetch firmware from cloud
log "Fetching firmware from cloud server..."
scp -P "$CLOUD_PORT" \
    "$CLOUD_HOST:$CLOUD_FIRMWARE_DIR/bootloader.bin" \
    "$CLOUD_HOST:$CLOUD_FIRMWARE_DIR/partitions.bin" \
    "$CLOUD_HOST:$CLOUD_FIRMWARE_DIR/firmware.bin" \
    "$TMP_DIR/"

if [ ! -f "$TMP_DIR/firmware.bin" ]; then
    error "Failed to fetch firmware from cloud"
fi

log "Firmware fetched successfully"

# Stop services and prepare for flashing
stop_services

# Clean up serial port
if command -v fuser &> /dev/null; then
    sudo fuser -k "$UART_DEV" 2>/dev/null || true
fi
if [ -e "$UART_DEV" ]; then
    stty -F "$UART_DEV" 115200 cs8 -cstopb -parenb raw -echo 2>/dev/null || true
fi

# Enter bootloader mode
enter_bootloader

# Flash firmware
log "Flashing firmware..."
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

log "Flash complete"

# Reset and restart services
reset_esp32
restart_services

# Cleanup
rm -rf "$TMP_DIR"

log "=== Update complete! ==="

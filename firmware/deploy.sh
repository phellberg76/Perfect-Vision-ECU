#!/bin/bash
#
# Deploy script for RPi Backup Power & GPS Hat v4 - ESP32 Firmware
#
# The ESP32 is connected via:
#   - RPi UART (TX/RX) -> ESP32 UART
#   - RPi GPIO17 -> ESP32 EN (reset)
#   - RPi GPIO27 -> ESP32 IO0 (boot mode)
#
# Usage:
#   ./deploy.sh          - Pull latest and upload to ESP32
#   ./deploy.sh build    - Pull latest and build only (no upload)
#   ./deploy.sh upload   - Upload without pulling
#   ./deploy.sh monitor  - Open serial monitor
#   ./deploy.sh setup    - Run first-time setup
#   ./deploy.sh reset    - Reset ESP32 (normal boot)
#   ./deploy.sh bootloader - Put ESP32 in bootloader mode
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
VENV_DIR="$SCRIPT_DIR/.venv"
PIO="$VENV_DIR/bin/pio"
ESPTOOL="$VENV_DIR/bin/esptool.py"

# Find esptool - check venv first, then PlatformIO packages
find_esptool() {
    if [ -f "$ESPTOOL" ]; then
        echo "$ESPTOOL"
    elif [ -f "$VENV_DIR/bin/esptool" ]; then
        echo "$VENV_DIR/bin/esptool"
    else
        # Use PlatformIO's bundled esptool
        local pio_esptool=$(find ~/.platformio/packages/tool-esptoolpy -name "esptool.py" 2>/dev/null | head -1)
        if [ -n "$pio_esptool" ]; then
            echo "$pio_esptool"
        else
            echo ""
        fi
    fi
}

# Hardware connections
UART_DEV="/dev/ttyAMA0"
GPIO_EN=17      # ESP32 EN (reset)
GPIO_IO0=27     # ESP32 IO0 (boot mode select)

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[DEPLOY]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Set GPIO using pinctrl (modern RPi OS) or fallback to gpioset/sysfs
gpio_set() {
    local pin=$1
    local value=$2

    if command -v pinctrl &> /dev/null; then
        # Modern RPi OS - use pinctrl
        pinctrl set $pin op dl  # Set as output, drive low first
        if [ "$value" = "1" ]; then
            pinctrl set $pin op dh  # Drive high
        else
            pinctrl set $pin op dl  # Drive low
        fi
    elif command -v gpioset &> /dev/null; then
        # Use libgpiod
        gpioset gpiochip0 $pin=$value
    else
        # Fallback to sysfs (deprecated)
        if [ ! -d "/sys/class/gpio/gpio$pin" ]; then
            echo $pin > /sys/class/gpio/export 2>/dev/null || true
            sleep 0.1
        fi
        echo out > /sys/class/gpio/gpio$pin/direction 2>/dev/null || true
        echo $value > /sys/class/gpio/gpio$pin/value
    fi
}

# Reset ESP32 into bootloader mode
enter_bootloader() {
    log "Entering bootloader mode..."

    # IO0 LOW (boot mode)
    gpio_set $GPIO_IO0 0
    sleep 0.1

    # Pulse EN LOW then HIGH (reset)
    gpio_set $GPIO_EN 0
    sleep 0.1
    gpio_set $GPIO_EN 1
    sleep 0.3

    log "ESP32 should now be in bootloader mode"
}

# Reset ESP32 for normal boot
reset_esp32() {
    log "Resetting ESP32..."

    # IO0 HIGH (normal boot)
    gpio_set $GPIO_IO0 1
    sleep 0.1

    # Pulse EN LOW then HIGH (reset)
    gpio_set $GPIO_EN 0
    sleep 0.1
    gpio_set $GPIO_EN 1

    log "ESP32 reset complete"
}

# Check for venv/PlatformIO
check_pio() {
    if [ ! -f "$PIO" ]; then
        error "PlatformIO not installed. Run: ./deploy.sh setup"
    fi
}

# First-time setup
setup() {
    log "Running first-time setup..."
    "$SCRIPT_DIR/rpi-setup.sh"
}

# Pull latest from git
pull() {
    log "Pulling latest changes..."
    cd "$REPO_DIR"
    git fetch origin
    git pull origin master
    log "Pull complete."
}

# Build firmware
build() {
    log "Building firmware..."
    cd "$SCRIPT_DIR"
    "$PIO" run
    log "Build complete."
}

# Upload to ESP32 via UART
upload() {
    log "Uploading to ESP32 via $UART_DEV..."
    cd "$SCRIPT_DIR"

    if [ ! -e "$UART_DEV" ]; then
        error "UART device $UART_DEV not found"
    fi

    # Enter bootloader mode
    enter_bootloader

    # Flash using esptool
    FIRMWARE="$SCRIPT_DIR/.pio/build/esp32s3/firmware.bin"
    BOOTLOADER="$SCRIPT_DIR/.pio/build/esp32s3/bootloader.bin"
    PARTITIONS="$SCRIPT_DIR/.pio/build/esp32s3/partitions.bin"

    if [ ! -f "$FIRMWARE" ]; then
        error "Firmware not found. Run build first."
    fi

    # Find esptool
    local esptool_path=$(find_esptool)
    if [ -z "$esptool_path" ]; then
        error "esptool not found. Installing..."
        "$VENV_DIR/bin/pip" install esptool
        esptool_path="$VENV_DIR/bin/esptool.py"
    fi

    log "Flashing firmware using $esptool_path..."
    python3 "$esptool_path" --chip esp32s3 \
        --port "$UART_DEV" \
        --baud 460800 \
        --before no_reset \
        --after no_reset \
        write_flash \
        -z \
        --flash_mode dio \
        --flash_freq 80m \
        --flash_size 8MB \
        0x0000 "$BOOTLOADER" \
        0x8000 "$PARTITIONS" \
        0x10000 "$FIRMWARE"

    log "Upload complete."

    # Reset to normal boot
    reset_esp32
}

# Serial monitor
monitor() {
    log "Starting serial monitor on $UART_DEV (Ctrl+C to exit)..."

    # Make sure ESP32 is in normal mode
    reset_esp32
    sleep 0.5

    # Use screen or minicom
    if command -v screen &> /dev/null; then
        screen "$UART_DEV" 115200
    elif command -v minicom &> /dev/null; then
        minicom -D "$UART_DEV" -b 115200
    else
        # Fallback to cat
        stty -F "$UART_DEV" 115200 raw -echo
        cat "$UART_DEV"
    fi
}

# Main
case "${1:-deploy}" in
    setup)
        setup
        ;;
    build)
        check_pio
        pull
        build
        ;;
    upload)
        check_pio
        upload
        ;;
    monitor)
        monitor
        ;;
    reset)
        reset_esp32
        ;;
    bootloader)
        enter_bootloader
        ;;
    deploy|*)
        check_pio
        pull
        build
        upload
        log "Deployment complete!"
        ;;
esac

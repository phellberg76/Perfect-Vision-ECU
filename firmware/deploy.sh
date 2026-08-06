#!/bin/bash
#
# Deploy script for RPi Backup Power & GPS Hat v4 - ESP32 Firmware
#
# Supports two programming methods:
#   1. USB - Connect ESP32 USB-C directly (auto-detects /dev/ttyACM* or /dev/ttyUSB*)
#   2. UART - Via RPi's hardware UART with GPIO reset control
#
# The ESP32 is connected via UART:
#   - RPi UART (TX/RX) -> ESP32 UART
#   - RPi GPIO17 -> ESP32 EN (reset)
#   - RPi GPIO27 -> ESP32 IO0 (boot mode)
#
# Usage:
#   ./deploy.sh              - Pull, build, and upload (auto-detect method)
#   ./deploy.sh build        - Pull and build only
#   ./deploy.sh upload       - Upload only (auto-detect)
#   ./deploy.sh upload usb   - Upload via USB
#   ./deploy.sh upload uart  - Upload via UART with GPIO control
#   ./deploy.sh monitor      - Open serial monitor
#   ./deploy.sh setup        - Run first-time setup
#   ./deploy.sh reset        - Reset ESP32 via GPIO
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
VENV_DIR="$SCRIPT_DIR/.venv"
PIO="$VENV_DIR/bin/pio"

# Hardware connections (UART method)
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

# Clean up PPP and serial port before flashing
cleanup_serial() {
    log "Cleaning up serial port..."

    # Kill any pppd processes using our serial port
    if pgrep -x pppd > /dev/null; then
        log "Stopping pppd..."
        sudo killall pppd 2>/dev/null || true
        sleep 1
    fi

    # Kill any process holding the serial port
    if command -v fuser &> /dev/null; then
        sudo fuser -k "$UART_DEV" 2>/dev/null || true
    fi

    # Reset serial port to known state
    if [ -e "$UART_DEV" ]; then
        stty -F "$UART_DEV" 115200 cs8 -cstopb -parenb raw -echo 2>/dev/null || true
    fi

    # Reset ESP32 to get it out of PPP mode
    log "Resetting ESP32..."
    reset_esp32 2>/dev/null || true
    sleep 0.5
}

# Detect USB serial device
find_usb_device() {
    local dev=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1)
    echo "$dev"
}

# Set GPIO using pinctrl (modern RPi OS) or fallback
gpio_set() {
    local pin=$1
    local value=$2

    if command -v pinctrl &> /dev/null; then
        if [ "$value" = "1" ]; then
            pinctrl set $pin op dh
        else
            pinctrl set $pin op dl
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

# Reset ESP32 into bootloader mode via GPIO
enter_bootloader() {
    log "Entering bootloader mode via GPIO..."
    gpio_set $GPIO_IO0 0   # IO0 low (boot mode)
    sleep 0.1
    gpio_set $GPIO_EN 0    # EN low (reset)
    sleep 0.1
    gpio_set $GPIO_EN 1    # EN high (release reset)
    sleep 0.3
    log "ESP32 should now be in bootloader mode"
}

# Reset ESP32 for normal boot via GPIO
reset_esp32() {
    log "Resetting ESP32 via GPIO..."
    gpio_set $GPIO_IO0 1   # IO0 high (normal boot)
    sleep 0.1
    gpio_set $GPIO_EN 0    # EN low
    sleep 0.1
    gpio_set $GPIO_EN 1    # EN high
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

# Upload via USB (simple - esptool handles reset automatically)
upload_usb() {
    local dev=$1
    log "Uploading to ESP32 via USB ($dev)..."
    cd "$SCRIPT_DIR"

    "$PIO" run -t upload --upload-port "$dev"
    log "Upload complete."
}

# Upload via UART with GPIO reset control
upload_uart() {
    log "Uploading to ESP32 via UART ($UART_DEV)..."
    cd "$SCRIPT_DIR"

    # Clean up any PPP session and reset serial port
    cleanup_serial

    if [ ! -e "$UART_DEV" ]; then
        error "UART device $UART_DEV not found"
    fi

    enter_bootloader

    FIRMWARE="$SCRIPT_DIR/.pio/build/esp32s3/firmware.bin"
    BOOTLOADER="$SCRIPT_DIR/.pio/build/esp32s3/bootloader.bin"
    PARTITIONS="$SCRIPT_DIR/.pio/build/esp32s3/partitions.bin"

    if [ ! -f "$FIRMWARE" ]; then
        error "Firmware not found. Run build first."
    fi

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
        0x0000 "$BOOTLOADER" \
        0x8000 "$PARTITIONS" \
        0x10000 "$FIRMWARE"

    log "Upload complete."
    reset_esp32
}

# Upload with auto-detection or specified method
upload() {
    local method=${1:-auto}

    case "$method" in
        usb)
            local usb_dev=$(find_usb_device)
            if [ -z "$usb_dev" ]; then
                error "No USB device found. Connect ESP32 via USB-C."
            fi
            upload_usb "$usb_dev"
            ;;
        uart)
            upload_uart
            ;;
        auto|*)
            # Prefer UART (always available on this board)
            if [ -e "$UART_DEV" ]; then
                log "Using UART for programming..."
                upload_uart
            else
                error "UART device $UART_DEV not found."
            fi
            ;;
    esac
}

# Serial monitor
monitor() {
    local dev=""

    # Prefer USB if available
    local usb_dev=$(find_usb_device)
    if [ -n "$usb_dev" ]; then
        dev="$usb_dev"
    else
        dev="$UART_DEV"
        reset_esp32
        sleep 0.5
    fi

    log "Starting serial monitor on $dev (Ctrl+C to exit)..."

    if command -v screen &> /dev/null; then
        screen "$dev" 115200
    elif command -v minicom &> /dev/null; then
        minicom -D "$dev" -b 115200
    else
        stty -F "$dev" 115200 raw -echo
        cat "$dev"
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
        upload "${2:-auto}"
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
        upload "${2:-auto}"
        log "Deployment complete!"
        ;;
esac

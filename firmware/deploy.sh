#!/bin/bash
#
# Deploy script for RPi Backup Power & GPS Hat v4 - ESP32 Firmware
#
# Usage:
#   ./deploy.sh          - Pull latest and upload to ESP32
#   ./deploy.sh build    - Pull latest and build only (no upload)
#   ./deploy.sh upload   - Upload without pulling
#   ./deploy.sh monitor  - Open serial monitor
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[DEPLOY]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Check for PlatformIO
check_pio() {
    if ! command -v pio &> /dev/null; then
        warn "PlatformIO not found. Installing..."
        pip3 install --user platformio
        export PATH="$HOME/.local/bin:$PATH"
    fi
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
    pio run
    log "Build complete."
}

# Upload to ESP32
upload() {
    log "Uploading to ESP32..."
    cd "$SCRIPT_DIR"

    # Find ESP32 device
    ESP_DEV=$(ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1)
    if [ -z "$ESP_DEV" ]; then
        error "No ESP32 device found. Check USB connection."
    fi

    log "Found device: $ESP_DEV"
    pio run -t upload --upload-port "$ESP_DEV"
    log "Upload complete."
}

# Serial monitor
monitor() {
    log "Starting serial monitor (Ctrl+C to exit)..."
    cd "$SCRIPT_DIR"
    pio device monitor
}

# Main
case "${1:-deploy}" in
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
        check_pio
        monitor
        ;;
    deploy|*)
        check_pio
        pull
        build
        upload
        log "Deployment complete!"
        ;;
esac

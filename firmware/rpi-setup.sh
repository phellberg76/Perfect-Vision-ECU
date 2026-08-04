#!/bin/bash
#
# One-time setup script for deploying ESP32 firmware from Raspberry Pi
#
# Run on your RPi:
#   ./rpi-setup.sh
#

set -e

GREEN='\033[0;32m'
NC='\033[0m'
log() { echo -e "${GREEN}[SETUP]${NC} $1"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV_DIR="$SCRIPT_DIR/.venv"

log "Installing system dependencies..."
sudo apt-get update
sudo apt-get install -y python3-pip python3-venv python3-full git

log "Creating Python virtual environment..."
python3 -m venv "$VENV_DIR"

log "Installing PlatformIO in venv..."
"$VENV_DIR/bin/pip" install --upgrade pip
"$VENV_DIR/bin/pip" install platformio

log "Installing ESP32 platform..."
"$VENV_DIR/bin/pio" pkg install -g -p espressif32

log ""
log "Setup complete!"
log ""
log "To deploy firmware to ESP32:"
log "  cd $SCRIPT_DIR"
log "  ./deploy.sh"
log ""
log "Commands:"
log "  ./deploy.sh         - Pull, build, and upload"
log "  ./deploy.sh build   - Pull and build only"
log "  ./deploy.sh upload  - Upload only"
log "  ./deploy.sh monitor - Serial monitor"
log ""

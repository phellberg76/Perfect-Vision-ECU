#!/bin/bash
#
# One-time setup script for deploying ESP32 firmware from Raspberry Pi
#
# Run this on your RPi:
#   curl -sL <raw-url-to-this-script> | bash
# Or clone the repo and run:
#   ./rpi-setup.sh
#

set -e

GREEN='\033[0;32m'
NC='\033[0m'
log() { echo -e "${GREEN}[SETUP]${NC} $1"; }

REPO_URL="https://github.com/phellberg76/Raspberry-Pi-Backup-power-and-GPS-hat-v4.git"
INSTALL_DIR="$HOME/gps-hat-firmware"

log "Installing dependencies..."
sudo apt-get update
sudo apt-get install -y python3-pip python3-venv git

log "Installing PlatformIO..."
pip3 install --user platformio
export PATH="$HOME/.local/bin:$PATH"

# Add to bashrc if not already there
if ! grep -q "/.local/bin" ~/.bashrc; then
    echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc
fi

log "Cloning repository..."
if [ -d "$INSTALL_DIR" ]; then
    log "Directory exists, pulling latest..."
    cd "$INSTALL_DIR"
    git pull origin master
else
    git clone "$REPO_URL" "$INSTALL_DIR"
fi

log "Installing PlatformIO ESP32 platform..."
pio pkg install -g -p espressif32

log ""
log "Setup complete!"
log ""
log "To deploy firmware to ESP32:"
log "  cd $INSTALL_DIR/firmware"
log "  ./deploy.sh"
log ""
log "Commands:"
log "  ./deploy.sh         - Pull, build, and upload"
log "  ./deploy.sh build   - Pull and build only"
log "  ./deploy.sh upload  - Upload only"
log "  ./deploy.sh monitor - Serial monitor"
log ""

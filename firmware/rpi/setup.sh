#!/bin/bash
#
# Setup script for RPi GPS Hat v4 - ESP32 SPI Communication
#
# This script:
# 1. Installs required Python packages
# 2. Compiles and installs the device tree overlay
# 3. Configures /boot/firmware/config.txt
# 4. Creates systemd service for GPS monitoring (optional)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OVERLAY_NAME="esp32-spi-slave"

echo "=== RPi GPS Hat v4 Setup ==="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root: sudo $0"
    exit 1
fi

# Install Python dependencies
echo "[1/4] Installing Python dependencies..."
apt-get update
apt-get install -y python3-pip python3-spidev python3-rpi.gpio device-tree-compiler

# Compile device tree overlay
echo "[2/4] Compiling device tree overlay..."
cd "$SCRIPT_DIR"
if [ -f "${OVERLAY_NAME}.dts" ]; then
    dtc -@ -I dts -O dtb -o "${OVERLAY_NAME}.dtbo" "${OVERLAY_NAME}.dts"
    cp "${OVERLAY_NAME}.dtbo" /boot/firmware/overlays/
    echo "    Overlay installed to /boot/firmware/overlays/${OVERLAY_NAME}.dtbo"
else
    echo "    Warning: ${OVERLAY_NAME}.dts not found, skipping overlay"
fi

# Update config.txt
echo "[3/4] Checking /boot/firmware/config.txt..."
CONFIG_FILE="/boot/firmware/config.txt"

# Ensure SPI is enabled
if ! grep -q "^dtparam=spi=on" "$CONFIG_FILE"; then
    echo "dtparam=spi=on" >> "$CONFIG_FILE"
    echo "    Added: dtparam=spi=on"
fi

# Add ESP32 SPI overlay if not present
if ! grep -q "^dtoverlay=${OVERLAY_NAME}" "$CONFIG_FILE"; then
    echo "dtoverlay=${OVERLAY_NAME}" >> "$CONFIG_FILE"
    echo "    Added: dtoverlay=${OVERLAY_NAME}"
fi

# Ensure UART is enabled for PPP/debug
if ! grep -q "^enable_uart=1" "$CONFIG_FILE"; then
    echo "enable_uart=1" >> "$CONFIG_FILE"
    echo "    Added: enable_uart=1"
fi

# Make esp32_spi.py executable
echo "[4/4] Setting up ESP32 control utility..."
chmod +x "$SCRIPT_DIR/esp32_spi.py"

# Create symlink in /usr/local/bin
ln -sf "$SCRIPT_DIR/esp32_spi.py" /usr/local/bin/esp32-hat

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Next steps:"
echo "1. Reboot the Raspberry Pi: sudo reboot"
echo "2. After reboot, test ESP32 communication:"
echo "   esp32-hat ping"
echo "   esp32-hat gps"
echo "   esp32-hat status"
echo ""
echo "To configure WiFi on the ESP32:"
echo "   esp32-hat connect --ssid 'YourNetwork' --password 'YourPassword'"
echo ""
echo "For PPP network sharing (after WiFi is connected):"
echo "   esp32-hat ppp_start"
echo "   # Then run pppd on /dev/ttyAMA0"
echo ""

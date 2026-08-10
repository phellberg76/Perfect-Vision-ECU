#!/bin/bash
#
# Setup HTTP Proxy Daemon for GPS Hat v4
#
# This installs the daemon that receives HTTP requests from ESP32
# and forwards them to localhost.
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="/opt/gps-hat"

echo "=== GPS Hat v4 HTTP Proxy Setup ==="
echo ""

# Install dependencies
echo "[1/4] Installing dependencies..."
apt-get update -qq
apt-get install -y -qq python3 python3-serial

# Create install directory
echo "[2/4] Installing daemon..."
mkdir -p "$INSTALL_DIR"
cp "$SCRIPT_DIR/http_proxy_daemon.py" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/http_proxy_daemon.py"

# Install systemd service
echo "[3/4] Installing systemd service..."
cp "$SCRIPT_DIR/http-proxy.service" /etc/systemd/system/
systemctl daemon-reload

# Enable and start service
echo "[4/4] Enabling service..."
systemctl enable http-proxy.service

echo ""
echo "=== Setup Complete ==="
echo ""
echo "The HTTP proxy daemon is now installed."
echo ""
echo "Commands:"
echo "  Start:   systemctl start http-proxy"
echo "  Stop:    systemctl stop http-proxy"
echo "  Status:  systemctl status http-proxy"
echo "  Logs:    journalctl -u http-proxy -f"
echo ""
echo "To start the ESP32 AP, send this command over UART:"
echo "  HTTP_PROXY_START"
echo ""
echo "Or with custom SSID/password:"
echo "  HTTP_PROXY_START MyNetwork MyPassword123"
echo ""

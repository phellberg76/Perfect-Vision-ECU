#!/bin/bash
#
# Setup HTTP Proxy and Web Interface for GPS Hat v4
#
# This installs:
# - HTTP proxy daemon (forwards requests from ESP32 to localhost)
# - Web interface (shows WiFi/cellular status, allows WiFi provisioning)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="/opt/gps-hat"

echo "=== GPS Hat v4 Setup ==="
echo ""

# Install dependencies
echo "[1/5] Installing dependencies..."
apt-get update -qq
apt-get install -y -qq python3 python3-serial wireless-tools

# Create install directory
echo "[2/5] Installing daemons..."
mkdir -p "$INSTALL_DIR"
cp "$SCRIPT_DIR/http_proxy_daemon.py" "$INSTALL_DIR/"
cp "$SCRIPT_DIR/web_interface.py" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/http_proxy_daemon.py"
chmod +x "$INSTALL_DIR/web_interface.py"

# Install systemd services
echo "[3/5] Installing systemd services..."
cp "$SCRIPT_DIR/http-proxy.service" /etc/systemd/system/
cp "$SCRIPT_DIR/gps-hat-web.service" /etc/systemd/system/
systemctl daemon-reload

# Enable services
echo "[4/5] Enabling services..."
systemctl enable http-proxy.service
systemctl enable gps-hat-web.service

# Start services
echo "[5/5] Starting services..."
systemctl start gps-hat-web.service
systemctl start http-proxy.service

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Services installed:"
echo "  - gps-hat-web  : Web interface on port 80"
echo "  - http-proxy   : ESP32 HTTP proxy daemon"
echo ""
echo "Commands:"
echo "  systemctl status gps-hat-web http-proxy"
echo "  journalctl -u gps-hat-web -f"
echo "  journalctl -u http-proxy -f"
echo ""
echo "To start the ESP32 AP, send this command over UART:"
echo "  echo 'HTTP_PROXY_START' > /dev/ttyAMA0"
echo ""
echo "Or with custom SSID/password:"
echo "  echo 'HTTP_PROXY_START MyNetwork MyPassword123' > /dev/ttyAMA0"
echo ""
echo "Then connect to the ESP32 AP and browse to http://192.168.4.1/"
echo ""

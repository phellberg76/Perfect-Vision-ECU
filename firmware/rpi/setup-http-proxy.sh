#!/bin/bash
#
# Setup HTTP Proxy and Web Interface for GPS Hat v4
#
# This installs:
# - nginx (reverse proxy on port 80)
# - HTTP proxy daemon (forwards requests from ESP32 to nginx)
# - Web interface (network status and WiFi config)
#
# Architecture:
#   Phone -> ESP32 AP (port 80) -> UART -> http-proxy -> nginx (port 80)
#                                                          |
#                                          +---------------+---------------+
#                                          |                               |
#                                   /planter_app/*                        /*
#                                          |                               |
#                                  planter app:8080              web_interface:8081
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="/opt/gps-hat"

echo "=== GPS Hat v4 Setup ==="
echo ""

# Install dependencies
echo "[1/6] Installing dependencies..."
apt-get update -qq
apt-get install -y -qq python3 python3-serial wireless-tools nginx

# Create install directory
echo "[2/6] Installing daemons..."
mkdir -p "$INSTALL_DIR"
cp "$SCRIPT_DIR/http_proxy_daemon.py" "$INSTALL_DIR/"
cp "$SCRIPT_DIR/web_interface.py" "$INSTALL_DIR/"
chmod +x "$INSTALL_DIR/http_proxy_daemon.py"
chmod +x "$INSTALL_DIR/web_interface.py"

# Configure nginx
echo "[3/6] Configuring nginx..."
cp "$SCRIPT_DIR/nginx-gps-hat.conf" /etc/nginx/sites-available/gps-hat
ln -sf /etc/nginx/sites-available/gps-hat /etc/nginx/sites-enabled/
rm -f /etc/nginx/sites-enabled/default
nginx -t

# Install systemd services
echo "[4/6] Installing systemd services..."
cp "$SCRIPT_DIR/http-proxy.service" /etc/systemd/system/
cp "$SCRIPT_DIR/gps-hat-web.service" /etc/systemd/system/
systemctl daemon-reload

# Enable services
echo "[5/6] Enabling services..."
systemctl enable nginx
systemctl enable http-proxy.service
systemctl enable gps-hat-web.service

# Start services
echo "[6/6] Starting services..."
systemctl restart nginx
systemctl restart gps-hat-web.service
systemctl restart http-proxy.service

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Architecture:"
echo "  ESP32 AP (port 80) -> http-proxy -> nginx (port 80)"
echo "                                        |"
echo "                         +--------------+--------------+"
echo "                         |                             |"
echo "                  /planter_app/*                      /*"
echo "                         |                             |"
echo "                 planter app:8080            web_interface:8081"
echo ""
echo "Services:"
echo "  nginx          : Reverse proxy on port 80"
echo "  gps-hat-web    : Web interface on port 8081"
echo "  http-proxy     : ESP32 UART proxy"
echo ""
echo "Commands:"
echo "  systemctl status nginx gps-hat-web http-proxy"
echo "  journalctl -u http-proxy -f"
echo ""

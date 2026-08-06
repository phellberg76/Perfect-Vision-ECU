#!/bin/bash
#
# RPi AP+STA Hotspot Setup
#
# Configures the RPi to run WiFi in concurrent mode:
#   - wlan0: Client mode (connects to upstream WiFi for internet)
#   - uap0: AP mode (hotspot for tablet/laptop)
#
# Usage:
#   sudo ./setup-hotspot.sh
#
# After setup, edit /etc/hostapd/hostapd.conf to change SSID/password
#

set -e

# Configuration
AP_INTERFACE="uap0"
AP_IP="192.168.4.1"
AP_NETMASK="255.255.255.0"
DHCP_RANGE_START="192.168.4.10"
DHCP_RANGE_END="192.168.4.50"
AP_SSID="Implement"
AP_PASSWORD="implement123"
AP_CHANNEL="6"

echo "=== RPi AP+STA Hotspot Setup ==="
echo ""

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    echo "Please run as root: sudo $0"
    exit 1
fi

# Install required packages
echo "[1/6] Installing hostapd and dnsmasq..."
apt-get update
apt-get install -y hostapd dnsmasq

# Stop services during configuration
systemctl stop hostapd 2>/dev/null || true
systemctl stop dnsmasq 2>/dev/null || true

# Create udev rule to create virtual AP interface
echo "[2/6] Creating udev rule for virtual AP interface..."
cat > /etc/udev/rules.d/90-wireless-ap.rules << 'EOF'
# Create virtual AP interface when wlan0 comes up
ACTION=="add", SUBSYSTEM=="net", KERNEL=="wlan0", RUN+="/sbin/iw dev wlan0 interface add uap0 type __ap"
EOF

# Create the interface now if wlan0 exists
if ip link show wlan0 &>/dev/null; then
    echo "Creating $AP_INTERFACE interface..."
    iw dev wlan0 interface add $AP_INTERFACE type __ap 2>/dev/null || true
fi

# Configure hostapd
echo "[3/6] Configuring hostapd..."
cat > /etc/hostapd/hostapd.conf << EOF
# AP Interface
interface=$AP_INTERFACE

# Driver (nl80211 for most modern WiFi chips)
driver=nl80211

# Network name (SSID)
ssid=$AP_SSID

# WiFi channel (must match or be compatible with wlan0's channel)
channel=$AP_CHANNEL

# 802.11n support
hw_mode=g
ieee80211n=1
wmm_enabled=1

# Security
wpa=2
wpa_passphrase=$AP_PASSWORD
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP

# Other settings
country_code=ZA
auth_algs=1
ignore_broadcast_ssid=0
EOF

# Point hostapd to config file
sed -i 's|^#DAEMON_CONF=.*|DAEMON_CONF="/etc/hostapd/hostapd.conf"|' /etc/default/hostapd
echo 'DAEMON_CONF="/etc/hostapd/hostapd.conf"' > /etc/default/hostapd

# Configure dnsmasq
echo "[4/6] Configuring dnsmasq..."

# Backup original dnsmasq.conf
if [ -f /etc/dnsmasq.conf ] && [ ! -f /etc/dnsmasq.conf.orig ]; then
    cp /etc/dnsmasq.conf /etc/dnsmasq.conf.orig
fi

cat > /etc/dnsmasq.conf << EOF
# AP interface only
interface=$AP_INTERFACE

# Don't use /etc/resolv.conf
no-resolv

# Don't forward queries without domain
domain-needed
bogus-priv

# DHCP range for AP clients
dhcp-range=$DHCP_RANGE_START,$DHCP_RANGE_END,12h

# Set default gateway to RPi
dhcp-option=3,$AP_IP

# Set DNS to RPi (which forwards to upstream)
dhcp-option=6,$AP_IP

# Bind only to AP interface
bind-interfaces
except-interface=lo
except-interface=wlan0
except-interface=eth0
EOF

# Configure network interface
echo "[5/6] Configuring network interface..."

# Create systemd service to configure AP interface on boot
cat > /etc/systemd/system/ap-interface.service << EOF
[Unit]
Description=Configure AP Interface
After=network.target
Before=hostapd.service dnsmasq.service

[Service]
Type=oneshot
RemainAfterExit=yes
ExecStart=/bin/bash -c 'iw dev wlan0 interface add $AP_INTERFACE type __ap 2>/dev/null || true; ip addr add $AP_IP/24 dev $AP_INTERFACE 2>/dev/null || true; ip link set $AP_INTERFACE up'

[Install]
WantedBy=multi-user.target
EOF

# Enable IP forwarding
echo "[6/6] Enabling IP forwarding..."
sed -i 's/#net.ipv4.ip_forward=1/net.ipv4.ip_forward=1/' /etc/sysctl.conf
sysctl -w net.ipv4.ip_forward=1

# Set up NAT (optional - only if AP clients need internet access)
# Uncomment these lines if you want AP clients to access internet via wlan0
# iptables -t nat -A POSTROUTING -o wlan0 -j MASQUERADE
# iptables -A FORWARD -i uap0 -o wlan0 -j ACCEPT
# iptables -A FORWARD -i wlan0 -o uap0 -m state --state RELATED,ESTABLISHED -j ACCEPT

# Unmask and enable services
systemctl unmask hostapd
systemctl enable ap-interface
systemctl enable hostapd
systemctl enable dnsmasq

# Configure interface now
echo ""
echo "Configuring interface now..."
ip addr add $AP_IP/24 dev $AP_INTERFACE 2>/dev/null || true
ip link set $AP_INTERFACE up 2>/dev/null || true

# Start services
systemctl start ap-interface
sleep 1
systemctl start hostapd
systemctl start dnsmasq

echo ""
echo "=== Setup Complete ==="
echo ""
echo "Hotspot Configuration:"
echo "  SSID:     $AP_SSID"
echo "  Password: $AP_PASSWORD"
echo "  IP:       $AP_IP"
echo "  DHCP:     $DHCP_RANGE_START - $DHCP_RANGE_END"
echo ""
echo "Edit /etc/hostapd/hostapd.conf to change SSID/password"
echo ""
echo "Connect your tablet/laptop to '$AP_SSID' and access:"
echo "  - Implement app: http://$AP_IP:8080 (or your app port)"
echo "  - SSH: ssh user@$AP_IP"
echo ""
echo "Note: If wlan0 connects to a different channel, you may need to"
echo "      update the channel in /etc/hostapd/hostapd.conf to match."

#!/bin/bash
#
# Check hotspot status
#

echo "=== Hotspot Status ==="
echo ""

echo "Interfaces:"
ip addr show uap0 2>/dev/null || echo "  uap0: NOT FOUND"
echo ""

echo "Services:"
echo -n "  hostapd:  "; systemctl is-active hostapd
echo -n "  dnsmasq:  "; systemctl is-active dnsmasq
echo ""

echo "Connected clients (DHCP leases):"
cat /var/lib/misc/dnsmasq.leases 2>/dev/null || echo "  No leases file"
echo ""

echo "Hostapd connected stations:"
hostapd_cli all_sta 2>/dev/null || echo "  hostapd_cli not available or not running"

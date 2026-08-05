# RPi GPS Hat v4 - Unified ESP32 Firmware

This firmware provides GPS + WiFi + CAN functionality in a single program.

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                         Raspberry Pi                            │
├─────────────────────────────────────────────────────────────────┤
│  wlan0 (hostapd)     ppp0 (internet)      can0/can1 (CAN bus)  │
│        ↓                   ↓                     ↓              │
│    Hotspot for        Via PPP over        MCP2515 controllers   │
│    other devices      /dev/ttyAMA0        on SPI CE0/CE1        │
└────────────────────────────┬────────────────────────────────────┘
                             │
                    SPI (GPIO4=CS) + UART
                             │
┌────────────────────────────┴────────────────────────────────────┐
│                        ESP32-S3-MINI-1                          │
├─────────────────────────────────────────────────────────────────┤
│  SPI Slave           PPP Server            WiFi STA             │
│  (GPS/Status)        (UART0)               (Internet)           │
│       ↑                                         ↑               │
│       │                                         │               │
│   UART1 (38400)                          Home Router            │
│       ↑                                                         │
│   NEO-M9N GPS                                                   │
└─────────────────────────────────────────────────────────────────┘
```

## Communication Channels

| Interface | Purpose | Details |
|-----------|---------|---------|
| SPI Slave | GPS data, Status, WiFi control | GPIO4 as CS, shared bus with CAN |
| UART0 | PPP network link | 115200 baud to RPi /dev/ttyAMA0 |
| UART1 | GPS module | 38400 baud to NEO-M9N |
| WiFi | Internet upstream | Station mode, connects to router |

## Features

### GPS
- Reads NMEA from NEO-M9N at 38400 baud
- Parses with TinyGPS++ library
- Provides JSON via SPI: position, altitude, speed, course, satellites, HDOP

### WiFi
- Connects as client (station mode) to configured network
- Credentials stored in ESP32 NVS flash
- Status includes: connected, SSID, IP, RSSI

### PPP (Network Sharing)
- PPP server on UART0 provides internet to RPi
- NAT/routing between WiFi and PPP interfaces
- RPi sees `ppp0` interface with internet access

### SPI Slave
- Shares SPI bus with MCP2515 CAN controllers
- GPIO4 used as chip select (directly controlled)
- Commands for GPS, status, WiFi configuration

## Building the Firmware

```bash
cd firmware

# Build
pio run

# Upload via USB
pio run -t upload -e esp32s3-usb

# Upload via RPi UART (before PPP mode active)
pio run -t upload
```

## RPi Setup

```bash
cd firmware/rpi
sudo ./setup.sh
sudo reboot
```

## Usage

### Command Line

```bash
# Test communication
esp32-hat ping

# Get GPS position
esp32-hat gps

# Get overall status
esp32-hat status

# Get WiFi status
esp32-hat wifi

# Configure and connect WiFi
esp32-hat connect --ssid "MyNetwork" --password "MyPassword"

# Start PPP mode for network sharing
esp32-hat ppp_start
```

### Python Library

```python
from esp32_spi import ESP32Hat

hat = ESP32Hat()

# Check connectivity
if hat.ping():
    print("ESP32 responding")

# Get GPS data
gps = hat.get_gps()
if gps and gps.get('fix'):
    print(f"Position: {gps['lat']}, {gps['lng']}")
    print(f"Speed: {gps['speed']} m/s")
    print(f"Satellites: {gps['sats']}")

# Get status
status = hat.get_status()
print(f"Uptime: {status['uptime']}s")
print(f"WiFi: {status['wifi']}")
print(f"PPP: {status['ppp']}")

# Configure WiFi
hat.configure_wifi("MyNetwork", "MyPassword")

# Check WiFi status
wifi = hat.get_wifi_status()
if wifi['connected']:
    print(f"IP: {wifi['ip']}")
```

### PPP Network Setup

After configuring WiFi and starting PPP mode:

```bash
# On RPi, start pppd
sudo pppd /dev/ttyAMA0 115200 \
    10.0.0.2:10.0.0.1 \
    noauth \
    local \
    nocrtscts \
    persist \
    defaultroute

# Verify ppp0 interface
ip addr show ppp0

# Test connectivity
ping -I ppp0 8.8.8.8

# Enable NAT for RPi's hotspot clients
sudo iptables -t nat -A POSTROUTING -o ppp0 -j MASQUERADE
sudo sysctl -w net.ipv4.ip_forward=1
```

## SPI Protocol

Commands sent to ESP32 SPI slave:

| Byte | Command | Response |
|------|---------|----------|
| 0x01 | Get GPS | JSON: `{"type":"gps","lat":...}` |
| 0x02 | Get Status | JSON: `{"type":"status","uptime":...}` |
| 0x03 | Get WiFi | JSON: `{"type":"wifi","connected":...}` |
| 0x10 | Set SSID | + null-terminated string |
| 0x11 | Set Password | + null-terminated string |
| 0x12 | Connect WiFi | Initiates connection |
| 0x13 | Disconnect WiFi | Disconnects |
| 0x20 | Start PPP | Enables PPP mode on UART0 |
| 0x21 | Stop PPP | Disables PPP mode |
| 0xFE | Ping | Returns 0xFE |
| 0xFF | Reset | Reboots ESP32 |

## Serial Commands (Before PPP Mode)

Commands can also be sent via UART0 before PPP is started:

```
STATUS          → Returns status JSON
GPS             → Returns GPS JSON
WIFI            → Returns WiFi JSON
WIFI_SSID xxx   → Set WiFi SSID
WIFI_PASS xxx   → Set WiFi password
WIFI_CONNECT    → Connect to WiFi
PPP_START       → Start PPP mode
RESET           → Reboot ESP32
```

## Hardware Pin Mapping

### ESP32-S3-MINI-1

| Function | ESP32 GPIO | Connected To |
|----------|------------|--------------|
| SPI MOSI | IO11 | RPi GPIO10 (MOSI) |
| SPI MISO | IO13 | RPi GPIO9 (MISO) |
| SPI SCK | IO12 | RPi GPIO11 (SCLK) |
| SPI CS | IO10 | RPi GPIO4 |
| Handshake | IO3 | RPi GPIO5 |
| Data Ready | IO4 | RPi GPIO6 |
| GPS TX | IO17 | NEO-M9N RX |
| GPS RX | IO18 | NEO-M9N TX |
| GPS LNA EN | IO8 | MAX2659 SHDN |
| PPP TX | IO43 | RPi RX (ttyAMA0) |
| PPP RX | IO44 | RPi TX (ttyAMA0) |

### Raspberry Pi SPI Bus

| Device | CS Pin | RPi GPIO |
|--------|--------|----------|
| MCP2515 CAN0 | CE0 | GPIO8 |
| MCP2515 CAN1 | CE1 | GPIO7 |
| ESP32 | CE2 | GPIO4 |

## Troubleshooting

### ESP32 not responding on SPI
- Check GPIO4 is configured as output
- Verify device tree overlay is loaded: `dtoverlay -l`
- Check `/dev/spidev0.2` exists

### WiFi not connecting
- Verify credentials via SPI or serial
- Check router is in range
- Use `esp32-hat wifi` to see status

### PPP connection fails
- Ensure WiFi is connected first
- Check pppd options match ESP32 config
- Monitor with `journalctl -f -u ppp`

### GPS no fix
- Verify LNA is powered (IO8 high)
- Check antenna connection
- Move to area with sky view
- Allow 30+ seconds for cold start

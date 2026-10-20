# Perfect Vision ECU Firmware

CAN/BLE bridge firmware for the Perfect Vision ECU, based on ESP32-S3.

## Features

- Dual CAN channel support:
  - **CAN0**: ESP32-S3 internal TWAI controller
  - **CAN1**: MCP2515 external SPI CAN controller
- BLE Nordic UART Service (NUS) for wireless CAN data streaming
- Device advertises as `PerfectVision_<MAC>` (last 3 bytes of Bluetooth MAC)
- Buffered message queue to prevent data loss
- **WiFi OTA firmware updates** triggered via BLE, downloaded from cloud server
- Automatic rollback protection for failed OTA updates
- JSON command protocol for app communication

## TODO

- [ ] **GPS Integration**: Read GPS location and speed from the onboard GPS module
  (connected via UART0 - GPIO43/GPIO44) and stream alongside CAN data. This is
  needed for standalone operation or when the ISOBUS terminal lacks GPS capability.

## Hardware Connections

Based on the Perfect-Vision-ECU schematic:

| Function | GPIO | Notes |
|----------|------|-------|
| CAN0 TX | GPIO4 | To SN65HVD230 transceiver |
| CAN0 RX | GPIO5 | From SN65HVD230 transceiver |
| MCP2515 CS | GPIO10 | SPI Chip Select |
| MCP2515 MOSI | GPIO11 | SPI Data Out |
| MCP2515 CLK | GPIO12 | SPI Clock |
| MCP2515 MISO | GPIO13 | SPI Data In |
| MCP2515 INT | GPIO9 | Interrupt (active low) |
| Relay Control | GPIO8 | CAN channel switching relay |

## Building

### Prerequisites

- ESP-IDF v5.x installed
- ESP32-S3 target configured

### Build Commands

```bash
# Set target (only needed once)
idf.py set-target esp32s3

# Configure (optional)
idf.py menuconfig

# Build
idf.py build

# Flash
idf.py -p /dev/ttyUSB0 flash

# Monitor serial output
idf.py -p /dev/ttyUSB0 monitor
```

## BLE Protocol

### Nordic UART Service (NUS)

| UUID | Description |
|------|-------------|
| `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | Service UUID |
| `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | RX Characteristic (App → ESP32, Write) |
| `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | TX Characteristic (ESP32 → App, Notify) |

### JSON Commands (App → ESP32)

All commands are JSON objects with a `cmd` field, terminated by newline.

#### Get Version
```json
{"cmd":"version"}
```
Response:
```json
{"type":"version","version":"1.0.0","model":"perfect-vision","hash":"abc123..."}
```

#### Get Status
```json
{"cmd":"status"}
```
Response:
```json
{"type":"status","can_running":true,"can0_rx":1234,"can0_err":0,"can1_rx":567,"can1_err":0,"buffer_count":10,"buffer_overflow":0,"total_messages":1801}
```

#### CAN Control
```json
{"cmd":"can_start"}
{"cmd":"can_stop"}
{"cmd":"clear_buffer"}
```

#### WiFi Scan
```json
{"cmd":"wifi_scan"}
```
Response:
```json
{"type":"wifi_scan_result","success":true,"count":3,"networks":[{"ssid":"MyWiFi","rssi":-45,"secured":true},...]}
```

#### WiFi OTA Update
```json
{
  "cmd":"wifi_ota",
  "ssid":"MyWiFi",
  "password":"secret123",
  "url":"https://your-server.com/firmware.bin",
  "token":"optional_bearer_token",
  "size":123456,
  "hash":"sha256_hash_optional"
}
```
Progress notifications:
```json
{"type":"wifi_ota_progress","status":"downloading","progress":50,"received":61728,"total":123456,"message":"Downloading 60 / 120 KB"}
```

Status values: `connecting_wifi`, `wifi_connected`, `downloading`, `validating`, `complete`, `error`

#### Abort OTA
```json
{"cmd":"wifi_ota_abort"}
```

### CAN Data Format (Binary, 16 bytes per message)

CAN messages are streamed as binary notifications:

```
Offset  Size  Field
0       1     Channel (0=CAN0, 1=CAN1)
1       4     Timestamp (ms since boot, little-endian)
5       4     CAN ID (little-endian)
9       1     Flags (bit0=Extended ID, bit1=RTR)
10      1     DLC (0-8)
11-18   8     Data bytes (padded with zeros if DLC < 8)
```

## OTA Update Flow

1. **Flutter app** checks current firmware version via `{"cmd":"version"}`
2. **App** compares with server version and gets download URL
3. **App** optionally scans WiFi networks via `{"cmd":"wifi_scan"}`
4. **User** selects WiFi network in app
5. **App** sends `wifi_ota` command with WiFi credentials and firmware URL
6. **ESP32** connects to WiFi, downloads firmware via HTTPS
7. **ESP32** validates and installs firmware
8. **ESP32** reboots with new firmware
9. On first boot, firmware is marked as valid (rollback protection)

## Partition Table

| Name | Type | Size | Description |
|------|------|------|-------------|
| nvs | data | 24 KB | Non-volatile storage |
| phy_init | data | 4 KB | WiFi/BT calibration |
| otadata | data | 8 KB | OTA metadata |
| ota_0 | app | 1920 KB | Firmware slot 0 |
| ota_1 | app | 1920 KB | Firmware slot 1 |

## Error Codes

| Code | Description |
|------|-------------|
| 1 | Invalid JSON |
| 2 | Missing cmd field |
| 3 | Unknown command |
| 40 | Failed to start CAN |
| 50 | OTA in progress (cannot scan) |
| 51 | Scan already in progress |
| 52 | Failed to start WiFi scan |
| 60 | OTA already in progress |
| 61 | Missing ssid or url |
| 62 | Failed to start OTA |
| 63 | No OTA in progress (abort) |

## License

Proprietary - Perfect Precision Hardware

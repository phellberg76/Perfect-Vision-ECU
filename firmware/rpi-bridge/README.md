# Planter BLE Bridge

BLE to HTTP bridge for the Perfect Precision Planter App. Acts as a BLE peripheral (GATT server) that the Flutter app connects to, and proxies requests to the Java backend on localhost.

## Architecture

```
Flutter App <──BLE GATT──> This Bridge <──HTTP──> Java Backend (localhost:8080)
```

## GATT Service

- **Service UUID:** `a1b2c3d4-e5f6-7890-abcd-ef1234567890`
- **Request Characteristic:** `a1b2c3d4-e5f6-7890-abcd-ef1234567891` (Write)
- **Response Characteristic:** `a1b2c3d4-e5f6-7890-abcd-ef1234567892` (Notify)

## Protocol

Messages are JSON with newline delimiters.

### Request from App

```json
{"type":"http","id":1,"method":"GET","path":"/getSeedSummaryData"}
```

### Response to App

```json
{"type":"http","id":1,"status":200,"body":"{...}"}
```

### File Download (for large files)

Request:
```json
{"type":"file_download","id":2,"path":"/planter_app/videos/splash.mp4"}
```

Response (metadata + chunks):
```json
{"type":"file_meta","id":2,"size":12700000,"total_chunks":388,"content_type":"video/mp4","filename":"splash.mp4"}
{"type":"file_chunk","id":2,"chunk_index":0,"total_chunks":388,"data":"base64...","is_last":false}
...
{"type":"file_chunk","id":2,"chunk_index":387,"total_chunks":388,"data":"base64...","is_last":true}
```

## Usage

```bash
# Run with defaults (name: PerfectPrecisionPlanter, backend: http://localhost:8080)
./planter-ble-bridge

# Custom options
./planter-ble-bridge --name "MyPlanter" --backend "http://localhost:9000" --debug
```

## Building

```bash
cargo build --release
```

Binary will be at `target/release/planter-ble-bridge` (~3.5MB).

## Requirements

- Linux with BlueZ 5.x
- Bluetooth adapter supporting BLE
- D-Bus

## Running as a Service

Create `/etc/systemd/system/planter-ble-bridge.service`:

```ini
[Unit]
Description=Planter BLE Bridge
After=bluetooth.target

[Service]
Type=simple
ExecStart=/usr/local/bin/planter-ble-bridge --name PerfectPrecisionPlanter --backend http://localhost:8080
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Enable and start:
```bash
sudo cp target/release/planter-ble-bridge /usr/local/bin/
sudo systemctl enable planter-ble-bridge
sudo systemctl start planter-ble-bridge
```

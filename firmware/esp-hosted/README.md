# ESP-Hosted WiFi Coprocessor Setup

This configures the ESP32-S3 as a WiFi/Bluetooth coprocessor for the Raspberry Pi
using ESP-Hosted-FG over SPI.

## Hardware Pin Mapping

| Signal | RPi GPIO | ESP32-S3 GPIO |
|--------|----------|---------------|
| CS | GPIO4 | IO10 |
| MOSI | GPIO10 | IO11 |
| MISO | GPIO9 | IO13 |
| SCLK | GPIO11 | IO12 |
| Handshake | GPIO5 | IO3 |
| Data Ready | GPIO6 | IO4 |
| Reset | GPIO17 | EN |

## Important Note

When using ESP-Hosted mode, the ESP32 operates as an SPI slave and:
- GPS functionality is **disabled** (UART1 not available in this mode)
- CAN controllers share the SPI bus - may need coordination

Consider running in either:
- **GPS Mode**: Current firmware with GPS + UART communication
- **WiFi Mode**: ESP-Hosted firmware for network connectivity

## Setup Instructions

### 1. Build ESP-Hosted Slave Firmware

On a development machine with ESP-IDF installed:

```bash
# Clone ESP-Hosted
git clone --recurse-submodules https://github.com/espressif/esp-hosted.git
cd esp-hosted/esp_hosted_fg/esp/esp_driver/network_adapter

# Set target
idf.py set-target esp32s3

# Configure
idf.py menuconfig
```

In menuconfig, navigate to:
- `Example Configuration` → `Transport layer` → Select `SPI`
- `Example Configuration` → `SPI Configuration`:
  - SPI Mode: `Mode 2`
  - SPI GPIO Handshake: `3` (change from default 2)
  - SPI GPIO Data Ready: `4`
  - SPI MOSI GPIO: `11`
  - SPI MISO GPIO: `13`
  - SPI CLK GPIO: `12`

```bash
# Build and flash
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

### 2. Install Raspberry Pi Driver

```bash
# Clone ESP-Hosted on the RPi
cd ~
git clone --recurse-submodules https://github.com/espressif/esp-hosted.git
cd esp-hosted/esp_hosted_fg/host/linux/host_driver/esp32

# Build kernel module
make

# Load module
sudo insmod esp32_spi.ko \
    resetpin=17 \
    handshake=5 \
    dataready=6 \
    spi_bus=0 \
    spi_cs=0 \
    spi_mode=2 \
    spi_clock=10000000
```

### 3. Device Tree Overlay (Alternative)

Compile and install the overlay:

```bash
cd /path/to/firmware/esp-hosted
dtc -@ -I dts -O dtb -o esp-hosted-spi.dtbo esp-hosted-spi-overlay.dts
sudo cp esp-hosted-spi.dtbo /boot/firmware/overlays/
```

Add to `/boot/firmware/config.txt`:
```
dtoverlay=esp-hosted-spi
```

### 4. Network Configuration

After the driver loads, you should see an `espsta0` interface:

```bash
# Check interface
ip link show espsta0

# Connect to WiFi
sudo wpa_supplicant -i espsta0 -c /etc/wpa_supplicant/wpa_supplicant.conf &
sudo dhclient espsta0
```

## Switching Between Modes

### To use GPS Mode:
Flash the GPS firmware from `../src/main.cpp`

### To use WiFi Mode:
Flash ESP-Hosted firmware as described above

## Troubleshooting

1. **No espsta0 interface**: Check `dmesg` for driver errors
2. **SPI errors**: Verify wiring and pull-ups on CS line
3. **Reset not working**: Ensure GPIO17 is correctly connected to ESP32 EN pin

## References

- [ESP-Hosted GitHub](https://github.com/espressif/esp-hosted)
- [ESP-Hosted-FG Documentation](https://github.com/espressif/esp-hosted/tree/master/esp_hosted_fg/docs)

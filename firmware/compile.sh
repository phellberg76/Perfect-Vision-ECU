#!/bin/bash
#
# Compile script for Raspberry Pi Backup Power and GPS Hat v4 firmware (cloud server)
# Uses PlatformIO - compiles only, flashing is done on the Pi
#
# Usage:
#   ./compile.sh           - Build default environment (esp32s3)
#   ./compile.sh esp32s3-usb - Build USB environment
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV="${1:-esp32s3}"

echo "=== Raspberry Pi Backup Power and GPS Hat v4 Compile Script (Cloud) ==="
echo "Environment: $ENV"
echo ""

# Check for PlatformIO
if ! command -v pio &> /dev/null; then
    echo "PlatformIO not found. Installing..."
    pip install platformio
fi

# Compile
cd "$SCRIPT_DIR"
echo "Compiling firmware..."
pio run -e "$ENV"

# Find the output binary
BUILD_DIR="$SCRIPT_DIR/.pio/build/$ENV"
FIRMWARE_BIN="$BUILD_DIR/firmware.bin"

if [ -f "$FIRMWARE_BIN" ]; then
    echo ""
    echo "Build complete!"
    echo "  Binary: $FIRMWARE_BIN"
    echo "  Size: $(du -h "$FIRMWARE_BIN" | cut -f1)"
    echo ""
    echo "To flash from Pi:"
    echo "  esptool --chip esp32s3 --port /dev/ttyAMA0 write-flash 0x0 firmware.bin"
else
    echo ""
    echo "Build complete! Output in: $BUILD_DIR/"
    ls -la "$BUILD_DIR/"*.bin 2>/dev/null || true
fi

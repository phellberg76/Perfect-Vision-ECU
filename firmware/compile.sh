#!/bin/bash
# Perfect Vision ECU - Server-Side Compilation Script
#
# This script is designed for automated/server-side firmware builds.
# It builds the firmware, computes SHA256 hash, and prepares output for OTA distribution.
#
# Version is automatically extracted from main/include/config.h (FIRMWARE_VERSION define).
#
# Usage:
#   ./compile.sh                    # Build firmware (version from config.h)
#   ./compile.sh --clean            # Clean and rebuild
#   ./compile.sh --version 1.2.3    # Override version (optional)
#   ./compile.sh --output /path     # Custom output directory
#
# Output:
#   - perfect-vision-<version>.bin    (firmware binary)
#   - perfect-vision-<version>.md5    (SHA256 hash file)
#   - perfect-vision-<version>.json   (metadata for server API)
#
# Environment:
#   IDF_PATH    - Path to ESP-IDF (default: ~/esp/esp-idf)

set -e

# Defaults
VERSION=""  # Will be extracted from config.h if not specified
OUTPUT_DIR="./dist"
CLEAN_BUILD=false
OUTPUT_JSON=true  # Always generate JSON for server to read
PROJECT_NAME="perfect-vision"
CONFIG_FILE="main/include/config.h"

# Extract version from config.h
extract_version() {
    if [ -f "$CONFIG_FILE" ]; then
        # Extract FIRMWARE_VERSION value from #define FIRMWARE_VERSION "x.y.z"
        local ver=$(grep -E '^\s*#define\s+FIRMWARE_VERSION\s+"[^"]*"' "$CONFIG_FILE" | \
                    sed 's/.*"\([^"]*\)".*/\1/')
        if [ -n "$ver" ]; then
            echo "$ver"
            return 0
        fi
    fi
    echo "dev"
    return 1
}

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_status() {
    echo -e "${GREEN}[BUILD]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --clean)
            CLEAN_BUILD=true
            shift
            ;;
        --version)
            VERSION="$2"
            shift 2
            ;;
        --output)
            OUTPUT_DIR="$2"
            shift 2
            ;;
        --help|-h)
            echo "Perfect Vision ECU - Server-Side Compilation Script"
            echo ""
            echo "Usage: $0 [options]"
            echo ""
            echo "Options:"
            echo "  --clean           Clean build directory before building"
            echo "  --version VER     Override firmware version (default: from config.h)"
            echo "  --output DIR      Output directory (default: ./dist)"
            echo "  --help, -h        Show this help message"
            echo ""
            echo "Environment variables:"
            echo "  IDF_PATH          Path to ESP-IDF installation"
            echo ""
            echo "The firmware version is automatically extracted from main/include/config.h"
            echo "(FIRMWARE_VERSION define). Use --version to override if needed."
            echo ""
            echo "Example:"
            echo "  $0 --clean"
            echo "  $0 --output /var/www/perfect_vision_cloud/firmware"
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            exit 1
            ;;
    esac
done

# ESP-IDF installation paths to try
IDF_PATHS=(
    "$IDF_PATH"
    "$HOME/esp/esp-idf"
    "/opt/esp-idf"
    "/opt/esp/idf"
    "/usr/local/esp-idf"
)

# Find and source ESP-IDF
setup_idf() {
    # If IDF_PATH is already set and valid, use it
    if [ -n "$IDF_PATH" ] && [ -f "$IDF_PATH/export.sh" ]; then
        print_status "Using ESP-IDF at: $IDF_PATH"
        source "$IDF_PATH/export.sh" > /dev/null 2>&1
        return 0
    fi

    # Try to find ESP-IDF
    for path in "${IDF_PATHS[@]}"; do
        if [ -n "$path" ] && [ -f "$path/export.sh" ]; then
            print_status "Found ESP-IDF at: $path"
            export IDF_PATH="$path"
            source "$path/export.sh" > /dev/null 2>&1
            return 0
        fi
    done

    print_error "ESP-IDF not found!"
    echo ""
    echo "Please install ESP-IDF or set IDF_PATH environment variable."
    echo "Installation: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/"
    exit 1
}

# Set target if needed
set_target() {
    if [ ! -f "sdkconfig" ]; then
        print_status "Setting target to ESP32-S3..."
        # Clean any corrupted build directory first
        rm -rf build
        idf.py set-target esp32s3
    fi
}

# Clean build directory
clean_build() {
    if [ "$CLEAN_BUILD" = true ]; then
        print_status "Cleaning build directory..."
        # Force remove build dir and sdkconfig to handle corrupted builds
        rm -rf build sdkconfig
    fi
}

# Build the firmware
build_firmware() {
    print_status "Building firmware..."

    # Version is defined in config.h - no need to override via EXTRA_CFLAGS
    idf.py build

    # Check if build succeeded
    if [ ! -f "build/perfect_vision_ecu.bin" ]; then
        print_error "Build failed - firmware binary not found"
        exit 1
    fi

    print_status "Build complete!"
}

# Prepare output
prepare_output() {
    print_status "Preparing output..."

    # Create output directory
    mkdir -p "$OUTPUT_DIR"

    # Source and destination paths
    SRC_BIN="build/perfect_vision_ecu.bin"
    DST_BIN="$OUTPUT_DIR/${PROJECT_NAME}-${VERSION}.bin"
    DST_MD5="$OUTPUT_DIR/${PROJECT_NAME}-${VERSION}.md5"
    DST_JSON="$OUTPUT_DIR/${PROJECT_NAME}-${VERSION}.json"

    # Also create a "latest" symlink/copy for easy access
    LATEST_BIN="$OUTPUT_DIR/perfect_vision_ecu.bin"

    # Copy binary
    cp "$SRC_BIN" "$DST_BIN"
    cp "$SRC_BIN" "$LATEST_BIN"

    # Compute SHA256 (ESP32 firmware uses SHA256 in app descriptor)
    if command -v sha256sum &> /dev/null; then
        HASH=$(sha256sum "$DST_BIN" | awk '{print $1}')
    elif command -v shasum &> /dev/null; then
        # macOS
        HASH=$(shasum -a 256 "$DST_BIN" | awk '{print $1}')
    else
        print_error "sha256sum or shasum command not found"
        exit 1
    fi

    # Write hash file
    echo "$HASH" > "$DST_MD5"

    # Get file size
    FILE_SIZE=$(stat -f%z "$DST_BIN" 2>/dev/null || stat -c%s "$DST_BIN" 2>/dev/null)

    # Get build timestamp
    BUILD_TIME=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

    # Get ESP-IDF version
    IDF_VER=$(idf.py --version 2>/dev/null | head -1 || echo "unknown")

    print_info "Firmware binary: $DST_BIN"
    print_info "Size: $FILE_SIZE bytes"
    print_info "SHA256: $HASH"

    # Generate JSON metadata for server API
    # Note: Field is still called "md5" for API compatibility, but contains SHA256
    cat > "$DST_JSON" << EOF
{
    "model": "perfect-vision",
    "version": "$VERSION",
    "md5": "$HASH",
    "size": $FILE_SIZE,
    "filename": "${PROJECT_NAME}-${VERSION}.bin",
    "latestFilename": "perfect_vision_ecu.bin",
    "buildTime": "$BUILD_TIME",
    "idfVersion": "$IDF_VER",
    "target": "esp32s3"
}
EOF

    # Also create a latest.json for easy version checking
    cat > "$OUTPUT_DIR/latest.json" << EOF
{
    "model": "perfect-vision",
    "version": "$VERSION",
    "md5": "$HASH",
    "size": $FILE_SIZE,
    "filename": "perfect_vision_ecu.bin",
    "buildTime": "$BUILD_TIME",
    "target": "esp32s3"
}
EOF

    print_info "Metadata: $DST_JSON"
    print_info "Latest: $OUTPUT_DIR/latest.json"

    echo ""
    print_status "Output files ready in: $OUTPUT_DIR"
    echo ""
    ls -lh "$OUTPUT_DIR/"
}

# Main
print_status "Perfect Vision ECU - Compile Script"

# Change to script directory first (needed for extract_version)
cd "$(dirname "$0")"

# Extract version from config.h if not specified
if [ -z "$VERSION" ]; then
    VERSION=$(extract_version)
    print_info "Extracted version from config.h: $VERSION"
fi

print_info "Version: $VERSION"
print_info "Output: $OUTPUT_DIR"
echo ""

# Setup
setup_idf
set_target
clean_build

# Build
build_firmware

# Prepare output
prepare_output

echo ""
print_status "Done!"

#!/bin/bash
#
# Compile all Planter/Implement app components on cloud server
#
# Builds:
#   1. ESP32 firmware (PlatformIO)
#   2. Rust BLE bridge (cross-compiled for aarch64)
#   3. Java webapp (Maven)
#
# Usage:
#   ./compile_all.sh           - Build all components
#   ./compile_all.sh esp32     - Build ESP32 only
#   ./compile_all.sh rust      - Build Rust only
#   ./compile_all.sh java      - Build Java only
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUST_DIR="$SCRIPT_DIR/rpi-bridge"
JAVA_DIR="/Source/Perfect-Precision-Implement-app"
TARGET="aarch64-unknown-linux-gnu"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[BUILD]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

build_esp32() {
    log "=== Building ESP32 Firmware ==="

    # Check for PlatformIO
    if ! command -v pio &> /dev/null; then
        log "Installing PlatformIO..."
        pip install platformio
    fi

    cd "$SCRIPT_DIR"
    pio run -e esp32s3

    log "ESP32 build complete!"
    log "  Output: $SCRIPT_DIR/.pio/build/esp32s3/firmware.bin"
}

build_rust() {
    log "=== Building Rust BLE Bridge ==="

    # Check for cross
    if ! command -v cross &> /dev/null && ! [ -f ~/.cargo/bin/cross ]; then
        log "Installing cross..."
        cargo install cross
    fi

    CROSS="${HOME}/.cargo/bin/cross"
    if ! [ -f "$CROSS" ]; then
        CROSS="cross"
    fi

    cd "$RUST_DIR"

    # Create Cross.toml if it doesn't exist
    if [ ! -f Cross.toml ]; then
        log "Creating Cross.toml for ARM64 cross-compilation..."
        cat > Cross.toml << 'EOF'
[target.aarch64-unknown-linux-gnu]
pre-build = [
    "dpkg --add-architecture arm64",
    "apt-get update",
    "apt-get install -y libdbus-1-dev:arm64 libudev-dev:arm64"
]
EOF
    fi

    log "Cross-compiling for $TARGET..."
    "$CROSS" build --release --target "$TARGET"

    log "Rust build complete!"
    log "  Output: $RUST_DIR/target/$TARGET/release/planter-ble-bridge"
}

build_java() {
    log "=== Building Java Webapp ==="

    if [ ! -d "$JAVA_DIR" ]; then
        error "Java project not found at $JAVA_DIR"
    fi

    # Check for Maven
    if ! command -v mvn &> /dev/null; then
        error "Maven not installed. Run: sudo apt install maven"
    fi

    cd "$JAVA_DIR"

    log "Running Maven build..."
    mvn clean package -DskipTests

    WAR_FILE=$(find target -name "*.war" 2>/dev/null | head -1)
    if [ -n "$WAR_FILE" ]; then
        log "Java build complete!"
        log "  Output: $JAVA_DIR/$WAR_FILE"
    else
        warn "No WAR file found in target/"
    fi
}

# Main
case "${1:-all}" in
    esp32)
        build_esp32
        ;;
    rust)
        build_rust
        ;;
    java)
        build_java
        ;;
    all|*)
        build_esp32
        echo ""
        build_rust
        echo ""
        build_java
        echo ""
        log "=== All builds complete! ==="
        ;;
esac

#!/bin/bash
# Perfect Vision ECU - Deploy Script
# Builds and flashes firmware to ESP32-S3 via USB

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Default settings
PORT=""
BAUD_RATE=460800
CLEAN=false
BUILD_ONLY=false

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -p, --port PORT     Serial port (auto-detected if not specified)"
    echo "  -b, --baud RATE     Baud rate (default: 460800)"
    echo "  -c, --clean         Clean build before compiling"
    echo "  --build-only        Build without flashing"
    echo "  -h, --help          Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                      # Build, flash, and monitor"
    echo "  $0 -p /dev/ttyUSB0      # Use specific port"
    echo "  $0 -c                   # Clean build first"
}

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Auto-detect serial port
detect_port() {
    local ports=()

    # Check common ESP32 USB ports on Linux
    for p in /dev/ttyUSB* /dev/ttyACM*; do
        [ -e "$p" ] && ports+=("$p")
    done

    if [ ${#ports[@]} -eq 0 ]; then
        log_error "No serial ports found. Is the ESP32 connected?"
        exit 1
    elif [ ${#ports[@]} -eq 1 ]; then
        PORT="${ports[0]}"
        log_info "Auto-detected port: $PORT"
    else
        log_warn "Multiple ports found: ${ports[*]}"
        PORT="${ports[0]}"
        log_info "Using first port: $PORT"
    fi
}

# Source ESP-IDF environment if needed
setup_idf() {
    if [ -z "$IDF_PATH" ]; then
        if [ -f "$HOME/esp/esp-idf/export.sh" ]; then
            log_info "Sourcing ESP-IDF environment..."
            source "$HOME/esp/esp-idf/export.sh"
        else
            log_error "ESP-IDF not found at ~/esp/esp-idf"
            log_error "Install ESP-IDF or set IDF_PATH manually"
            exit 1
        fi
    fi

    if ! command -v idf.py &> /dev/null; then
        log_error "idf.py not found. Ensure ESP-IDF is properly installed."
        exit 1
    fi

    log_info "Using ESP-IDF at: $IDF_PATH"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -p|--port)
            PORT="$2"
            shift 2
            ;;
        -b|--baud)
            BAUD_RATE="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN=true
            shift
            ;;
        --build-only)
            BUILD_ONLY=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            log_error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Main deployment process
log_info "Perfect Vision ECU Deployment"
echo "================================"

# Setup ESP-IDF environment
setup_idf

# Clean corrupted build directory if it exists but isn't valid
if [ -d "build" ] && [ ! -f "build/CMakeCache.txt" ]; then
    log_warn "Found corrupted build directory, removing..."
    rm -rf build
fi

# Set target if not already configured
if [ ! -f "sdkconfig" ]; then
    log_info "Setting target to esp32s3..."
    idf.py set-target esp32s3
fi

# Clean build if requested
if [ "$CLEAN" = true ]; then
    log_info "Cleaning build directory..."
    rm -rf build
fi

# Build
log_info "Building firmware..."
idf.py build

if [ "$BUILD_ONLY" = true ]; then
    log_info "Build complete. Skipping flash."
    exit 0
fi

# Detect port if not specified
if [ -z "$PORT" ]; then
    detect_port
fi

# Verify port exists
if [ ! -e "$PORT" ]; then
    log_error "Port $PORT does not exist"
    exit 1
fi

# Flash
log_info "Flashing to $PORT at $BAUD_RATE baud..."
idf.py -p "$PORT" -b "$BAUD_RATE" flash

log_info "Flash complete!"

# Start serial monitor
log_info "Starting serial monitor (Ctrl+] to exit)..."
idf.py -p "$PORT" monitor

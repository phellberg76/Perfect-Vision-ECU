#!/bin/bash
#
# Update all Planter/Implement app components from cloud server
#
# Updates:
#   1. ESP32 firmware (flash via UART)
#   2. Rust BLE bridge (copy and restart service)
#   3. Java webapp (copy WAR and restart Tomcat)
#
# Usage:
#   ./update_all_from_cloud.sh           - Update all components (only if changed)
#   ./update_all_from_cloud.sh esp32     - Update ESP32 only
#   ./update_all_from_cloud.sh rust      - Update Rust only
#   ./update_all_from_cloud.sh java      - Update Java only
#   ./update_all_from_cloud.sh --force   - Force update all (ignore checksums)
#   ./update_all_from_cloud.sh esp32 --force - Force update ESP32 only
#
# The script compares MD5 checksums of local and remote files and only downloads
# files that have changed. Use --force to override this behavior.
#
# Prerequisites:
#   - Run compile_all.sh on cloud server first
#   - SSH access to cloud server configured
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP_DIR="/tmp/planter-update"
FORCE_UPDATE=false

# Parse arguments
COMPONENT="all"
for arg in "$@"; do
    case "$arg" in
        --force|-f)
            FORCE_UPDATE=true
            ;;
        esp32|rust|java|all)
            COMPONENT="$arg"
            ;;
    esac
done

# Find or install esptool
FIRMWARE_DIR="/root/Raspberry-Pi-Backup-power-and-GPS-hat-v4/firmware"
VENV_DIR="$FIRMWARE_DIR/.venv"

find_esptool() {
    # Check venv first
    if [ -f "$VENV_DIR/bin/python" ]; then
        if "$VENV_DIR/bin/python" -m esptool version &> /dev/null; then
            echo "$VENV_DIR/bin/python -m esptool"
            return 0
        fi
    fi
    # Check system installations
    for cmd in esptool.py esptool ~/.local/bin/esptool /usr/local/bin/esptool /usr/bin/esptool; do
        if command -v "$cmd" &> /dev/null || [ -x "$cmd" ]; then
            echo "$cmd"
            return 0
        fi
    done
    return 1
}

ESPTOOL=$(find_esptool || true)

if [ -z "$ESPTOOL" ]; then
    echo "[UPDATE] esptool not found, installing in venv..."

    # Ensure python3-venv is installed
    if ! python3 -m venv --help &> /dev/null; then
        echo "[UPDATE] Installing python3-venv..."
        sudo apt-get update && sudo apt-get install -y python3-venv python3-full
    fi

    # Create venv and install esptool
    mkdir -p "$FIRMWARE_DIR"
    python3 -m venv "$VENV_DIR"
    "$VENV_DIR/bin/pip" install --upgrade pip
    "$VENV_DIR/bin/pip" install esptool

    ESPTOOL=$(find_esptool || true)
    if [ -z "$ESPTOOL" ]; then
        echo "Error: Failed to install esptool"
        exit 1
    fi
    echo "[UPDATE] esptool installed successfully"
fi

# Cloud server settings
CLOUD_HOST="peter@cloud"
CLOUD_PORT="3591"
CLOUD_BASE="/Source/Raspberry-Pi-Backup-power-and-GPS-hat-v4/firmware"
CLOUD_ESP32_DIR="$CLOUD_BASE/.pio/build/esp32s3"
CLOUD_RUST_BIN="$CLOUD_BASE/rpi-bridge/target/aarch64-unknown-linux-gnu/release/planter-ble-bridge"
CLOUD_JAVA_DIR="/Source/Perfect-Precision-Implement-app/target"

# Local paths
RUST_BIN_DEST="/usr/local/bin/planter-ble-bridge"
JAVA_WAR_DEST="/opt/tomcat/webapps/planter_app.war"

# Hardware connections for ESP32
UART_DEV="/dev/ttyAMA0"
GPIO_EN=17
GPIO_IO0=27

# Watchdog control
BRIDGE_WDT_URL="http://localhost:8081"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log() { echo -e "${GREEN}[UPDATE]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

# Disable ESP32 watchdog during updates
disable_watchdog() {
    log "Disabling ESP32 watchdog for update..."

    # Stop bridge service first so we can access UART directly
    sudo systemctl stop planter-ble-bridge.service 2>/dev/null || true
    sleep 1

    # Send disable command directly to UART
    echo '{"cmd":"wdt_disable"}' > "$UART_DEV" 2>/dev/null || true
    sleep 0.5
    log "Watchdog disabled, bridge stopped"
}

# Re-enable ESP32 watchdog after updates
enable_watchdog() {
    log "Re-enabling ESP32 watchdog..."

    # Start bridge service first
    sudo systemctl start planter-ble-bridge.service 2>/dev/null || true
    sleep 2

    # Enable watchdog via bridge HTTP
    if curl -s --max-time 3 "$BRIDGE_WDT_URL/enableWatchdog" > /dev/null 2>&1; then
        log "Watchdog enabled via bridge HTTP"
    else
        # Fallback: direct UART (bridge may not be ready yet)
        echo '{"cmd":"wdt_enable"}' > "$UART_DEV" 2>/dev/null || true
        log "Watchdog enabled via UART"
    fi
}

# Hash cache directory (persists between runs)
HASH_CACHE_DIR="/var/cache/planter-update"

# Get MD5 hash of a remote file via SSH
get_remote_hash() {
    local remote_path="$1"
    ssh -p "$CLOUD_PORT" "$CLOUD_HOST" "md5sum '$remote_path' 2>/dev/null | cut -d' ' -f1" 2>/dev/null
}

# Get MD5 hash of a local file
get_local_hash() {
    local local_path="$1"
    if [ -f "$local_path" ]; then
        md5sum "$local_path" 2>/dev/null | cut -d' ' -f1
    fi
}

# Check if a file needs to be downloaded (returns 0 if download needed, 1 if not)
needs_download() {
    local remote_path="$1"
    local local_path="$2"
    local cache_key="$3"  # Optional: use cached hash instead of local file

    # Force update bypasses checksum comparison
    if [ "$FORCE_UPDATE" = true ]; then
        return 0
    fi

    local remote_hash=$(get_remote_hash "$remote_path")
    if [ -z "$remote_hash" ]; then
        warn "Could not get remote hash for $remote_path, will download"
        return 0
    fi

    local local_hash=""
    if [ -n "$cache_key" ]; then
        # Use cached hash (for files like ESP32 firmware that get flashed, not stored)
        local cache_file="$HASH_CACHE_DIR/$cache_key"
        if [ -f "$cache_file" ]; then
            local_hash=$(cat "$cache_file")
        fi
    else
        local_hash=$(get_local_hash "$local_path")
    fi

    if [ "$remote_hash" = "$local_hash" ]; then
        return 1  # No download needed
    fi
    return 0  # Download needed
}

# Save hash to cache (for files that get flashed/deployed, not stored locally)
save_hash_cache() {
    local cache_key="$1"
    local hash="$2"
    sudo mkdir -p "$HASH_CACHE_DIR"
    echo "$hash" | sudo tee "$HASH_CACHE_DIR/$cache_key" > /dev/null
}

# Smart copy: only download if file has changed
smart_scp() {
    local remote_path="$1"
    local local_path="$2"
    local cache_key="${3:-}"  # Optional cache key for non-persistent files

    if needs_download "$remote_path" "$local_path" "$cache_key"; then
        log "Downloading $(basename "$remote_path") (changed)..."
        scp -P "$CLOUD_PORT" "$CLOUD_HOST:$remote_path" "$local_path" || return 1

        # Update cache if using cache key
        if [ -n "$cache_key" ]; then
            local new_hash=$(get_local_hash "$local_path")
            save_hash_cache "$cache_key" "$new_hash"
        fi
        return 0
    else
        log "Skipping $(basename "$remote_path") (unchanged)"
        return 2  # Special return code: file unchanged
    fi
}

# Set GPIO using pinctrl
gpio_set() {
    local pin=$1
    local value=$2

    if command -v pinctrl &> /dev/null; then
        pinctrl set $pin op
        if [ "$value" = "1" ]; then
            pinctrl set $pin dh
        else
            pinctrl set $pin dl
        fi
    elif command -v gpioset &> /dev/null; then
        gpioset gpiochip0 $pin=$value
    else
        if [ ! -d "/sys/class/gpio/gpio$pin" ]; then
            echo $pin > /sys/class/gpio/export 2>/dev/null || true
            sleep 0.1
        fi
        echo out > /sys/class/gpio/gpio$pin/direction 2>/dev/null || true
        echo $value > /sys/class/gpio/gpio$pin/value
    fi
}

enter_bootloader() {
    log "Entering bootloader mode..."
    gpio_set $GPIO_IO0 0
    sleep 0.1
    gpio_set $GPIO_EN 0
    sleep 0.1
    gpio_set $GPIO_EN 1
    sleep 0.3
}

reset_esp32() {
    log "Resetting ESP32..."
    gpio_set $GPIO_IO0 1
    sleep 0.1
    gpio_set $GPIO_EN 0
    sleep 0.1
    gpio_set $GPIO_EN 1
}

update_esp32() {
    log "=== Updating ESP32 Firmware ==="

    mkdir -p "$TMP_DIR"

    # Check if any ESP32 files have changed
    log "Checking for ESP32 firmware changes..."
    local esp32_changed=false

    for file in bootloader.bin partitions.bin firmware.bin; do
        if needs_download "$CLOUD_ESP32_DIR/$file" "$TMP_DIR/$file" "esp32_$file"; then
            esp32_changed=true
            break
        fi
    done

    if [ "$esp32_changed" = false ]; then
        log "ESP32 firmware unchanged, skipping update"
        return 0
    fi

    log "ESP32 firmware changed, downloading all binaries..."
    # Download all 3 files since flashing requires all of them
    for file in bootloader.bin partitions.bin firmware.bin; do
        log "Downloading $file..."
        scp -P "$CLOUD_PORT" "$CLOUD_HOST:$CLOUD_ESP32_DIR/$file" "$TMP_DIR/$file" \
            || error "Failed to fetch $file"
    done

    # Update hash cache for all files after successful download
    for file in bootloader.bin partitions.bin firmware.bin; do
        local new_hash=$(get_local_hash "$TMP_DIR/$file")
        save_hash_cache "esp32_$file" "$new_hash"
    done

    # Stop services using serial port
    log "Stopping planter-ble-bridge.service..."
    sudo systemctl stop planter-ble-bridge.service 2>/dev/null || true
    sleep 1

    if pgrep -x pppd > /dev/null; then
        log "Stopping pppd..."
        sudo killall pppd 2>/dev/null || true
        sleep 1
    fi

    # Clean up serial port
    if command -v fuser &> /dev/null; then
        sudo fuser -k "$UART_DEV" >/dev/null 2>&1 || true
        sleep 1
    fi

    # Reset serial port to known state
    if [ -e "$UART_DEV" ]; then
        stty -F "$UART_DEV" 115200 cs8 -cstopb -parenb raw -echo 2>/dev/null || true
    fi

    # Reset ESP32 first to get it out of PPP mode
    log "Resetting ESP32 out of PPP mode..."
    reset_esp32
    sleep 0.5

    enter_bootloader

    log "Flashing ESP32..."
    $ESPTOOL --chip esp32s3 \
        --port "$UART_DEV" \
        --baud 460800 \
        --before no-reset \
        --after no-reset \
        write-flash \
        -z \
        --flash-mode dio \
        --flash-freq 80m \
        --flash-size 8MB \
        0x0000 "$TMP_DIR/bootloader.bin" \
        0x8000 "$TMP_DIR/partitions.bin" \
        0x10000 "$TMP_DIR/firmware.bin"

    reset_esp32

    # Restart service
    sleep 2
    log "Restarting planter-ble-bridge.service..."
    sudo systemctl start planter-ble-bridge.service 2>/dev/null || true

    log "ESP32 update complete!"
}

update_rust() {
    log "=== Updating Rust BLE Bridge ==="

    mkdir -p "$TMP_DIR"

    # Ensure systemd service is installed and not masked
    SERVICE_FILE="/etc/systemd/system/planter-ble-bridge.service"
    NEEDS_INSTALL=false

    # Check if masked (symlink to /dev/null) or missing
    if [ -L "$SERVICE_FILE" ]; then
        # It's a symlink - check if it points to /dev/null (masked)
        if [ "$(readlink -f "$SERVICE_FILE")" = "/dev/null" ]; then
            log "Service is masked, removing mask..."
            sudo rm -f "$SERVICE_FILE"
            sudo systemctl daemon-reload
            NEEDS_INSTALL=true
        fi
    elif [ ! -f "$SERVICE_FILE" ]; then
        NEEDS_INSTALL=true
    fi

    if [ "$NEEDS_INSTALL" = true ]; then
        log "Installing planter-ble-bridge systemd service..."
        scp -P "$CLOUD_PORT" "$CLOUD_HOST:$CLOUD_BASE/rpi/planter-ble-bridge.service" "$TMP_DIR/" \
            || error "Failed to fetch service file"
        sudo cp "$TMP_DIR/planter-ble-bridge.service" "$SERVICE_FILE"
        sudo systemctl daemon-reload
        sudo systemctl enable planter-ble-bridge.service
        log "Service installed and enabled"
    fi

    log "Checking for Rust binary changes..."
    # Compare against installed binary
    if ! needs_download "$CLOUD_RUST_BIN" "$RUST_BIN_DEST"; then
        log "Rust binary unchanged, skipping update"
        return 0
    fi

    log "Rust binary changed, downloading..."
    scp -P "$CLOUD_PORT" "$CLOUD_HOST:$CLOUD_RUST_BIN" "$TMP_DIR/planter-ble-bridge" \
        || error "Failed to fetch Rust binary"

    log "Stopping planter-ble-bridge service..."
    sudo systemctl stop planter-ble-bridge.service 2>/dev/null || true

    log "Installing new binary..."
    sudo cp "$TMP_DIR/planter-ble-bridge" "$RUST_BIN_DEST"
    sudo chmod +x "$RUST_BIN_DEST"

    log "Starting planter-ble-bridge service..."
    sudo systemctl start planter-ble-bridge.service

    log "Rust update complete!"
}

update_java() {
    log "=== Updating Java Webapp ==="

    mkdir -p "$TMP_DIR"

    # Find the WAR file name on remote
    log "Checking for Java WAR changes..."
    REMOTE_WAR=$(ssh -p "$CLOUD_PORT" "$CLOUD_HOST" "ls -1 $CLOUD_JAVA_DIR/planter_app*.war 2>/dev/null | head -1")
    if [ -z "$REMOTE_WAR" ]; then
        error "No WAR file found on cloud server"
    fi

    # Compare against installed WAR
    if ! needs_download "$REMOTE_WAR" "$JAVA_WAR_DEST"; then
        log "Java WAR unchanged, skipping update"
        return 0
    fi

    log "Java WAR changed, downloading..."
    scp -P "$CLOUD_PORT" "$CLOUD_HOST:$REMOTE_WAR" "$TMP_DIR/" \
        || error "Failed to fetch WAR file"

    # Ensure file is fully written to disk
    sync

    WAR_FILE=$(ls "$TMP_DIR"/*.war 2>/dev/null | head -1)
    if [ -z "$WAR_FILE" ]; then
        error "No WAR file found"
    fi

    # Verify WAR file is valid (not empty or corrupted)
    log "Verifying WAR file integrity..."
    WAR_SIZE=$(stat -c%s "$WAR_FILE" 2>/dev/null || stat -f%z "$WAR_FILE" 2>/dev/null)
    if [ -z "$WAR_SIZE" ] || [ "$WAR_SIZE" -lt 1000 ]; then
        error "WAR file is too small ($WAR_SIZE bytes) - likely corrupted"
    fi

    if ! unzip -t "$WAR_FILE" > /dev/null 2>&1; then
        error "WAR file is corrupted (failed zip integrity check)"
    fi
    log "WAR file verified: $WAR_SIZE bytes, integrity OK"

    # Stop Tomcat before deploying (prevents partial deployment issues)
    log "Stopping Tomcat..."
    sudo service tomcat stop
    sleep 2

    # Remove old webapp directory to ensure clean deployment
    WEBAPP_DIR="/opt/tomcat/webapps/planter_app"
    if [ -d "$WEBAPP_DIR" ]; then
        log "Removing old webapp directory..."
        sudo rm -rf "$WEBAPP_DIR"
    fi

    # Deploy the new WAR
    log "Deploying to Tomcat..."
    sudo cp "$WAR_FILE" "$JAVA_WAR_DEST"
    sync

    # Verify deployed WAR
    if ! unzip -t "$JAVA_WAR_DEST" > /dev/null 2>&1; then
        error "Deployed WAR file is corrupted - deployment failed"
    fi

    log "Starting Tomcat..."
    sudo service tomcat start

    # Wait for Tomcat to start and check if deployment succeeded
    log "Waiting for Tomcat to start..."
    sleep 5
    if [ -d "$WEBAPP_DIR" ]; then
        log "Java webapp deployed successfully!"
    else
        warn "Webapp directory not created yet - check Tomcat logs if issues persist"
    fi

    log "Java update complete!"
}

cleanup() {
    rm -rf "$TMP_DIR"
}

# Ensure system is configured for Pi4J v2 GPIO access
ensure_system_setup() {
    log "=== Ensuring System Configuration ==="

    # Ensure tomcat user has GPIO access
    log "Ensuring tomcat user is in gpio group..."
    sudo usermod -aG gpio tomcat

    # Install and enable pigpiod for Pi4J v2 GPIO access
    if ! command -v pigpiod &> /dev/null; then
        log "Installing pigpio package..."
        sudo apt-get update
        sudo apt-get install -y pigpio
    fi

    if ! systemctl is-active --quiet pigpiod; then
        log "Starting pigpiod daemon..."
        sudo systemctl enable pigpiod
        sudo systemctl start pigpiod
    else
        log "pigpiod already running"
    fi
}

# Main
trap cleanup EXIT

if [ "$FORCE_UPDATE" = true ]; then
    log "Force update enabled - checksums will be ignored"
fi

case "$COMPONENT" in
    esp32)
        disable_watchdog
        update_esp32
        enable_watchdog
        ;;
    rust)
        disable_watchdog
        update_rust
        enable_watchdog
        ;;
    java)
        disable_watchdog
        update_java
        enable_watchdog
        ;;
    all)
        log "=== Updating All Components from Cloud ==="
        echo ""
        disable_watchdog
        echo ""
        ensure_system_setup
        echo ""
        update_esp32
        echo ""
        update_rust
        echo ""
        update_java
        echo ""
        enable_watchdog
        echo ""
        log "=== All updates complete! ==="
        ;;
    *)
        error "Unknown component: $COMPONENT (use esp32, rust, java, or all)"
        ;;
esac

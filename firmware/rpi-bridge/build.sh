#!/bin/bash
#
# Planter BLE Bridge - Build and Deploy Script
#
# Usage:
#   ./build.sh              # Build only
#   ./build.sh install      # Build and install to /usr/local/bin
#   ./build.sh service      # Build, install, and setup systemd service
#   ./build.sh pull         # Git pull, build, and install
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY_NAME="planter-ble-bridge"
INSTALL_DIR="/usr/local/bin"
SERVICE_NAME="planter-ble-bridge"

# ============================================================
# CONFIGURATION - Edit these values for your deployment
# ============================================================
BLUETOOTH_NAME="PerfectPrecisionPlanter"
BACKEND_URL="http://localhost:8080"
# UART mode: ESP32 handles BLE, this bridge handles HTTP proxy
USE_UART_MODE=true
UART_PORT="/dev/ttyAMA0"
UART_BAUD=115200
# ============================================================

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if running on ARM (RPi)
check_architecture() {
    ARCH=$(uname -m)
    if [[ "$ARCH" != "aarch64" && "$ARCH" != "armv7l" ]]; then
        log_warn "Not running on ARM architecture ($ARCH). This script is intended for Raspberry Pi."
    fi
}

# Install system dependencies
install_dependencies() {
    log_info "Checking system dependencies..."

    PACKAGES_NEEDED=""

    if ! dpkg -s libdbus-1-dev &>/dev/null; then
        PACKAGES_NEEDED="$PACKAGES_NEEDED libdbus-1-dev"
    fi

    if ! dpkg -s pkg-config &>/dev/null; then
        PACKAGES_NEEDED="$PACKAGES_NEEDED pkg-config"
    fi

    if ! dpkg -s build-essential &>/dev/null; then
        PACKAGES_NEEDED="$PACKAGES_NEEDED build-essential"
    fi

    if ! dpkg -s libssl-dev &>/dev/null; then
        PACKAGES_NEEDED="$PACKAGES_NEEDED libssl-dev"
    fi

    if [ -n "$PACKAGES_NEEDED" ]; then
        log_info "Installing: $PACKAGES_NEEDED"
        sudo apt-get update
        sudo apt-get install -y $PACKAGES_NEEDED
    else
        log_info "All system dependencies installed"
    fi
}

# Install Rust if not present
install_rust() {
    if command -v cargo &>/dev/null; then
        log_info "Rust is already installed: $(cargo --version)"
    else
        log_info "Installing Rust..."
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
        source "$HOME/.cargo/env"
        log_info "Rust installed: $(cargo --version)"
    fi
}

# Build the project
build_project() {
    log_info "Building $BINARY_NAME..."
    cd "$SCRIPT_DIR"

    # Source cargo env in case it was just installed
    if [ -f "$HOME/.cargo/env" ]; then
        source "$HOME/.cargo/env"
    fi

    # Limit to single job to reduce memory usage on RPi
    export CARGO_BUILD_JOBS=1
    cargo build --release

    BINARY_SIZE=$(ls -lh "target/release/$BINARY_NAME" | awk '{print $5}')
    log_info "Build complete: target/release/$BINARY_NAME ($BINARY_SIZE)"
}

# Install binary to system
install_binary() {
    log_info "Installing $BINARY_NAME to $INSTALL_DIR..."
    sudo cp "target/release/$BINARY_NAME" "$INSTALL_DIR/"
    sudo chmod +x "$INSTALL_DIR/$BINARY_NAME"
    log_info "Installed: $INSTALL_DIR/$BINARY_NAME"
}

# Configure Bluetooth in /boot/firmware/config.txt for CM4
configure_bluetooth() {
    log_info "Checking Bluetooth configuration..."

    CONFIG_FILE="/boot/firmware/config.txt"
    if [ ! -f "$CONFIG_FILE" ]; then
        CONFIG_FILE="/boot/config.txt"
    fi

    if [ ! -f "$CONFIG_FILE" ]; then
        log_warn "Could not find config.txt. Skipping Bluetooth configuration."
        return
    fi

    NEEDS_REBOOT=false

    # Check for disable-bt and replace with miniuart-bt
    if grep -q "dtoverlay=disable-bt" "$CONFIG_FILE"; then
        log_info "Found dtoverlay=disable-bt, replacing with miniuart-bt..."
        sudo sed -i 's/dtoverlay=disable-bt/dtoverlay=miniuart-bt/' "$CONFIG_FILE"
        NEEDS_REBOOT=true
    elif ! grep -q "dtoverlay=miniuart-bt" "$CONFIG_FILE"; then
        log_info "Adding dtoverlay=miniuart-bt..."
        echo "dtoverlay=miniuart-bt" | sudo tee -a "$CONFIG_FILE" > /dev/null
        NEEDS_REBOOT=true
    else
        log_info "Bluetooth overlay already configured correctly"
    fi

    # Ensure core_freq is set to stabilize mini UART clock
    if ! grep -q "^core_freq=" "$CONFIG_FILE"; then
        log_info "Adding core_freq=250 to stabilize mini UART clock..."
        echo "core_freq=250" | sudo tee -a "$CONFIG_FILE" > /dev/null
        NEEDS_REBOOT=true
    fi

    if [ "$NEEDS_REBOOT" = true ]; then
        log_warn "Bluetooth configuration changed. A REBOOT is required for changes to take effect."
        log_warn "Run 'sudo reboot' after installation completes."
    fi
}

# Setup systemd service
setup_service() {
    log_info "Setting up systemd service..."

    # Build ExecStart command based on mode
    if [ "$USE_UART_MODE" = true ]; then
        EXEC_START="$INSTALL_DIR/$BINARY_NAME --uart --port $UART_PORT --baud $UART_BAUD --backend $BACKEND_URL"
        EXEC_PRE=""
        MODE_COMMENT="# UART mode - ESP32 handles BLE"
    else
        EXEC_START="$INSTALL_DIR/$BINARY_NAME --name $BLUETOOTH_NAME --backend $BACKEND_URL"
        EXEC_PRE="ExecStartPre=/usr/sbin/rfkill unblock bluetooth
ExecStartPre=/bin/sleep 1
ExecStartPre=/usr/bin/hciconfig hci0 up"
        MODE_COMMENT="# BLE mode - this bridge handles BLE directly"
    fi

    # Create service file
    cat > /tmp/planter-ble-bridge.service << EOF
[Unit]
Description=Planter BLE Bridge
Documentation=https://github.com/perfectprecision/planter-ble-bridge
After=bluetooth.target
Wants=bluetooth.target

[Service]
Type=simple
User=root
$MODE_COMMENT
$EXEC_PRE
ExecStart=$EXEC_START
Restart=always
RestartSec=5
StandardOutput=journal
StandardError=journal

# Bluetooth requires root or special capabilities
AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW

[Install]
WantedBy=multi-user.target
EOF

    sudo mv /tmp/planter-ble-bridge.service "/etc/systemd/system/${SERVICE_NAME}.service"

    log_info "Configured with:"
    if [ "$USE_UART_MODE" = true ]; then
        log_info "  Mode: UART (ESP32 handles BLE)"
        log_info "  UART port: $UART_PORT @ $UART_BAUD baud"
    else
        log_info "  Mode: BLE (direct)"
        log_info "  Bluetooth name: $BLUETOOTH_NAME"
    fi
    log_info "  Backend URL: $BACKEND_URL"

    sudo systemctl daemon-reload
    sudo systemctl enable "$SERVICE_NAME"

    log_info "Service installed: $SERVICE_NAME"
    log_info "Commands:"
    log_info "  sudo systemctl start $SERVICE_NAME    # Start service"
    log_info "  sudo systemctl stop $SERVICE_NAME     # Stop service"
    log_info "  sudo systemctl status $SERVICE_NAME   # Check status"
    log_info "  journalctl -u $SERVICE_NAME -f        # View logs"
}

# Git pull latest changes
git_pull() {
    log_info "Pulling latest changes..."
    cd "$SCRIPT_DIR"

    if [ -d ".git" ]; then
        git pull
    else
        log_warn "Not a git repository. Skipping pull."
    fi
}

# Restart service if running
restart_service() {
    if systemctl is-active --quiet "$SERVICE_NAME"; then
        log_info "Restarting $SERVICE_NAME service..."
        sudo systemctl restart "$SERVICE_NAME"
        sleep 2
        sudo systemctl status "$SERVICE_NAME" --no-pager
    fi
}

# Main
main() {
    check_architecture

    case "${1:-build}" in
        build)
            install_dependencies
            install_rust
            build_project
            ;;
        install)
            install_dependencies
            install_rust
            build_project
            install_binary
            restart_service
            ;;
        service)
            install_dependencies
            install_rust
            build_project
            install_binary
            configure_bluetooth
            setup_service
            sudo systemctl start "$SERVICE_NAME"
            sleep 2
            sudo systemctl status "$SERVICE_NAME" --no-pager
            ;;
        pull)
            git_pull
            install_dependencies
            install_rust
            build_project
            install_binary
            restart_service
            ;;
        status)
            sudo systemctl status "$SERVICE_NAME" --no-pager
            ;;
        logs)
            journalctl -u "$SERVICE_NAME" -f
            ;;
        stop)
            sudo systemctl stop "$SERVICE_NAME"
            log_info "Service stopped"
            ;;
        start)
            sudo systemctl start "$SERVICE_NAME"
            sleep 2
            sudo systemctl status "$SERVICE_NAME" --no-pager
            ;;
        *)
            echo "Usage: $0 {build|install|service|pull|status|logs|start|stop}"
            echo ""
            echo "Commands:"
            echo "  build    - Build the project (default)"
            echo "  install  - Build and install to $INSTALL_DIR"
            echo "  service  - Build, install, and setup systemd service"
            echo "  pull     - Git pull, build, and install"
            echo "  status   - Show service status"
            echo "  logs     - Follow service logs"
            echo "  start    - Start the service"
            echo "  stop     - Stop the service"
            exit 1
            ;;
    esac
}

main "$@"

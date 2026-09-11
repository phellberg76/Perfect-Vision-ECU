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
#   ./compile_all.sh           - Pull and build changed components
#   ./compile_all.sh esp32     - Build ESP32 only
#   ./compile_all.sh rust      - Build Rust only
#   ./compile_all.sh java      - Build Java only
#   ./compile_all.sh --force   - Force rebuild all components
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FIRMWARE_DIR="$SCRIPT_DIR"
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

# Track what needs rebuilding
REBUILD_ESP32=false
REBUILD_RUST=false
REBUILD_JAVA=false

# Pull a git repo and check if specific paths changed
# Usage: pull_and_check_changes <repo_dir> <path_pattern> <var_name>
# Sets the variable to true if changes detected
pull_and_check_changes() {
    local repo_dir="$1"
    local path_pattern="$2"
    local var_name="$3"

    if [ ! -d "$repo_dir" ]; then
        warn "Directory not found: $repo_dir"
        return 1
    fi

    cd "$repo_dir"

    # Get current commit
    local before_commit=$(git rev-parse HEAD)

    # Pull changes
    log "Pulling $repo_dir..."
    git pull --quiet

    # Get new commit
    local after_commit=$(git rev-parse HEAD)

    # Check if commits differ
    if [ "$before_commit" != "$after_commit" ]; then
        # Check if the relevant files changed
        if [ -n "$path_pattern" ]; then
            local changed_files=$(git diff --name-only "$before_commit" "$after_commit" -- $path_pattern)
            if [ -n "$changed_files" ]; then
                log "Changes detected in $path_pattern"
                eval "$var_name=true"
                return 0
            else
                log "Commit changed but no changes in $path_pattern"
                return 0
            fi
        else
            # No path pattern, any change triggers rebuild
            log "Changes detected"
            eval "$var_name=true"
            return 0
        fi
    else
        log "No changes in $repo_dir"
    fi
    return 0
}

do_git_pulls() {
    log "=== Checking for updates ==="

    # Pull firmware repo and check for ESP32/Rust changes
    cd "$FIRMWARE_DIR"
    local before_commit=$(git rev-parse HEAD)
    log "Pulling firmware repo..."
    git pull --quiet
    local after_commit=$(git rev-parse HEAD)

    if [ "$before_commit" != "$after_commit" ]; then
        # Check ESP32 sources
        local esp32_changes=$(git diff --name-only "$before_commit" "$after_commit" -- src/ platformio.ini)
        if [ -n "$esp32_changes" ]; then
            log "ESP32 source changes detected"
            REBUILD_ESP32=true
        fi

        # Check Rust sources
        local rust_changes=$(git diff --name-only "$before_commit" "$after_commit" -- rpi-bridge/)
        if [ -n "$rust_changes" ]; then
            log "Rust source changes detected"
            REBUILD_RUST=true
        fi
    else
        log "No changes in firmware repo"
    fi

    # Pull Java repo and check for source changes
    if [ -d "$JAVA_DIR" ]; then
        cd "$JAVA_DIR"
        before_commit=$(git rev-parse HEAD)
        log "Pulling Java repo..."
        git pull --quiet
        after_commit=$(git rev-parse HEAD)

        if [ "$before_commit" != "$after_commit" ]; then
            # Check Java sources (src/ and pom.xml)
            local java_changes=$(git diff --name-only "$before_commit" "$after_commit" -- src/ pom.xml)
            if [ -n "$java_changes" ]; then
                log "Java source changes detected"
                REBUILD_JAVA=true
            fi
        else
            log "No changes in Java repo"
        fi
    fi

    echo ""
}

build_esp32() {
    log "=== Building ESP32 Firmware ==="

    # Check for PlatformIO
    if ! command -v pio &> /dev/null; then
        log "Installing PlatformIO via pipx..."
        if ! command -v pipx &> /dev/null; then
            sudo apt install -y pipx
            pipx ensurepath
        fi
        pipx install platformio
        export PATH="$HOME/.local/bin:$PATH"
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

    # Create Cross.toml if it doesn't exist or update it
    log "Creating Cross.toml for ARM64 cross-compilation..."
    cat > Cross.toml << 'EOF'
[target.aarch64-unknown-linux-gnu]
pre-build = [
    "dpkg --add-architecture arm64",
    "apt-get update",
    "apt-get install -y libdbus-1-dev:arm64 libudev-dev:arm64 libssl-dev:arm64"
]
EOF

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
    force|--force)
        log "Force rebuild all components"
        do_git_pulls
        build_esp32
        echo ""
        build_rust
        echo ""
        build_java
        echo ""
        log "=== All builds complete! ==="
        ;;
    all|*)
        do_git_pulls

        if [ "$REBUILD_ESP32" = true ]; then
            build_esp32
            echo ""
        else
            log "ESP32: No rebuild needed (no changes)"
            echo ""
        fi

        if [ "$REBUILD_RUST" = true ]; then
            build_rust
            echo ""
        else
            log "Rust: No rebuild needed (no changes)"
            echo ""
        fi

        if [ "$REBUILD_JAVA" = true ]; then
            build_java
            echo ""
        else
            log "Java: No rebuild needed (no changes)"
            echo ""
        fi

        log "=== Build check complete! ==="
        ;;
esac

#!/bin/bash
#
# Planter Shutdown Monitor
# Watches for /tmp/shutdown and /tmp/reboot files created by the Java app
# Performs graceful shutdown: stops Tomcat, disables ESP32 watchdog, then powers off
#

SHUTDOWN_FILE="/tmp/shutdown"
REBOOT_FILE="/tmp/reboot"
CHECK_INTERVAL=5

log() {
    echo "$(date '+%Y-%m-%d %H:%M:%S') [shutdown-monitor] $1"
    logger -t planter-shutdown-monitor "$1"
}

graceful_shutdown() {
    local action=$1
    log "Detected $action request"

    # Disable ESP32 watchdog first (in case Tomcat is already stopped)
    log "Disabling ESP32 watchdog..."
    curl -s --connect-timeout 2 http://localhost:8081/disableWatchdog || true

    # Stop Tomcat gracefully (this triggers destroy() which also disables watchdog)
    log "Stopping Tomcat..."
    systemctl stop tomcat.service 2>/dev/null || systemctl stop tomcat 2>/dev/null || true

    # Stop the BLE bridge
    log "Stopping BLE bridge..."
    systemctl stop planter-ble-bridge.service 2>/dev/null || true

    # Small delay to ensure everything is stopped
    sleep 2

    # Remove the trigger file
    rm -f "$SHUTDOWN_FILE" "$REBOOT_FILE"

    # Execute the action
    log "Executing $action..."
    if [ "$action" = "shutdown" ]; then
        /sbin/shutdown -h now "Planter app requested shutdown"
    else
        /sbin/shutdown -r now "Planter app requested reboot"
    fi
}

log "Starting shutdown monitor"

# Clean up any stale files from previous boot
rm -f "$SHUTDOWN_FILE" "$REBOOT_FILE"

while true; do
    if [ -f "$SHUTDOWN_FILE" ]; then
        graceful_shutdown "shutdown"
    elif [ -f "$REBOOT_FILE" ]; then
        graceful_shutdown "reboot"
    fi
    sleep $CHECK_INTERVAL
done

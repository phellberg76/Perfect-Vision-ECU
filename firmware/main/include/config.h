/**
 * @file config.h
 * @brief Perfect Vision ECU firmware configuration
 */

#ifndef CONFIG_H
#define CONFIG_H

// Firmware version
#define FIRMWARE_VERSION    "1.0.3"

// Device model identifier
#define DEVICE_MODEL        "perfect-vision"

// BLE device name prefix (MAC suffix will be appended)
#define BLE_DEVICE_NAME_PREFIX  "PerfectVision"

// Default CAN bitrate (250 kbps for agricultural/ISOBUS)
#define DEFAULT_CAN_BITRATE     250000

// CAN message buffer size
#define CAN_MESSAGE_BUFFER_SIZE 256

// CAN data streaming interval (ms)
#define CAN_STREAM_INTERVAL_MS  10

#endif // CONFIG_H

/**
 * @file wifi_transfer.h
 * @brief WiFi-based high-speed data transfer for Perfect Vision ECU
 *
 * Connects to a phone's WiFi hotspot for high-throughput CAN data streaming.
 * BLE is used to provision the WiFi credentials and for command/control.
 *
 * TODO: OTA Relay via Tablet Hotspot
 * Since we're already connected to the tablet's hotspot for CAN streaming,
 * we could use this connection for OTA updates as well:
 * - Tablet downloads firmware from cloud and serves it locally
 * - ESP32 fetches OTA from tablet's IP (e.g., http://192.168.43.1:8080/firmware.bin)
 * - Eliminates need to provision separate WiFi credentials for OTA
 * - Works in field conditions without external WiFi
 * See also: flutter_example/lib/services/wifi_transfer_service.dart
 */

#ifndef WIFI_TRANSFER_H
#define WIFI_TRANSFER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

/**
 * @brief WiFi transfer connection state
 */
typedef enum {
    WIFI_TRANSFER_DISCONNECTED,   // Not connected to hotspot
    WIFI_TRANSFER_CONNECTING,     // Attempting to connect
    WIFI_TRANSFER_CONNECTED,      // Connected to hotspot, no TCP yet
    WIFI_TRANSFER_READY,          // TCP connection established, ready for data
    WIFI_TRANSFER_ERROR,          // Connection error
} wifi_transfer_state_t;

/**
 * @brief Callback for connection state changes
 */
typedef void (*wifi_transfer_state_callback_t)(wifi_transfer_state_t state);

/**
 * @brief Callback for received data over WiFi
 * @param data Pointer to received data
 * @param len Length of received data
 */
typedef void (*wifi_transfer_rx_callback_t)(const uint8_t *data, size_t len);

/**
 * @brief Initialize WiFi transfer module
 *
 * Sets up WiFi station mode but doesn't connect yet.
 * Call wifi_transfer_connect() to connect to a hotspot.
 *
 * @param rx_callback Callback for received data
 * @return ESP_OK on success
 */
esp_err_t wifi_transfer_init(wifi_transfer_rx_callback_t rx_callback);

/**
 * @brief Set state change callback
 * @param callback Function to call on state changes
 */
void wifi_transfer_set_state_callback(wifi_transfer_state_callback_t callback);

/**
 * @brief Connect to a WiFi hotspot and establish TCP connection
 *
 * @param ssid Hotspot SSID
 * @param password Hotspot password
 * @param host_ip IP address of the host (phone) to connect to
 * @param port TCP port to connect to
 * @return ESP_OK if connection started (async)
 */
esp_err_t wifi_transfer_connect(const char *ssid, const char *password,
                                 const char *host_ip, uint16_t port);

/**
 * @brief Disconnect from WiFi hotspot
 * @return ESP_OK on success
 */
esp_err_t wifi_transfer_disconnect(void);

/**
 * @brief Send data over WiFi TCP connection
 *
 * @param data Data to send
 * @param len Length of data
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t wifi_transfer_send(const uint8_t *data, size_t len);

/**
 * @brief Send CAN frame over WiFi (binary format)
 *
 * Binary format (13 bytes per frame):
 *   [4 bytes: CAN ID (big-endian)] [1 byte: DLC] [8 bytes: data]
 *
 * @param can_id CAN identifier
 * @param data CAN frame data
 * @param dlc Data length code (0-8)
 * @return ESP_OK on success
 */
esp_err_t wifi_transfer_send_can_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc);

/**
 * @brief Check if WiFi transfer is active and ready
 * @return true if connected and TCP ready
 */
bool wifi_transfer_is_ready(void);

/**
 * @brief Get current connection state
 * @return Current state
 */
wifi_transfer_state_t wifi_transfer_get_state(void);

/**
 * @brief Get transfer statistics
 * @param tx_bytes Pointer to store transmitted bytes (can be NULL)
 * @param rx_bytes Pointer to store received bytes (can be NULL)
 * @param tx_frames Pointer to store transmitted CAN frames (can be NULL)
 */
void wifi_transfer_get_stats(uint32_t *tx_bytes, uint32_t *rx_bytes, uint32_t *tx_frames);

/**
 * @brief Deinitialize WiFi transfer module
 */
void wifi_transfer_deinit(void);

#endif // WIFI_TRANSFER_H

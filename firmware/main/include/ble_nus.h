/**
 * @file ble_nus.h
 * @brief BLE Nordic UART Service for Flutter app communication
 *
 * Implements NUS (Nordic UART Service) which is widely supported
 * by Flutter BLE libraries for iOS and Android.
 */

#ifndef BLE_NUS_H
#define BLE_NUS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Callback type for received BLE messages (text/JSON)
 * @param data The received data (JSON string, null-terminated)
 * @param len Length of the data
 */
typedef void (*ble_rx_callback_t)(const char *data, size_t len);

/**
 * @brief Callback type for received binary data (CAN data mode)
 * @param data The received binary data
 * @param len Length of the data
 */
typedef void (*ble_binary_rx_callback_t)(const uint8_t *data, size_t len);

/**
 * @brief Callback type for BLE connection state changes
 * @param connected true if connected, false if disconnected
 */
typedef void (*ble_connection_callback_t)(bool connected);

/**
 * @brief Initialize BLE NUS service
 * @param device_name_prefix Prefix for device name (MAC will be appended)
 * @param rx_callback Callback function for received messages
 * @return ESP_OK on success
 */
esp_err_t ble_nus_init(const char *device_name_prefix, ble_rx_callback_t rx_callback);

/**
 * @brief Set connection state callback
 * @param callback Function to call on connect/disconnect
 */
void ble_nus_set_connection_callback(ble_connection_callback_t callback);

/**
 * @brief Send data to connected BLE client
 * @param data The data to send (JSON string)
 * @param len Length of the data (0 to use strlen)
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not connected
 */
esp_err_t ble_nus_send(const char *data, size_t len);

/**
 * @brief Send binary data without newline terminator
 * @param data The binary data to send
 * @param len Length of the data
 * @return ESP_OK on success
 */
esp_err_t ble_nus_send_binary(const uint8_t *data, size_t len);

/**
 * @brief Send CAN frame data on dedicated CAN characteristic
 * @param data The CAN frame data
 * @param len Length of the data
 * @return ESP_OK on success
 */
esp_err_t ble_nus_send_can(const uint8_t *data, size_t len);

/**
 * @brief Send GPS data on dedicated GPS characteristic
 * @param json The GPS JSON string
 * @param len Length of the data (0 to use strlen)
 * @return ESP_OK on success
 */
esp_err_t ble_nus_send_gps(const char *json, size_t len);

/**
 * @brief Check if a BLE client is connected
 * @return true if connected
 */
bool ble_nus_is_connected(void);

/**
 * @brief Get the current BLE connection handle
 * @return Connection handle or 0 if not connected
 */
uint16_t ble_nus_get_conn_handle(void);

/**
 * @brief Deinitialize BLE
 */
void ble_nus_deinit(void);

/**
 * @brief Set binary receive mode
 * @param enabled true to enable binary mode
 * @param callback Callback for binary data (NULL to disable)
 */
void ble_nus_set_binary_mode(bool enabled, ble_binary_rx_callback_t callback);

/**
 * @brief Check if binary mode is active
 * @return true if in binary mode
 */
bool ble_nus_is_binary_mode(void);

/**
 * @brief Get the device name being advertised
 * @return Pointer to device name string
 */
const char* ble_nus_get_device_name(void);

#ifdef __cplusplus
}
#endif

#endif // BLE_NUS_H

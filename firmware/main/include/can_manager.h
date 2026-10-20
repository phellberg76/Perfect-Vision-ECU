/**
 * @file can_manager.h
 * @brief CAN bus manager for dual-channel CAN interface
 *
 * Manages both CAN0 (ESP32 internal TWAI) and CAN1 (MCP2515 via SPI).
 * Received messages are pushed to the shared CAN buffer for BLE transmission.
 */

#ifndef CAN_MANAGER_H
#define CAN_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>

// CAN channel identifiers
typedef enum {
    CAN_CHANNEL_0 = 0,  // Internal TWAI controller
    CAN_CHANNEL_1 = 1,  // MCP2515 external controller
} can_channel_t;

// CAN bitrate options (in bits per second)
typedef enum {
    CAN_BITRATE_125K  = 125000,
    CAN_BITRATE_250K  = 250000,
    CAN_BITRATE_500K  = 500000,
    CAN_BITRATE_1M    = 1000000,
} can_bitrate_t;

/**
 * @brief Initialize the CAN manager
 *
 * Sets up both CAN channels with the specified bitrate.
 * Default bitrate is 250 kbps (common for agricultural equipment).
 *
 * @param bitrate CAN bus bitrate for both channels
 * @return ESP_OK on success
 */
esp_err_t can_manager_init(can_bitrate_t bitrate);

/**
 * @brief Start CAN reception on both channels
 * @return ESP_OK on success
 */
esp_err_t can_manager_start(void);

/**
 * @brief Stop CAN reception
 * @return ESP_OK on success
 */
esp_err_t can_manager_stop(void);

/**
 * @brief Check if CAN manager is running
 * @return true if running
 */
bool can_manager_is_running(void);

/**
 * @brief Get statistics for a CAN channel
 * @param channel CAN channel to query
 * @param rx_count Pointer to store received message count
 * @param error_count Pointer to store error count
 */
void can_manager_get_stats(can_channel_t channel, uint32_t *rx_count, uint32_t *error_count);

/**
 * @brief Reset statistics for all CAN channels
 */
void can_manager_reset_stats(void);

/**
 * @brief Send a CAN message on specified channel
 * @param channel CAN channel to use
 * @param id CAN message ID
 * @param data Pointer to data bytes
 * @param len Data length (0-8)
 * @param extended_id true for 29-bit ID, false for 11-bit
 * @return ESP_OK on success
 */
esp_err_t can_manager_send(can_channel_t channel, uint32_t id,
                           const uint8_t *data, uint8_t len, bool extended_id);

#endif // CAN_MANAGER_H

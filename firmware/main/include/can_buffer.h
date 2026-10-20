/**
 * @file can_buffer.h
 * @brief Thread-safe CAN message buffer for BLE transmission
 *
 * Implements a ring buffer with FreeRTOS synchronization to queue
 * CAN messages from both CAN0 (TWAI) and CAN1 (MCP2515) for BLE transmission.
 */

#ifndef CAN_BUFFER_H
#define CAN_BUFFER_H

#include <stdint.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Buffer size in number of CAN messages
#define CAN_BUFFER_SIZE     256

// CAN message structure for BLE transmission
typedef struct __attribute__((packed)) {
    uint8_t  channel;       // 0 = CAN0 (TWAI), 1 = CAN1 (MCP2515)
    uint32_t timestamp_ms;  // Timestamp in milliseconds since boot
    uint32_t id;            // CAN ID (11-bit or 29-bit)
    uint8_t  flags;         // Bit 0: Extended ID, Bit 1: RTR
    uint8_t  dlc;           // Data Length Code (0-8)
    uint8_t  data[8];       // CAN data bytes
} can_message_t;

// Flag definitions
#define CAN_FLAG_EXTENDED_ID    0x01
#define CAN_FLAG_RTR            0x02

/**
 * @brief Initialize the CAN message buffer
 * @return ESP_OK on success
 */
esp_err_t can_buffer_init(void);

/**
 * @brief Add a CAN message to the buffer
 * @param msg Pointer to the CAN message to add
 * @return true if message was added, false if buffer is full
 */
bool can_buffer_push(const can_message_t *msg);

/**
 * @brief Get the number of messages in the buffer
 * @return Number of messages available
 */
size_t can_buffer_count(void);

/**
 * @brief Read messages from buffer into provided array
 * @param dest Destination array for messages
 * @param max_count Maximum number of messages to read
 * @return Number of messages actually read
 */
size_t can_buffer_read(can_message_t *dest, size_t max_count);

/**
 * @brief Clear all messages from the buffer
 */
void can_buffer_clear(void);

/**
 * @brief Get buffer statistics
 * @param overflow_count Pointer to store overflow count (messages dropped)
 * @param total_count Pointer to store total messages received
 */
void can_buffer_get_stats(uint32_t *overflow_count, uint32_t *total_count);

/**
 * @brief Reset buffer statistics (overflow and total counts)
 */
void can_buffer_reset_stats(void);

#endif // CAN_BUFFER_H

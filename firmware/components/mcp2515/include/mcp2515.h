/**
 * @file mcp2515.h
 * @brief MCP2515 CAN Controller Driver for ESP-IDF
 *
 * Driver for the Microchip MCP2515 stand-alone CAN controller
 * with SPI interface.
 */

#ifndef MCP2515_H
#define MCP2515_H

#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

// MCP2515 handle (opaque pointer)
typedef struct mcp2515_handle *mcp2515_handle_t;

// Configuration structure
typedef struct {
    spi_host_device_t spi_host;
    gpio_num_t mosi_gpio;
    gpio_num_t miso_gpio;
    gpio_num_t clk_gpio;
    gpio_num_t cs_gpio;
    gpio_num_t int_gpio;
    uint32_t spi_freq_hz;
    uint32_t crystal_freq_hz;   // MCP2515 crystal frequency (usually 8MHz or 16MHz)
    uint32_t bitrate;           // CAN bus bitrate in bps
} mcp2515_config_t;

// CAN message structure
typedef struct {
    uint32_t id;        // CAN ID (11-bit or 29-bit)
    uint8_t dlc;        // Data Length Code (0-8)
    uint8_t data[8];    // Data bytes
    bool extended;      // true for extended (29-bit) ID
    bool rtr;           // Remote Transmission Request
} mcp2515_message_t;

// MCP2515 operating modes
typedef enum {
    MCP2515_MODE_NORMAL     = 0x00,
    MCP2515_MODE_SLEEP      = 0x20,
    MCP2515_MODE_LOOPBACK   = 0x40,
    MCP2515_MODE_LISTEN     = 0x60,
    MCP2515_MODE_CONFIG     = 0x80,
} mcp2515_mode_t;

/**
 * @brief Initialize the MCP2515 controller
 *
 * @param config Pointer to configuration structure
 * @param out_handle Pointer to store the handle
 * @return ESP_OK on success
 */
esp_err_t mcp2515_init(const mcp2515_config_t *config, mcp2515_handle_t *out_handle);

/**
 * @brief Deinitialize the MCP2515 controller
 *
 * @param handle MCP2515 handle
 * @return ESP_OK on success
 */
esp_err_t mcp2515_deinit(mcp2515_handle_t handle);

/**
 * @brief Start the MCP2515 in normal mode
 *
 * @param handle MCP2515 handle
 * @return ESP_OK on success
 */
esp_err_t mcp2515_start(mcp2515_handle_t handle);

/**
 * @brief Stop the MCP2515 (enter configuration mode)
 *
 * @param handle MCP2515 handle
 * @return ESP_OK on success
 */
esp_err_t mcp2515_stop(mcp2515_handle_t handle);

/**
 * @brief Send a CAN message
 *
 * @param handle MCP2515 handle
 * @param msg Pointer to message structure
 * @param timeout_ticks Timeout in FreeRTOS ticks
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if no TX buffer available
 */
esp_err_t mcp2515_send(mcp2515_handle_t handle, const mcp2515_message_t *msg,
                       TickType_t timeout_ticks);

/**
 * @brief Receive a CAN message
 *
 * @param handle MCP2515 handle
 * @param msg Pointer to store received message
 * @param timeout_ticks Timeout in FreeRTOS ticks
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if no message available
 */
esp_err_t mcp2515_receive(mcp2515_handle_t handle, mcp2515_message_t *msg,
                          TickType_t timeout_ticks);

/**
 * @brief Set operating mode
 *
 * @param handle MCP2515 handle
 * @param mode Operating mode
 * @return ESP_OK on success
 */
esp_err_t mcp2515_set_mode(mcp2515_handle_t handle, mcp2515_mode_t mode);

/**
 * @brief Reset the MCP2515
 *
 * @param handle MCP2515 handle
 * @return ESP_OK on success
 */
esp_err_t mcp2515_reset(mcp2515_handle_t handle);

/**
 * @brief Read MCP2515 status register
 *
 * @param handle MCP2515 handle
 * @param status Pointer to store status byte
 * @return ESP_OK on success
 */
esp_err_t mcp2515_read_status(mcp2515_handle_t handle, uint8_t *status);

#ifdef __cplusplus
}
#endif

#endif // MCP2515_H

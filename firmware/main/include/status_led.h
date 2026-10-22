/**
 * @file status_led.h
 * @brief Status LED control for Perfect Vision ECU
 *
 * LED states:
 * - Solid ON: Setup complete, waiting for BLE connection
 * - 1Hz blink: BLE connected and operating normally
 * - 10Hz blink: CAN or BLE error detected
 */

#ifndef STATUS_LED_H
#define STATUS_LED_H

#include "esp_err.h"

/**
 * @brief Status LED states
 */
typedef enum {
    STATUS_LED_OFF,         // LED off
    STATUS_LED_SOLID,       // Solid on
    STATUS_LED_BLINK_1HZ,   // 1Hz blink (BLE connected)
    STATUS_LED_BLINK_10HZ,  // 10Hz blink (error)
} status_led_state_t;

/**
 * @brief Initialize the status LED GPIO
 * @return ESP_OK on success
 */
esp_err_t status_led_init(void);

/**
 * @brief Set the status LED state
 * @param state The desired LED state
 */
void status_led_set_state(status_led_state_t state);

/**
 * @brief Get the current status LED state
 * @return Current LED state
 */
status_led_state_t status_led_get_state(void);

/**
 * @brief Signal an error condition (switches to 10Hz blink)
 *
 * The error state will persist for a configurable duration before
 * reverting to the previous state.
 */
void status_led_signal_error(void);

#endif // STATUS_LED_H

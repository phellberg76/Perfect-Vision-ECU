/**
 * @file web_log.h
 * @brief Web-based log viewer via WiFi AP
 *
 * Provides a simple web interface to view ESP32 logs when USB access
 * is unavailable. Starts a WiFi AP and serves logs via HTTP.
 */

#ifndef WEB_LOG_H
#define WEB_LOG_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief Start the web log server
 *
 * Creates a WiFi AP and starts an HTTP server serving logs.
 * AP SSID: PV_LOG_<MAC>
 * AP Password: pvdebug123
 * URL: http://192.168.4.1/
 *
 * @return ESP_OK on success
 */
esp_err_t web_log_start(void);

/**
 * @brief Stop the web log server
 *
 * Stops HTTP server and WiFi AP.
 *
 * @return ESP_OK on success
 */
esp_err_t web_log_stop(void);

/**
 * @brief Check if web log server is running
 *
 * @return true if running
 */
bool web_log_is_running(void);

/**
 * @brief Add a log entry
 *
 * Called by the custom log handler to capture logs.
 *
 * @param level Log level character (E/W/I/D/V)
 * @param tag Log tag
 * @param message Log message
 */
void web_log_add(char level, const char *tag, const char *message);

/**
 * @brief Get the AP SSID
 *
 * @return SSID string
 */
const char* web_log_get_ssid(void);

#endif // WEB_LOG_H

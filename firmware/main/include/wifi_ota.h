/**
 * @file wifi_ota.h
 * @brief WiFi-based OTA firmware update
 *
 * Downloads firmware directly from server over WiFi, bypassing BLE bandwidth limits.
 * WiFi credentials and URL are provided by the Flutter app via BLE command.
 *
 * Flow:
 * 1. App sends wifi_ota command with SSID, password, URL, token
 * 2. ESP32 connects to WiFi
 * 3. ESP32 downloads firmware via HTTPS with Bearer token auth
 * 4. ESP32 validates and applies update
 * 5. ESP32 reboots with new firmware
 */

#ifndef WIFI_OTA_H
#define WIFI_OTA_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi OTA status codes
 */
typedef enum {
    WIFI_OTA_STATUS_IDLE,
    WIFI_OTA_STATUS_CONNECTING_WIFI,
    WIFI_OTA_STATUS_WIFI_CONNECTED,
    WIFI_OTA_STATUS_DOWNLOADING,
    WIFI_OTA_STATUS_VALIDATING,
    WIFI_OTA_STATUS_COMPLETE,
    WIFI_OTA_STATUS_ERROR
} wifi_ota_status_t;

/**
 * @brief WiFi OTA progress callback
 *
 * @param status Current status
 * @param progress Progress percentage (0-100, or -1 for error)
 * @param received Bytes received so far
 * @param total Total bytes expected
 * @param message Human-readable status message
 */
typedef void (*wifi_ota_progress_callback_t)(
    wifi_ota_status_t status,
    int progress,
    size_t received,
    size_t total,
    const char *message
);

/**
 * @brief WiFi OTA configuration
 */
typedef struct {
    const char *ssid;           ///< WiFi SSID (required)
    const char *password;       ///< WiFi password (can be NULL for open networks)
    const char *url;            ///< Firmware download URL (required, HTTPS)
    const char *auth_token;     ///< Bearer token for Authorization header
    size_t expected_size;       ///< Expected firmware size in bytes
    const char *expected_hash;  ///< Expected SHA256 hash (hex string, optional)
} wifi_ota_config_t;

/**
 * @brief Start WiFi OTA update
 *
 * Starts a background task that:
 * 1. Connects to WiFi
 * 2. Downloads firmware from URL
 * 3. Validates and applies update
 * 4. Reboots on success
 *
 * @param config OTA configuration (strings are copied internally)
 * @param callback Progress callback function
 * @return ESP_OK on successful start, error code otherwise
 */
esp_err_t wifi_ota_start(const wifi_ota_config_t *config, wifi_ota_progress_callback_t callback);

/**
 * @brief Abort WiFi OTA update
 */
void wifi_ota_abort(void);

/**
 * @brief Check if WiFi OTA is in progress
 * @return true if OTA is active
 */
bool wifi_ota_is_active(void);

/**
 * @brief Get current WiFi OTA status
 * @return Current status code
 */
wifi_ota_status_t wifi_ota_get_status(void);

// ============================================================================
// WiFi Scanning
// ============================================================================

/**
 * @brief WiFi scan result entry
 */
typedef struct {
    char ssid[33];          ///< Network SSID
    int8_t rssi;            ///< Signal strength in dBm
    uint8_t authmode;       ///< Authentication mode (0=open, else=secured)
} wifi_scan_result_t;

/**
 * @brief WiFi scan callback
 */
typedef void (*wifi_scan_callback_t)(
    const wifi_scan_result_t *results,
    size_t count,
    const char *error_msg
);

/**
 * @brief Start WiFi network scan
 * @param callback Callback to receive results
 * @return ESP_OK on successful start
 */
esp_err_t wifi_scan_start(wifi_scan_callback_t callback);

/**
 * @brief Check if WiFi scan is in progress
 * @return true if scan is active
 */
bool wifi_scan_is_active(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_OTA_H

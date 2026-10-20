/**
 * @file gps.h
 * @brief GPS module interface for Perfect Vision ECU
 *
 * Reads NMEA sentences from the onboard GPS module via UART.
 * For testing, logs raw NMEA data and parsed position/speed.
 */

#ifndef GPS_H
#define GPS_H

#include "esp_err.h"
#include <stdbool.h>

/**
 * @brief GPS fix quality
 */
typedef enum {
    GPS_FIX_INVALID = 0,
    GPS_FIX_GPS = 1,
    GPS_FIX_DGPS = 2,
} gps_fix_t;

/**
 * @brief Parsed GPS data
 */
typedef struct {
    bool valid;              // True if we have a valid fix
    gps_fix_t fix_quality;   // Fix quality from GGA
    double latitude;         // Decimal degrees (positive = N, negative = S)
    double longitude;        // Decimal degrees (positive = E, negative = W)
    float altitude;          // Meters above sea level
    float speed_knots;       // Speed in knots from RMC
    float speed_kmh;         // Speed in km/h (converted)
    float course;            // Course over ground in degrees
    int satellites;          // Number of satellites in use
    float hdop;              // Horizontal dilution of precision
    // Time from GPS
    int hour;
    int minute;
    int second;
    int day;
    int month;
    int year;
} gps_data_t;

/**
 * @brief Initialize the GPS module
 *
 * Sets up UART and starts the GPS reading task.
 *
 * @return ESP_OK on success
 */
esp_err_t gps_init(void);

/**
 * @brief Get the latest GPS data
 *
 * @param data Pointer to store GPS data
 * @return true if data is valid, false otherwise
 */
bool gps_get_data(gps_data_t *data);

/**
 * @brief Check if GPS has a valid fix
 *
 * @return true if GPS has valid position data
 */
bool gps_has_fix(void);

/**
 * @brief Enable/disable raw NMEA logging
 *
 * @param enable true to log raw NMEA sentences
 */
void gps_set_raw_logging(bool enable);

#endif // GPS_H

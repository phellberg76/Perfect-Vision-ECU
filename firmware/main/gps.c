/**
 * @file gps.c
 * @brief GPS module implementation for Perfect Vision ECU
 *
 * Reads NMEA sentences from the GPS module via UART and parses
 * GGA (position) and RMC (speed/course) sentences.
 */

#include "gps.h"
#include "pin_config.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "gps";

// UART buffer sizes
#define GPS_UART_BUF_SIZE       512
#define GPS_LINE_BUF_SIZE       128

// GPS data (protected by mutex in a real implementation)
static gps_data_t s_gps_data = {0};
static bool s_raw_logging = false;

// Task handle
static TaskHandle_t s_gps_task_handle = NULL;

// -----------------------------------------------------------------------------
// NMEA Parsing Helpers
// -----------------------------------------------------------------------------

static double parse_latitude(const char *str, const char *dir)
{
    if (str == NULL || strlen(str) < 4) {
        return 0.0;
    }

    double raw = atof(str);
    int degrees = (int)(raw / 100);
    double minutes = raw - (degrees * 100);
    double decimal = degrees + (minutes / 60.0);

    if (dir && (dir[0] == 'S' || dir[0] == 's')) {
        decimal = -decimal;
    }

    return decimal;
}

static double parse_longitude(const char *str, const char *dir)
{
    if (str == NULL || strlen(str) < 5) {
        return 0.0;
    }

    double raw = atof(str);
    int degrees = (int)(raw / 100);
    double minutes = raw - (degrees * 100);
    double decimal = degrees + (minutes / 60.0);

    if (dir && (dir[0] == 'W' || dir[0] == 'w')) {
        decimal = -decimal;
    }

    return decimal;
}

static char* get_field(char **sentence)
{
    if (*sentence == NULL) {
        return NULL;
    }

    char *start = *sentence;
    char *comma = strchr(start, ',');

    if (comma) {
        *comma = '\0';
        *sentence = comma + 1;
    } else {
        char *end = strchr(start, '*');
        if (end) {
            *end = '\0';
        }
        *sentence = NULL;
    }

    return start;
}

static void parse_gga(char *sentence)
{
    char *ptr = sentence;
    char *field;

    // Skip header ($GPGGA or $GNGGA)
    field = get_field(&ptr);
    if (field == NULL) return;

    // Time (HHMMSS.ss)
    field = get_field(&ptr);
    if (field && strlen(field) >= 6) {
        s_gps_data.hour = (field[0] - '0') * 10 + (field[1] - '0');
        s_gps_data.minute = (field[2] - '0') * 10 + (field[3] - '0');
        s_gps_data.second = (field[4] - '0') * 10 + (field[5] - '0');
    }

    // Latitude
    char *lat = get_field(&ptr);
    char *lat_dir = get_field(&ptr);
    if (lat && lat_dir && strlen(lat) > 0) {
        s_gps_data.latitude = parse_latitude(lat, lat_dir);
    }

    // Longitude
    char *lon = get_field(&ptr);
    char *lon_dir = get_field(&ptr);
    if (lon && lon_dir && strlen(lon) > 0) {
        s_gps_data.longitude = parse_longitude(lon, lon_dir);
    }

    // Fix quality
    field = get_field(&ptr);
    if (field && strlen(field) > 0) {
        s_gps_data.fix_quality = (gps_fix_t)atoi(field);
        s_gps_data.valid = (s_gps_data.fix_quality > 0);
    }

    // Number of satellites
    field = get_field(&ptr);
    if (field && strlen(field) > 0) {
        s_gps_data.satellites = atoi(field);
    }

    // HDOP
    field = get_field(&ptr);
    if (field && strlen(field) > 0) {
        s_gps_data.hdop = atof(field);
    }

    // Altitude
    field = get_field(&ptr);
    if (field && strlen(field) > 0) {
        s_gps_data.altitude = atof(field);
    }
}

static void parse_rmc(char *sentence)
{
    char *ptr = sentence;
    char *field;

    // Skip header ($GPRMC or $GNRMC)
    field = get_field(&ptr);
    if (field == NULL) return;

    // Time (already parsed from GGA)
    get_field(&ptr);

    // Status (A=active, V=void)
    field = get_field(&ptr);
    bool active = (field && field[0] == 'A');
    if (!active) {
        return;
    }

    // Skip lat/lon (we use GGA for position)
    get_field(&ptr);  // lat
    get_field(&ptr);  // lat dir
    get_field(&ptr);  // lon
    get_field(&ptr);  // lon dir

    // Speed in knots
    field = get_field(&ptr);
    if (field && strlen(field) > 0) {
        s_gps_data.speed_knots = atof(field);
        s_gps_data.speed_kmh = s_gps_data.speed_knots * 1.852f;
    }

    // Course over ground
    field = get_field(&ptr);
    if (field && strlen(field) > 0) {
        s_gps_data.course = atof(field);
    }

    // Date (DDMMYY)
    field = get_field(&ptr);
    if (field && strlen(field) >= 6) {
        s_gps_data.day = (field[0] - '0') * 10 + (field[1] - '0');
        s_gps_data.month = (field[2] - '0') * 10 + (field[3] - '0');
        s_gps_data.year = 2000 + (field[4] - '0') * 10 + (field[5] - '0');
    }
}

static void process_nmea_sentence(char *sentence)
{
    if (sentence == NULL || strlen(sentence) < 6) {
        return;
    }

    if (s_raw_logging) {
        ESP_LOGI(TAG, "NMEA: %s", sentence);
    }

    if (sentence[0] != '$') {
        return;
    }

    char buf[GPS_LINE_BUF_SIZE];
    strncpy(buf, sentence, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    if (strstr(buf, "GGA") != NULL) {
        parse_gga(buf);
    } else if (strstr(buf, "RMC") != NULL) {
        parse_rmc(buf);
    }
}

// -----------------------------------------------------------------------------
// GPS Reading Task
// -----------------------------------------------------------------------------

static void gps_task(void *arg)
{
    char line_buf[GPS_LINE_BUF_SIZE];
    int line_pos = 0;
    uint8_t rx_buf[64];

    ESP_LOGI(TAG, "GPS task started");

    TickType_t last_status = xTaskGetTickCount();

    while (1) {
        int len = uart_read_bytes(GPS_UART_NUM, rx_buf, sizeof(rx_buf), pdMS_TO_TICKS(100));

        if (len > 0) {
            for (int i = 0; i < len; i++) {
                uint8_t byte = rx_buf[i];

                if (byte == '\n' || byte == '\r') {
                    if (line_pos > 0) {
                        line_buf[line_pos] = '\0';
                        process_nmea_sentence(line_buf);
                        line_pos = 0;
                    }
                } else if (line_pos < GPS_LINE_BUF_SIZE - 1) {
                    line_buf[line_pos++] = byte;
                } else {
                    line_pos = 0;
                }
            }
        }

        // GPS status every 10 seconds
        TickType_t now = xTaskGetTickCount();
        if ((now - last_status) > pdMS_TO_TICKS(10000)) {
            last_status = now;
            if (s_gps_data.valid) {
                ESP_LOGI(TAG, "GPS: lat=%.6f lon=%.6f speed=%.1f km/h sats=%d",
                         s_gps_data.latitude, s_gps_data.longitude,
                         s_gps_data.speed_kmh, s_gps_data.satellites);
            } else {
                ESP_LOGD(TAG, "GPS: waiting for fix...");
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

esp_err_t gps_init(void)
{
    ESP_LOGI(TAG, "Initializing GPS (UART%d, RX=GPIO%d, %d baud)",
             GPS_UART_NUM, GPS_RX_GPIO, GPS_BAUD_RATE);

    // Enable LNA power for active antenna
    gpio_config_t lna_conf = {
        .pin_bit_mask = (1ULL << GPS_LNA_EN_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&lna_conf);
    gpio_set_level(GPS_LNA_EN_GPIO, 1);

    // Configure UART
    uart_config_t uart_config = {
        .baud_rate = GPS_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(GPS_UART_NUM, GPS_UART_BUF_SIZE, 0, 0, NULL, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "UART driver install failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_param_config(GPS_UART_NUM, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = uart_set_pin(GPS_UART_NUM, GPS_TX_GPIO, GPS_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART set pin failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create GPS reading task
    BaseType_t ret = xTaskCreate(gps_task, "gps", 4096, NULL, 3, &s_gps_task_handle);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPS task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GPS initialized");
    return ESP_OK;
}

bool gps_get_data(gps_data_t *data)
{
    if (data == NULL) {
        return false;
    }

    memcpy(data, &s_gps_data, sizeof(gps_data_t));
    return s_gps_data.valid;
}

bool gps_has_fix(void)
{
    return s_gps_data.valid;
}

void gps_set_raw_logging(bool enable)
{
    s_raw_logging = enable;
    ESP_LOGI(TAG, "Raw NMEA logging %s", enable ? "enabled" : "disabled");
}

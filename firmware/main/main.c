/**
 * @file main.c
 * @brief Perfect Vision ECU - CAN/BLE Bridge Main Application
 *
 * This firmware provides a CAN-to-BLE bridge functionality for the
 * Perfect Vision ECU. It reads CAN messages from two channels:
 * - CAN0: ESP32-S3 internal TWAI controller
 * - CAN1: MCP2515 external SPI CAN controller
 *
 * Messages are buffered and streamed via BLE (Nordic UART Service) to
 * a connected Flutter app. OTA firmware updates are supported via WiFi,
 * triggered by BLE commands from the app.
 *
 * GPS Integration Status
 * ----------------------
 * Basic GPS reading and NMEA parsing is implemented (gps.c):
 * - UART reading from GPS module (TXD0/RXD0 - see gps.kicad_sch)
 * - GGA parsing for position, fix quality, satellites
 * - RMC parsing for speed and course
 * - Raw NMEA logging enabled for testing
 *
 * TODO: Remaining GPS tasks:
 * - Add GPS data to BLE stream alongside CAN messages
 * - Add BLE command to enable/disable GPS streaming
 * - Use GPS as fallback when ISOBUS position data is unavailable
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "cJSON.h"

#include "driver/gpio.h"
#include "driver/uart.h"

#include "config.h"
#include "pin_config.h"
#include "can_manager.h"
#include "can_buffer.h"
#include "ble_nus.h"
#include "wifi_ota.h"
#include "gps.h"

static const char *TAG = "main";

// Cached firmware hash (SHA256)
static char s_firmware_hash[65] = {0};

// Current request ID (for including in responses)
static int s_current_request_id = -1;

// WiFi scan request ID (saved because scan is async)
static int s_wifi_scan_request_id = -1;

// Forward declarations
static void on_ble_receive(const char *data, size_t len);
static void on_ble_connection_change(bool connected);
static void on_wifi_ota_progress(wifi_ota_status_t status, int progress,
                                  size_t received, size_t total, const char *message);
static void on_wifi_scan_result(const wifi_scan_result_t *results, size_t count,
                                 const char *error_msg);

// -----------------------------------------------------------------------------
// Response Helpers
// -----------------------------------------------------------------------------

static void send_json_response(cJSON *json)
{
    // Include request ID in response if available
    if (s_current_request_id >= 0) {
        cJSON_AddNumberToObject(json, "id", s_current_request_id);
    }

    char *str = cJSON_PrintUnformatted(json);
    if (str) {
        ble_nus_send(str, 0);
        free(str);
    }
    cJSON_Delete(json);
}

static void send_error(int code, const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", "error");
    cJSON_AddNumberToObject(json, "code", code);
    cJSON_AddStringToObject(json, "message", message);
    send_json_response(json);
}

static void send_ok(const char *message)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", "ok");
    if (message) {
        cJSON_AddStringToObject(json, "message", message);
    }
    send_json_response(json);
}

// -----------------------------------------------------------------------------
// Firmware Hash
// -----------------------------------------------------------------------------

static const char* get_firmware_hash(void)
{
    if (s_firmware_hash[0] != '\0') {
        return s_firmware_hash;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        return "unknown";
    }

    esp_app_desc_t app_desc;
    if (esp_ota_get_partition_description(running, &app_desc) != ESP_OK) {
        return "unknown";
    }

    // Convert SHA256 to hex string
    for (int i = 0; i < 32; i++) {
        snprintf(&s_firmware_hash[i * 2], 3, "%02x", app_desc.app_elf_sha256[i]);
    }
    s_firmware_hash[64] = '\0';

    return s_firmware_hash;
}

// -----------------------------------------------------------------------------
// BLE Command Handlers
// -----------------------------------------------------------------------------

static void handle_version_cmd(void)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", "version");
    cJSON_AddStringToObject(json, "version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(json, "model", DEVICE_MODEL);
    cJSON_AddStringToObject(json, "hash", get_firmware_hash());
    send_json_response(json);
}

static void handle_status_cmd(void)
{
    uint32_t can0_rx, can0_err, can1_rx, can1_err;
    uint32_t overflow, total;

    can_manager_get_stats(CAN_CHANNEL_0, &can0_rx, &can0_err);
    can_manager_get_stats(CAN_CHANNEL_1, &can1_rx, &can1_err);
    can_buffer_get_stats(&overflow, &total);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", "status");
    cJSON_AddBoolToObject(json, "can_running", can_manager_is_running());
    cJSON_AddNumberToObject(json, "can0_rx", can0_rx);
    cJSON_AddNumberToObject(json, "can0_err", can0_err);
    cJSON_AddNumberToObject(json, "can1_rx", can1_rx);
    cJSON_AddNumberToObject(json, "can1_err", can1_err);
    cJSON_AddNumberToObject(json, "buffer_count", can_buffer_count());
    cJSON_AddNumberToObject(json, "buffer_overflow", overflow);
    cJSON_AddNumberToObject(json, "total_messages", total);
    send_json_response(json);
}

static void handle_can_start_cmd(void)
{
    if (can_manager_is_running()) {
        send_ok("CAN already running");
        return;
    }

    esp_err_t err = can_manager_start();
    if (err == ESP_OK) {
        send_ok("CAN started");
    } else {
        send_error(40, "Failed to start CAN");
    }
}

static void handle_can_stop_cmd(void)
{
    can_manager_stop();
    send_ok("CAN stopped");
}

static void handle_clear_buffer_cmd(void)
{
    can_buffer_clear();
    send_ok("Buffer cleared");
}

static void handle_clear_stats_cmd(void)
{
    can_manager_reset_stats();
    can_buffer_reset_stats();
    send_ok("Statistics cleared");
}

static void handle_wifi_scan_cmd(void)
{
    if (wifi_ota_is_active()) {
        send_error(50, "OTA in progress");
        return;
    }

    if (wifi_scan_is_active()) {
        send_error(51, "Scan already in progress");
        return;
    }

    // Save the request ID for the async callback
    s_wifi_scan_request_id = s_current_request_id;

    esp_err_t err = wifi_scan_start(on_wifi_scan_result);
    if (err != ESP_OK) {
        s_wifi_scan_request_id = -1;
        send_error(52, "Failed to start WiFi scan");
    }
    // Results sent via callback
}

static void handle_wifi_ota_cmd(cJSON *json)
{
    if (wifi_ota_is_active()) {
        send_error(60, "OTA already in progress");
        return;
    }

    cJSON *ssid_item = cJSON_GetObjectItem(json, "ssid");
    cJSON *password_item = cJSON_GetObjectItem(json, "password");
    cJSON *url_item = cJSON_GetObjectItem(json, "url");
    cJSON *token_item = cJSON_GetObjectItem(json, "token");
    cJSON *size_item = cJSON_GetObjectItem(json, "size");
    cJSON *hash_item = cJSON_GetObjectItem(json, "hash");

    if (!cJSON_IsString(ssid_item) || !cJSON_IsString(url_item)) {
        send_error(61, "Missing ssid or url");
        return;
    }

    wifi_ota_config_t config = {
        .ssid = ssid_item->valuestring,
        .password = cJSON_IsString(password_item) ? password_item->valuestring : NULL,
        .url = url_item->valuestring,
        .auth_token = cJSON_IsString(token_item) ? token_item->valuestring : NULL,
        .expected_size = cJSON_IsNumber(size_item) ? (size_t)size_item->valuedouble : 0,
        .expected_hash = cJSON_IsString(hash_item) ? hash_item->valuestring : NULL,
    };

    esp_err_t err = wifi_ota_start(&config, on_wifi_ota_progress);
    if (err != ESP_OK) {
        send_error(62, "Failed to start OTA");
    }
    // Progress sent via callback
}

static void handle_wifi_ota_abort_cmd(void)
{
    if (!wifi_ota_is_active()) {
        send_error(63, "No OTA in progress");
        return;
    }

    wifi_ota_abort();
    send_ok("OTA abort requested");
}

static void handle_reboot_cmd(void)
{
    ESP_LOGI(TAG, "Reboot requested via BLE");
    send_ok("Rebooting...");

    // Give time for the response to be sent
    vTaskDelay(pdMS_TO_TICKS(500));

    esp_restart();
}

// -----------------------------------------------------------------------------
// BLE Callbacks
// -----------------------------------------------------------------------------

static void on_ble_receive(const char *data, size_t len)
{
    ESP_LOGI(TAG, "BLE RX: %.*s", (int)len, data);

    // Reset request ID
    s_current_request_id = -1;

    cJSON *json = cJSON_ParseWithLength(data, len);
    if (json == NULL) {
        ESP_LOGW(TAG, "Invalid JSON received");
        send_error(1, "Invalid JSON");
        return;
    }

    // Extract request ID if present
    cJSON *id_item = cJSON_GetObjectItem(json, "id");
    if (cJSON_IsNumber(id_item)) {
        s_current_request_id = (int)id_item->valuedouble;
    }

    cJSON *cmd_item = cJSON_GetObjectItem(json, "cmd");
    if (!cJSON_IsString(cmd_item)) {
        ESP_LOGW(TAG, "Missing 'cmd' field");
        send_error(2, "Missing cmd");
        cJSON_Delete(json);
        return;
    }

    const char *cmd = cmd_item->valuestring;

    if (strcmp(cmd, "version") == 0) {
        handle_version_cmd();
    }
    else if (strcmp(cmd, "status") == 0) {
        handle_status_cmd();
    }
    else if (strcmp(cmd, "can_start") == 0) {
        handle_can_start_cmd();
    }
    else if (strcmp(cmd, "can_stop") == 0) {
        handle_can_stop_cmd();
    }
    else if (strcmp(cmd, "clear_buffer") == 0) {
        handle_clear_buffer_cmd();
    }
    else if (strcmp(cmd, "clear_stats") == 0) {
        handle_clear_stats_cmd();
    }
    else if (strcmp(cmd, "wifi_scan") == 0) {
        handle_wifi_scan_cmd();
    }
    else if (strcmp(cmd, "wifi_ota") == 0) {
        handle_wifi_ota_cmd(json);
    }
    else if (strcmp(cmd, "wifi_ota_abort") == 0) {
        handle_wifi_ota_abort_cmd();
    }
    else if (strcmp(cmd, "reboot") == 0) {
        handle_reboot_cmd();
    }
    else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd);
        send_error(3, "Unknown command");
    }

    cJSON_Delete(json);
}

static void on_ble_connection_change(bool connected)
{
    ESP_LOGI(TAG, "BLE %s", connected ? "connected" : "disconnected");

    if (!connected) {
        // Abort WiFi OTA if disconnected
        if (wifi_ota_is_active()) {
            ESP_LOGW(TAG, "BLE disconnected during OTA, aborting...");
            wifi_ota_abort();
        }
    }
}

// -----------------------------------------------------------------------------
// WiFi OTA Callbacks
// -----------------------------------------------------------------------------

static void on_wifi_ota_progress(wifi_ota_status_t status, int progress,
                                  size_t received, size_t total, const char *message)
{
    const char *status_str;
    switch (status) {
        case WIFI_OTA_STATUS_CONNECTING_WIFI: status_str = "connecting_wifi"; break;
        case WIFI_OTA_STATUS_WIFI_CONNECTED:  status_str = "wifi_connected"; break;
        case WIFI_OTA_STATUS_DOWNLOADING:     status_str = "downloading"; break;
        case WIFI_OTA_STATUS_VALIDATING:      status_str = "validating"; break;
        case WIFI_OTA_STATUS_COMPLETE:        status_str = "complete"; break;
        case WIFI_OTA_STATUS_ERROR:           status_str = "error"; break;
        default:                              status_str = "idle"; break;
    }

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", "wifi_ota_progress");
    cJSON_AddStringToObject(json, "status", status_str);
    cJSON_AddNumberToObject(json, "progress", progress);
    cJSON_AddNumberToObject(json, "received", received);
    cJSON_AddNumberToObject(json, "total", total);
    cJSON_AddStringToObject(json, "message", message ? message : "");
    send_json_response(json);
}

static void on_wifi_scan_result(const wifi_scan_result_t *results, size_t count,
                                 const char *error_msg)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "type", "wifi_scan_result");

    // Use saved request ID (not current, since scan is async)
    if (s_wifi_scan_request_id >= 0) {
        cJSON_AddNumberToObject(json, "id", s_wifi_scan_request_id);
        s_wifi_scan_request_id = -1;  // Clear after use
    }

    if (error_msg) {
        cJSON_AddBoolToObject(json, "success", false);
        cJSON_AddStringToObject(json, "error", error_msg);
    } else {
        cJSON_AddBoolToObject(json, "success", true);
        cJSON_AddNumberToObject(json, "count", count);

        cJSON *networks = cJSON_CreateArray();
        for (size_t i = 0; i < count; i++) {
            cJSON *net = cJSON_CreateObject();
            cJSON_AddStringToObject(net, "ssid", results[i].ssid);
            cJSON_AddNumberToObject(net, "rssi", results[i].rssi);
            cJSON_AddBoolToObject(net, "secured", results[i].authmode != 0);
            cJSON_AddItemToArray(networks, net);
        }
        cJSON_AddItemToObject(json, "networks", networks);
    }

    // Send directly without using send_json_response (which would add wrong id)
    char *str = cJSON_PrintUnformatted(json);
    if (str) {
        ble_nus_send(str, 0);
        free(str);
    }
    cJSON_Delete(json);
}

// -----------------------------------------------------------------------------
// CAN Data Streaming Task
// -----------------------------------------------------------------------------

static void can_stream_task(void *arg)
{
    ESP_LOGI(TAG, "CAN streaming task started");

    while (1) {
        if (ble_nus_is_connected() && can_manager_is_running()) {
            size_t count = can_buffer_count();
            if (count > 0) {
                // Read up to 8 messages at a time
                can_message_t msgs[8];
                size_t read_count = can_buffer_read(msgs, 8);

                if (read_count > 0) {
                    // Send on dedicated CAN characteristic (UUID ...0004)
                    ble_nus_send_can((uint8_t*)msgs, read_count * sizeof(can_message_t));
                    vTaskDelay(pdMS_TO_TICKS(5));
                } else {
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

// -----------------------------------------------------------------------------
// Status Monitoring Task
// -----------------------------------------------------------------------------

static void status_task(void *arg)
{
    ESP_LOGI(TAG, "Status monitoring task started");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000));  // Every 30 seconds

        uint32_t can0_rx, can0_err, can1_rx, can1_err;
        uint32_t overflow, total;

        can_manager_get_stats(CAN_CHANNEL_0, &can0_rx, &can0_err);
        can_manager_get_stats(CAN_CHANNEL_1, &can1_rx, &can1_err);
        can_buffer_get_stats(&overflow, &total);

        ESP_LOGI(TAG, "CAN0: RX=%lu ERR=%lu | CAN1: RX=%lu ERR=%lu | Overflow=%lu Total=%lu | BLE=%s",
                 (unsigned long)can0_rx, (unsigned long)can0_err,
                 (unsigned long)can1_rx, (unsigned long)can1_err,
                 (unsigned long)overflow, (unsigned long)total,
                 ble_nus_is_connected() ? "connected" : "disconnected");
    }
}

// -----------------------------------------------------------------------------
// Main Application
// -----------------------------------------------------------------------------

void app_main(void)
{
    ESP_LOGI(TAG, "===========================================");
    ESP_LOGI(TAG, "  Perfect Vision ECU - CAN/BLE Bridge");
    ESP_LOGI(TAG, "  Firmware: %s", FIRMWARE_VERSION);
    ESP_LOGI(TAG, "===========================================");

    // Initialize NVS (required for BLE and WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Mark current firmware as valid (for rollback protection)
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
            ESP_LOGI(TAG, "First boot after OTA, marking firmware as valid");
            esp_ota_mark_app_valid_cancel_rollback();
        }
    }

    // Initialize relay control GPIO (may be needed for CAN channel routing)
    gpio_config_t relay_conf = {
        .pin_bit_mask = (1ULL << RELAY_CONTROL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&relay_conf);
    gpio_set_level(RELAY_CONTROL_GPIO, 0);
    ESP_LOGI(TAG, "Relay control GPIO %d initialized (level=0)", RELAY_CONTROL_GPIO);

    // Initialize GPS module (for testing/logging)
    ret = gps_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GPS init failed: %s (continuing without GPS)", esp_err_to_name(ret));
    }

    // Initialize CAN manager
    ret = can_manager_init(CAN_BITRATE_250K);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CAN manager init failed: %s", esp_err_to_name(ret));
        return;
    }

    // Initialize BLE NUS
    ret = ble_nus_init(BLE_DEVICE_NAME_PREFIX, on_ble_receive);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE NUS init failed: %s", esp_err_to_name(ret));
        return;
    }
    ble_nus_set_connection_callback(on_ble_connection_change);

    ESP_LOGI(TAG, "Device name: %s", ble_nus_get_device_name());
    ESP_LOGI(TAG, "Firmware hash: %s", get_firmware_hash());

    // Start CAN reception
    ret = can_manager_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CAN manager start failed: %s", esp_err_to_name(ret));
        return;
    }

    // Create tasks
    xTaskCreate(can_stream_task, "can_stream", 4096, NULL, 5, NULL);
    xTaskCreate(status_task, "status", 2048, NULL, 1, NULL);

    ESP_LOGI(TAG, "System initialized successfully");
    ESP_LOGI(TAG, "CAN0: TWAI (TX=%d, RX=%d)", CAN0_TX_GPIO, CAN0_RX_GPIO);
    ESP_LOGI(TAG, "CAN1: MCP2515 (CS=%d, INT=%d)", MCP2515_CS_GPIO, MCP2515_INT_GPIO);
    ESP_LOGI(TAG, "GPS: UART%d (TX=%d, RX=%d)", GPS_UART_NUM, GPS_TX_GPIO, GPS_RX_GPIO);
    ESP_LOGI(TAG, "Waiting for BLE connection...");
}

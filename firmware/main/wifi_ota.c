/**
 * @file wifi_ota.c
 * @brief WiFi-based OTA firmware update implementation
 *
 * Downloads firmware directly from server over WiFi using esp_https_ota.
 * WiFi is temporarily enabled only for the OTA process.
 */

#include "wifi_ota.h"
#include "config.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"

static const char *TAG = "WIFI_OTA";

// WiFi connection event bits
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

// Connection timeout
#define WIFI_CONNECT_TIMEOUT_MS  15000

// State
static volatile bool s_active = false;
static volatile bool s_abort_requested = false;
static wifi_ota_status_t s_status = WIFI_OTA_STATUS_IDLE;
static wifi_ota_progress_callback_t s_callback = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static bool s_wifi_initialized = false;

// Configuration storage
static char s_ssid[33];
static char s_password[65];
static char s_url[512];
static char s_auth_token[2048];
static size_t s_expected_size = 0;
static char s_expected_hash[65];

// Task handle
static TaskHandle_t s_ota_task = NULL;

// Progress tracking
static size_t s_bytes_received = 0;

// Retry counter
static int s_retry_count = 0;
#define WIFI_MAX_RETRIES 3

// Scan mode flag
static volatile bool s_scan_only_mode = false;

// Forward declarations
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data);
static void wifi_ota_task(void *arg);
static esp_err_t init_wifi(void);
static esp_err_t connect_wifi(void);
static void cleanup_wifi(void);
static esp_err_t download_and_apply_ota(void);

static void report_progress(wifi_ota_status_t status, int progress,
                           size_t received, size_t total, const char *message)
{
    s_status = status;
    s_bytes_received = received;

    ESP_LOGI(TAG, "OTA: %s (progress: %d%%, %zu/%zu bytes)",
             message, progress, received, total);

    if (s_callback) {
        s_callback(status, progress, received, total, message);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                if (!s_scan_only_mode) {
                    ESP_LOGI(TAG, "WiFi STA started, connecting...");
                    esp_wifi_connect();
                } else {
                    ESP_LOGI(TAG, "WiFi STA started (scan mode)");
                }
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                if (s_scan_only_mode) break;

                wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "WiFi disconnected, reason: %d", event->reason);

                if (s_retry_count < WIFI_MAX_RETRIES && !s_abort_requested) {
                    s_retry_count++;
                    ESP_LOGI(TAG, "Retrying connection (%d/%d)...", s_retry_count, WIFI_MAX_RETRIES);
                    esp_wifi_connect();
                } else {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                }
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t init_wifi(void)
{
    if (s_wifi_initialized) {
        return ESP_OK;
    }

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(err));
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi STA netif");
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init WiFi: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    s_wifi_initialized = true;
    return ESP_OK;
}

static esp_err_t connect_wifi(void)
{
    wifi_config_t wifi_config = {0};

    memcpy(wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid));
    memcpy(wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = strlen(s_password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Waiting for WiFi connection to '%s'...", s_ssid);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdTRUE,
        pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "WiFi connection failed");
        return ESP_ERR_WIFI_NOT_CONNECT;
    } else {
        ESP_LOGE(TAG, "WiFi connection timeout");
        return ESP_ERR_TIMEOUT;
    }
}

static void cleanup_wifi(void)
{
    ESP_LOGI(TAG, "Cleaning up WiFi...");

    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_wifi_initialized = false;
}

// HTTP client handle
static esp_http_client_handle_t s_http_client = NULL;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGI(TAG, "HTTP connected");
            if (evt->client && strlen(s_auth_token) > 0) {
                char auth_header[2100];
                snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_auth_token);
                esp_http_client_set_header(evt->client, "Authorization", auth_header);
                ESP_LOGI(TAG, "Authorization header set");
            }
            s_http_client = evt->client;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP disconnected");
            s_http_client = NULL;
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t download_and_apply_ota(void)
{
    esp_http_client_config_t http_config = {
        .url = s_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .event_handler = http_event_handler,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
        report_progress(WIFI_OTA_STATUS_ERROR, -1, 0, s_expected_size,
                       "Failed to start download");
        return err;
    }

    if (s_http_client) {
        int content_length = esp_http_client_get_content_length(s_http_client);
        if (content_length > 0) {
            s_expected_size = content_length;
            ESP_LOGI(TAG, "Firmware size from server: %zu bytes", s_expected_size);
        }
    }

    int last_progress = -1;

    while (true) {
        if (s_abort_requested) {
            ESP_LOGW(TAG, "OTA aborted by user");
            esp_https_ota_abort(ota_handle);
            report_progress(WIFI_OTA_STATUS_ERROR, -1, s_bytes_received, s_expected_size,
                           "Update cancelled");
            return ESP_ERR_INVALID_STATE;
        }

        err = esp_https_ota_perform(ota_handle);

        if (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            s_bytes_received = esp_https_ota_get_image_len_read(ota_handle);

            int progress = 5;
            if (s_expected_size > 0) {
                progress = 5 + (int)(s_bytes_received * 90 / s_expected_size);
                if (progress > 95) progress = 95;
            }

            if (progress != last_progress && (progress % 5 == 0 || progress == 95)) {
                char msg[64];
                snprintf(msg, sizeof(msg), "Downloading %zu / %zu KB",
                        s_bytes_received / 1024, s_expected_size / 1024);
                report_progress(WIFI_OTA_STATUS_DOWNLOADING, progress,
                               s_bytes_received, s_expected_size, msg);
                last_progress = progress;
            }
            continue;
        }

        if (err == ESP_OK) {
            break;
        }

        ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
        esp_https_ota_abort(ota_handle);

        const char *error_msg = "Download failed";
        if (err == ESP_ERR_HTTP_CONNECT) {
            error_msg = "Failed to connect to server";
        } else if (err == ESP_ERR_HTTP_FETCH_HEADER) {
            error_msg = "Server error or firmware not found";
        }

        report_progress(WIFI_OTA_STATUS_ERROR, -1, s_bytes_received, s_expected_size, error_msg);
        return err;
    }

    s_bytes_received = esp_https_ota_get_image_len_read(ota_handle);
    report_progress(WIFI_OTA_STATUS_VALIDATING, 98, s_bytes_received, s_expected_size,
                   "Validating firmware...");

    if (!esp_https_ota_is_complete_data_received(ota_handle)) {
        ESP_LOGE(TAG, "Incomplete firmware received");
        esp_https_ota_abort(ota_handle);
        report_progress(WIFI_OTA_STATUS_ERROR, -1, s_bytes_received, s_expected_size,
                       "Incomplete download");
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_https_ota_finish(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));

        const char *error_msg = "Firmware validation failed";
        if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
            error_msg = "Invalid firmware image";
        }

        report_progress(WIFI_OTA_STATUS_ERROR, -1, s_bytes_received, s_expected_size, error_msg);
        return err;
    }

    ESP_LOGI(TAG, "OTA update successful!");
    return ESP_OK;
}

static void wifi_ota_task(void *arg)
{
    esp_err_t err;

    ESP_LOGI(TAG, "WiFi OTA task started");
    ESP_LOGI(TAG, "SSID: %s, URL: %s", s_ssid, s_url);

    report_progress(WIFI_OTA_STATUS_CONNECTING_WIFI, 0, 0, s_expected_size,
                   "Connecting to WiFi...");

    err = init_wifi();
    if (err != ESP_OK) {
        report_progress(WIFI_OTA_STATUS_ERROR, -1, 0, s_expected_size,
                       "Failed to initialize WiFi");
        goto cleanup;
    }

    if (s_abort_requested) {
        report_progress(WIFI_OTA_STATUS_ERROR, -1, 0, s_expected_size, "Cancelled");
        goto cleanup;
    }

    err = connect_wifi();
    if (err != ESP_OK) {
        const char *msg = "WiFi connection failed";
        if (err == ESP_ERR_TIMEOUT) {
            msg = "WiFi connection timeout";
        } else if (err == ESP_ERR_WIFI_NOT_CONNECT) {
            msg = "WiFi authentication failed - check password";
        }
        report_progress(WIFI_OTA_STATUS_ERROR, -1, 0, s_expected_size, msg);
        goto cleanup;
    }

    if (s_abort_requested) {
        report_progress(WIFI_OTA_STATUS_ERROR, -1, 0, s_expected_size, "Cancelled");
        goto cleanup;
    }

    char ip_msg[64];
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(s_sta_netif, &ip_info);
    snprintf(ip_msg, sizeof(ip_msg), "WiFi connected, IP: " IPSTR, IP2STR(&ip_info.ip));
    report_progress(WIFI_OTA_STATUS_WIFI_CONNECTED, 5, 0, s_expected_size, ip_msg);

    vTaskDelay(pdMS_TO_TICKS(500));

    err = download_and_apply_ota();
    if (err != ESP_OK) {
        goto cleanup;
    }

    report_progress(WIFI_OTA_STATUS_COMPLETE, 100, s_bytes_received, s_expected_size,
                   "Update complete, rebooting...");

    cleanup_wifi();

    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();

cleanup:
    cleanup_wifi();
    s_active = false;
    s_ota_task = NULL;
    ESP_LOGI(TAG, "WiFi OTA task exiting");
    vTaskDelete(NULL);
}

esp_err_t wifi_ota_start(const wifi_ota_config_t *config, wifi_ota_progress_callback_t callback)
{
    if (s_active) {
        ESP_LOGW(TAG, "WiFi OTA already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (config == NULL) {
        ESP_LOGE(TAG, "Config is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (config->ssid == NULL || strlen(config->ssid) == 0) {
        ESP_LOGE(TAG, "SSID is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (config->url == NULL || strlen(config->url) == 0) {
        ESP_LOGE(TAG, "URL is required");
        return ESP_ERR_INVALID_ARG;
    }

    memset(s_ssid, 0, sizeof(s_ssid));
    memset(s_password, 0, sizeof(s_password));
    memset(s_url, 0, sizeof(s_url));
    memset(s_auth_token, 0, sizeof(s_auth_token));
    memset(s_expected_hash, 0, sizeof(s_expected_hash));

    strncpy(s_ssid, config->ssid, sizeof(s_ssid) - 1);
    if (config->password) {
        strncpy(s_password, config->password, sizeof(s_password) - 1);
    }
    strncpy(s_url, config->url, sizeof(s_url) - 1);
    if (config->auth_token) {
        strncpy(s_auth_token, config->auth_token, sizeof(s_auth_token) - 1);
    }
    s_expected_size = config->expected_size;
    if (config->expected_hash) {
        strncpy(s_expected_hash, config->expected_hash, sizeof(s_expected_hash) - 1);
    }

    s_callback = callback;
    s_active = true;
    s_abort_requested = false;
    s_bytes_received = 0;
    s_retry_count = 0;
    s_status = WIFI_OTA_STATUS_IDLE;

    BaseType_t ret = xTaskCreate(wifi_ota_task, "wifi_ota", 8192, NULL, 5, &s_ota_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi OTA task");
        s_active = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WiFi OTA started");
    return ESP_OK;
}

void wifi_ota_abort(void)
{
    if (s_active) {
        ESP_LOGW(TAG, "WiFi OTA abort requested");
        s_abort_requested = true;
    }
}

bool wifi_ota_is_active(void)
{
    return s_active;
}

wifi_ota_status_t wifi_ota_get_status(void)
{
    return s_status;
}

// ============================================================================
// WiFi Scanning
// ============================================================================

static volatile bool s_scan_active = false;
static wifi_scan_callback_t s_scan_callback = NULL;
static TaskHandle_t s_scan_task = NULL;

#define MAX_SCAN_RESULTS 20

static void wifi_scan_task(void *arg)
{
    ESP_LOGI(TAG, "WiFi scan task started");

    esp_err_t err;
    wifi_scan_callback_t callback = s_scan_callback;

    s_scan_only_mode = true;

    err = init_wifi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init WiFi for scan");
        if (callback) {
            callback(NULL, 0, "Failed to initialize WiFi");
        }
        goto cleanup;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_scan_config_t scan_config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = 0,
        .scan_time.active.max = 0,
    };

    ESP_LOGI(TAG, "Starting WiFi scan...");
    err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        if (callback) {
            callback(NULL, 0, "WiFi scan failed");
        }
        goto cleanup;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    ESP_LOGI(TAG, "Found %d access points", ap_count);

    if (ap_count == 0) {
        if (callback) {
            callback(NULL, 0, NULL);
        }
        goto cleanup;
    }

    if (ap_count > MAX_SCAN_RESULTS) {
        ap_count = MAX_SCAN_RESULTS;
    }

    wifi_ap_record_t *ap_records = malloc(ap_count * sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        ESP_LOGE(TAG, "Failed to allocate memory for scan results");
        if (callback) {
            callback(NULL, 0, "Out of memory");
        }
        goto cleanup;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get scan results: %s", esp_err_to_name(err));
        free(ap_records);
        if (callback) {
            callback(NULL, 0, "Failed to get scan results");
        }
        goto cleanup;
    }

    wifi_scan_result_t *results = malloc(ap_count * sizeof(wifi_scan_result_t));
    if (results == NULL) {
        ESP_LOGE(TAG, "Failed to allocate results array");
        free(ap_records);
        if (callback) {
            callback(NULL, 0, "Out of memory");
        }
        goto cleanup;
    }

    size_t result_count = 0;
    for (int i = 0; i < ap_count; i++) {
        if (strlen((char *)ap_records[i].ssid) == 0) {
            continue;
        }

        bool duplicate = false;
        for (size_t j = 0; j < result_count; j++) {
            if (strcmp(results[j].ssid, (char *)ap_records[i].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        strncpy(results[result_count].ssid, (char *)ap_records[i].ssid, 32);
        results[result_count].ssid[32] = '\0';
        results[result_count].rssi = ap_records[i].rssi;
        results[result_count].authmode = (ap_records[i].authmode != WIFI_AUTH_OPEN) ? 1 : 0;
        result_count++;

        ESP_LOGI(TAG, "  %s (RSSI: %d, %s)",
                 results[result_count-1].ssid,
                 results[result_count-1].rssi,
                 results[result_count-1].authmode ? "secured" : "open");
    }

    free(ap_records);

    if (callback) {
        callback(results, result_count, NULL);
    }

    free(results);

cleanup:
    cleanup_wifi();
    s_scan_only_mode = false;
    s_scan_active = false;
    s_scan_task = NULL;
    ESP_LOGI(TAG, "WiFi scan task exiting");
    vTaskDelete(NULL);
}

esp_err_t wifi_scan_start(wifi_scan_callback_t callback)
{
    if (s_active) {
        ESP_LOGW(TAG, "Cannot scan while OTA is in progress");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_scan_active) {
        ESP_LOGW(TAG, "WiFi scan already in progress");
        return ESP_ERR_INVALID_STATE;
    }

    s_scan_callback = callback;
    s_scan_active = true;

    BaseType_t ret = xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, 5, &s_scan_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create WiFi scan task");
        s_scan_active = false;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "WiFi scan started");
    return ESP_OK;
}

bool wifi_scan_is_active(void)
{
    return s_scan_active;
}

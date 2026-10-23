/**
 * @file web_log.c
 * @brief Web-based log viewer via WiFi AP
 */

#include "web_log.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "nvs_flash.h"

static const char *TAG = "web_log";

// Log buffer configuration
#define LOG_BUFFER_SIZE 32768  // 32KB ring buffer
#define LOG_ENTRY_MAX_LEN 256

// State
static bool s_running = false;
static httpd_handle_t s_server = NULL;
static char s_ssid[32] = {0};

// Ring buffer for logs
static char s_log_buffer[LOG_BUFFER_SIZE];
static size_t s_log_write_pos = 0;
static size_t s_log_size = 0;
static SemaphoreHandle_t s_log_mutex = NULL;

// Original log handler
static vprintf_like_t s_original_log_handler = NULL;

// HTML template
static const char *HTML_HEADER =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PV ECU Logs</title>"
    "<style>"
    "body{font-family:monospace;background:#1e1e1e;color:#d4d4d4;margin:0;padding:10px;}"
    "h1{color:#569cd6;margin:0 0 10px 0;font-size:18px;}"
    ".info{color:#888;margin-bottom:10px;font-size:12px;}"
    "pre{background:#0d0d0d;padding:10px;overflow:auto;white-space:pre-wrap;word-wrap:break-word;"
    "font-size:11px;line-height:1.4;max-height:calc(100vh - 120px);}"
    ".E{color:#f44747;}.W{color:#ce9178;}.I{color:#4ec9b0;}.D{color:#9cdcfe;}.V{color:#6a9955;}"
    "button{background:#0e639c;color:#fff;border:none;padding:8px 16px;cursor:pointer;margin-right:10px;}"
    "button:hover{background:#1177bb;}"
    "</style>"
    "<script>"
    "function refresh(){location.reload();}"
    "function clear(){fetch('/clear').then(()=>refresh());}"
    "setTimeout(refresh,5000);"  // Auto-refresh every 5 seconds
    "</script>"
    "</head><body>"
    "<h1>Perfect Vision ECU - Debug Logs</h1>"
    "<div class='info'>Auto-refresh: 5s | ";

static const char *HTML_FOOTER =
    "</div>"
    "<button onclick='refresh()'>Refresh Now</button>"
    "<button onclick='clear()'>Clear Logs</button>"
    "<pre id='logs'>";

static const char *HTML_END = "</pre></body></html>";

// Custom log handler that captures logs
static int log_handler(const char *format, va_list args)
{
    // Call original handler first
    int ret = 0;
    if (s_original_log_handler) {
        va_list args_copy;
        va_copy(args_copy, args);
        ret = s_original_log_handler(format, args_copy);
        va_end(args_copy);
    }

    // Format and store in buffer
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        char entry[LOG_ENTRY_MAX_LEN];
        int len = vsnprintf(entry, sizeof(entry), format, args);
        if (len > 0) {
            if (len >= sizeof(entry)) {
                len = sizeof(entry) - 1;
            }

            // Write to ring buffer
            for (int i = 0; i < len; i++) {
                s_log_buffer[s_log_write_pos] = entry[i];
                s_log_write_pos = (s_log_write_pos + 1) % LOG_BUFFER_SIZE;
            }

            if (s_log_size + len > LOG_BUFFER_SIZE) {
                s_log_size = LOG_BUFFER_SIZE;
            } else {
                s_log_size += len;
            }
        }
        xSemaphoreGive(s_log_mutex);
    }

    return ret;
}

// HTTP handler for main page
static esp_err_t http_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");

    // Send header
    httpd_resp_send_chunk(req, HTML_HEADER, strlen(HTML_HEADER));

    // Send buffer info
    char info[128];
    snprintf(info, sizeof(info), "Buffer: %zu/%d bytes", s_log_size, LOG_BUFFER_SIZE);
    httpd_resp_send_chunk(req, info, strlen(info));
    httpd_resp_send_chunk(req, HTML_FOOTER, strlen(HTML_FOOTER));

    // Send logs with HTML escaping and color coding
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_log_size > 0) {
            size_t start;
            if (s_log_size >= LOG_BUFFER_SIZE) {
                start = s_log_write_pos;
            } else {
                start = 0;
            }

            char chunk[512];
            size_t chunk_pos = 0;
            bool at_line_start = true;
            char current_level = 0;

            for (size_t i = 0; i < s_log_size; i++) {
                size_t pos = (start + i) % LOG_BUFFER_SIZE;
                char c = s_log_buffer[pos];

                // Check for log level at line start (e.g., "I (1234) tag: ")
                if (at_line_start && i + 1 < s_log_size) {
                    size_t next_pos = (pos + 1) % LOG_BUFFER_SIZE;
                    if (s_log_buffer[next_pos] == ' ' && (c == 'E' || c == 'W' || c == 'I' || c == 'D' || c == 'V')) {
                        // Add color span
                        if (current_level) {
                            // Close previous span
                            const char *close = "</span>";
                            if (chunk_pos + strlen(close) >= sizeof(chunk) - 1) {
                                httpd_resp_send_chunk(req, chunk, chunk_pos);
                                chunk_pos = 0;
                            }
                            memcpy(chunk + chunk_pos, close, strlen(close));
                            chunk_pos += strlen(close);
                        }

                        // Open new span
                        char span[32];
                        snprintf(span, sizeof(span), "<span class='%c'>", c);
                        if (chunk_pos + strlen(span) >= sizeof(chunk) - 1) {
                            httpd_resp_send_chunk(req, chunk, chunk_pos);
                            chunk_pos = 0;
                        }
                        memcpy(chunk + chunk_pos, span, strlen(span));
                        chunk_pos += strlen(span);
                        current_level = c;
                    }
                }

                // Escape HTML special characters
                const char *escaped = NULL;
                size_t escaped_len = 0;
                switch (c) {
                    case '<': escaped = "&lt;"; escaped_len = 4; break;
                    case '>': escaped = "&gt;"; escaped_len = 4; break;
                    case '&': escaped = "&amp;"; escaped_len = 5; break;
                    default: break;
                }

                if (escaped) {
                    if (chunk_pos + escaped_len >= sizeof(chunk) - 1) {
                        httpd_resp_send_chunk(req, chunk, chunk_pos);
                        chunk_pos = 0;
                    }
                    memcpy(chunk + chunk_pos, escaped, escaped_len);
                    chunk_pos += escaped_len;
                } else {
                    if (chunk_pos >= sizeof(chunk) - 1) {
                        httpd_resp_send_chunk(req, chunk, chunk_pos);
                        chunk_pos = 0;
                    }
                    chunk[chunk_pos++] = c;
                }

                at_line_start = (c == '\n');
            }

            // Close any open span
            if (current_level) {
                const char *close = "</span>";
                if (chunk_pos + strlen(close) < sizeof(chunk)) {
                    memcpy(chunk + chunk_pos, close, strlen(close));
                    chunk_pos += strlen(close);
                }
            }

            if (chunk_pos > 0) {
                httpd_resp_send_chunk(req, chunk, chunk_pos);
            }
        }
        xSemaphoreGive(s_log_mutex);
    }

    // Send footer
    httpd_resp_send_chunk(req, HTML_END, strlen(HTML_END));
    httpd_resp_send_chunk(req, NULL, 0);

    return ESP_OK;
}

// HTTP handler for clear
static esp_err_t http_clear_handler(httpd_req_t *req)
{
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_log_write_pos = 0;
        s_log_size = 0;
        memset(s_log_buffer, 0, LOG_BUFFER_SIZE);
        xSemaphoreGive(s_log_mutex);
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// HTTP handler for raw logs (JSON)
static esp_err_t http_raw_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");

    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_log_size > 0) {
            size_t start;
            if (s_log_size >= LOG_BUFFER_SIZE) {
                start = s_log_write_pos;
            } else {
                start = 0;
            }

            char chunk[512];
            size_t chunk_pos = 0;

            for (size_t i = 0; i < s_log_size; i++) {
                size_t pos = (start + i) % LOG_BUFFER_SIZE;
                chunk[chunk_pos++] = s_log_buffer[pos];

                if (chunk_pos >= sizeof(chunk) - 1) {
                    httpd_resp_send_chunk(req, chunk, chunk_pos);
                    chunk_pos = 0;
                }
            }

            if (chunk_pos > 0) {
                httpd_resp_send_chunk(req, chunk, chunk_pos);
            }
        }
        xSemaphoreGive(s_log_mutex);
    }

    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static httpd_uri_t uri_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = http_get_handler,
    .user_ctx = NULL
};

static httpd_uri_t uri_clear = {
    .uri = "/clear",
    .method = HTTP_GET,
    .handler = http_clear_handler,
    .user_ctx = NULL
};

static httpd_uri_t uri_raw = {
    .uri = "/raw",
    .method = HTTP_GET,
    .handler = http_raw_handler,
    .user_ctx = NULL
};

esp_err_t web_log_start(void)
{
    if (s_running) {
        return ESP_OK;
    }

    esp_err_t ret;

    // Create mutex if needed
    if (s_log_mutex == NULL) {
        s_log_mutex = xSemaphoreCreateMutex();
        if (s_log_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    // Initialize WiFi in AP mode
    ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to init netif: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    // Build SSID with MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "PV_LOG_%02X%02X%02X", mac[3], mac[4], mac[5]);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = strlen(s_ssid),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .password = "pvdebug123",
        },
    };
    memcpy(wifi_config.ap.ssid, s_ssid, strlen(s_ssid));

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi mode: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set WiFi config: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start WiFi: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "WiFi AP started: SSID=%s, Password=pvdebug123", s_ssid);

    // Start HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        esp_wifi_stop();
        return ret;
    }

    httpd_register_uri_handler(s_server, &uri_get);
    httpd_register_uri_handler(s_server, &uri_clear);
    httpd_register_uri_handler(s_server, &uri_raw);

    // Install custom log handler
    s_original_log_handler = esp_log_set_vprintf(log_handler);

    s_running = true;
    ESP_LOGI(TAG, "Web log server started at http://192.168.4.1/");

    return ESP_OK;
}

esp_err_t web_log_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }

    // Restore original log handler
    if (s_original_log_handler) {
        esp_log_set_vprintf(s_original_log_handler);
        s_original_log_handler = NULL;
    }

    // Stop HTTP server
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    // Stop WiFi
    esp_wifi_stop();
    esp_wifi_deinit();

    s_running = false;
    ESP_LOGI(TAG, "Web log server stopped");

    return ESP_OK;
}

bool web_log_is_running(void)
{
    return s_running;
}

void web_log_add(char level, const char *tag, const char *message)
{
    if (s_log_mutex && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        char entry[LOG_ENTRY_MAX_LEN];
        int len = snprintf(entry, sizeof(entry), "%c (%lu) %s: %s\n",
                           level, (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                           tag, message);

        if (len > 0 && len < sizeof(entry)) {
            for (int i = 0; i < len; i++) {
                s_log_buffer[s_log_write_pos] = entry[i];
                s_log_write_pos = (s_log_write_pos + 1) % LOG_BUFFER_SIZE;
            }

            if (s_log_size + len > LOG_BUFFER_SIZE) {
                s_log_size = LOG_BUFFER_SIZE;
            } else {
                s_log_size += len;
            }
        }
        xSemaphoreGive(s_log_mutex);
    }
}

const char* web_log_get_ssid(void)
{
    return s_ssid;
}

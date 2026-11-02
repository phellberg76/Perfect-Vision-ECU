/**
 * @file wifi_transfer.c
 * @brief WiFi-based high-speed data transfer implementation
 */

#include "wifi_transfer.h"

#include <string.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "wifi_transfer";

// Event group bits
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

// TCP receive buffer size
#define TCP_RX_BUFFER_SIZE  4096

// Connection timeout (ms) - increased for slow Android hotspot startup
#define WIFI_CONNECT_TIMEOUT_MS   20000
#define TCP_CONNECT_TIMEOUT_MS    5000

// State
static wifi_transfer_state_t s_state = WIFI_TRANSFER_DISCONNECTED;
static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t *s_sta_netif = NULL;
static int s_tcp_socket = -1;
static TaskHandle_t s_rx_task = NULL;
static bool s_initialized = false;

// Connection info
static char s_host_ip[64] = {0};
static uint16_t s_host_port = 0;

// Callbacks
static wifi_transfer_rx_callback_t s_rx_callback = NULL;
static wifi_transfer_state_callback_t s_state_callback = NULL;
static wifi_transfer_debug_callback_t s_debug_callback = NULL;

// Debug logging helper
static void debug_log(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    ESP_LOGI(TAG, "%s", buf);
    if (s_debug_callback) {
        s_debug_callback(buf);
    }
}

// Statistics
static uint32_t s_tx_bytes = 0;
static uint32_t s_rx_bytes = 0;
static uint32_t s_tx_frames = 0;

// WiFi retry count
static int s_retry_count = 0;
#define MAX_WIFI_RETRY  5

// Scan results for targeting specific AP
#define MAX_SCAN_RESULTS 10
static wifi_ap_record_t s_scan_results[MAX_SCAN_RESULTS];
static uint16_t s_scan_count = 0;
static int s_current_ap_index = 0;

// -----------------------------------------------------------------------------
// Internal functions
// -----------------------------------------------------------------------------

static void set_state(wifi_transfer_state_t new_state)
{
    if (s_state != new_state) {
        ESP_LOGI(TAG, "State: %d -> %d", s_state, new_state);
        s_state = new_state;
        if (s_state_callback) {
            // For READY state, add delay and retry to ensure BLE message gets through
            // WiFi/BLE coexistence can cause BLE notifications to fail during active WiFi
            if (new_state == WIFI_TRANSFER_READY || new_state == WIFI_TRANSFER_CONNECTED) {
                // Let WiFi operations settle before BLE notification
                vTaskDelay(pdMS_TO_TICKS(100));
                for (int retry = 0; retry < 3; retry++) {
                    s_state_callback(new_state);
                    vTaskDelay(pdMS_TO_TICKS(50));  // Give BLE time to send
                }
            } else {
                s_state_callback(new_state);
            }
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "WiFi STA started");
                break;

            case WIFI_EVENT_STA_CONNECTED: {
                wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
                ESP_LOGI(TAG, "WiFi connected to SSID='%s' BSSID=%02x:%02x:%02x:%02x:%02x:%02x ch=%d",
                         event->ssid,
                         event->bssid[0], event->bssid[1], event->bssid[2],
                         event->bssid[3], event->bssid[4], event->bssid[5],
                         event->channel);
                s_retry_count = 0;
                break;
            }

            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "WiFi disconnected (reason=%d)", disconn->reason);
                if (s_state == WIFI_TRANSFER_CONNECTING && s_retry_count < MAX_WIFI_RETRY) {
                    s_retry_count++;
                    ESP_LOGI(TAG, "Retry WiFi connection (%d/%d) in 500ms", s_retry_count, MAX_WIFI_RETRY);
                    // Delay before retry to let WiFi stack settle and avoid "sta is connecting" error
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_err_t err = esp_wifi_connect();
                    if (err != ESP_OK) {
                        ESP_LOGW(TAG, "esp_wifi_connect retry failed: %s", esp_err_to_name(err));
                    }
                } else {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    if (s_state == WIFI_TRANSFER_READY || s_state == WIFI_TRANSFER_CONNECTED) {
                        // Lost connection while active
                        set_state(WIFI_TRANSFER_DISCONNECTED);
                    }
                }
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void tcp_rx_task(void *arg)
{
    uint8_t *rx_buffer = malloc(TCP_RX_BUFFER_SIZE);
    if (!rx_buffer) {
        ESP_LOGE(TAG, "Failed to allocate RX buffer");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "TCP RX task started");

    while (s_tcp_socket >= 0) {
        int len = recv(s_tcp_socket, rx_buffer, TCP_RX_BUFFER_SIZE, 0);

        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout, check if we should continue
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            ESP_LOGE(TAG, "TCP recv error: %d", errno);
            break;
        } else if (len == 0) {
            ESP_LOGI(TAG, "TCP connection closed by peer");
            break;
        } else {
            s_rx_bytes += len;
            ESP_LOGD(TAG, "TCP RX: %d bytes", len);

            if (s_rx_callback) {
                s_rx_callback(rx_buffer, len);
            }
        }
    }

    free(rx_buffer);
    ESP_LOGI(TAG, "TCP RX task exiting");

    // Close socket if still open
    if (s_tcp_socket >= 0) {
        close(s_tcp_socket);
        s_tcp_socket = -1;
    }

    set_state(WIFI_TRANSFER_CONNECTED);  // Still WiFi connected, but no TCP
    s_rx_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t tcp_connect(void)
{
    struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(s_host_ip);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(s_host_port);

    s_tcp_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_tcp_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        return ESP_FAIL;
    }

    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = TCP_CONNECT_TIMEOUT_MS / 1000;
    timeout.tv_usec = (TCP_CONNECT_TIMEOUT_MS % 1000) * 1000;
    setsockopt(s_tcp_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(s_tcp_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // Disable Nagle's algorithm for lower latency
    int flag = 1;
    setsockopt(s_tcp_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    debug_log("TCP: Connecting to %s:%d...", s_host_ip, s_host_port);

    int err = connect(s_tcp_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        debug_log("TCP: connect() failed, errno=%d", errno);
        close(s_tcp_socket);
        s_tcp_socket = -1;
        return ESP_FAIL;
    }

    debug_log("TCP: Connected successfully!");

    // Send "ready" confirmation over TCP immediately
    // This bypasses BLE/WiFi coexistence issues that can block BLE notifications
    const char *ready_msg = "{\"type\":\"wifi_transfer_state\",\"state\":\"ready\"}\n";
    int sent = send(s_tcp_socket, ready_msg, strlen(ready_msg), 0);
    if (sent > 0) {
        ESP_LOGI(TAG, "TCP: Sent ready confirmation (%d bytes)", sent);
    } else {
        ESP_LOGW(TAG, "TCP: Failed to send ready confirmation, errno=%d", errno);
    }

    // Brief delay to ensure JSON is transmitted before binary CAN frames start streaming
    // This prevents TCP from coalescing JSON and binary data into one packet,
    // which can confuse the Flutter-side parser
    vTaskDelay(pdMS_TO_TICKS(50));

    // Start RX task
    xTaskCreate(tcp_rx_task, "tcp_rx", 4096, NULL, 5, &s_rx_task);

    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

esp_err_t wifi_transfer_init(wifi_transfer_rx_callback_t rx_callback)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    s_rx_callback = rx_callback;
    s_initialized = true;

    ESP_LOGI(TAG, "WiFi transfer initialized (WiFi will be started on connect)");
    return ESP_OK;
}

void wifi_transfer_set_state_callback(wifi_transfer_state_callback_t callback)
{
    s_state_callback = callback;
}

void wifi_transfer_set_debug_callback(wifi_transfer_debug_callback_t callback)
{
    s_debug_callback = callback;
}

// Flag to track if we initialized WiFi
static bool s_wifi_started = false;

static esp_err_t init_wifi_stack(void)
{
    if (s_wifi_event_group != NULL) {
        // Already initialized
        return ESP_OK;
    }

    debug_log("WiFi: Initializing stack...");

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop (may already exist)
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to create event loop: %s", esp_err_to_name(err));
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        return err;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create WiFi STA netif");
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init WiFi: %s", esp_err_to_name(err));
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    debug_log("WiFi: Stack initialized");
    return ESP_OK;
}

static void cleanup_wifi_stack(void)
{
    debug_log("WiFi: Cleaning up stack...");

    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }

    esp_wifi_deinit();

    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    debug_log("WiFi: Stack cleaned up");
}

// Helper to parse MAC address from string "XX:XX:XX:XX:XX:XX"
static bool parse_mac_address(const char *str, uint8_t *mac)
{
    if (!str || strlen(str) != 17) return false;
    unsigned int m[6];
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) != 6) {
        return false;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = (uint8_t)m[i];
    }
    return true;
}

// Scan for WiFi networks and find all APs with matching SSID
// Returns number of matching APs found, sorted by RSSI (strongest first)
static int scan_for_ssid(const char *target_ssid)
{
    debug_log("WiFi: Scanning for SSID '%s'...", target_ssid);

    // Do a full scan (not SSID-filtered) to find all networks
    // Some ESP-IDF versions have issues with SSID-targeted scans
    // Use default scan times when BLE is active (ESP-IDF requirement)
    wifi_scan_config_t scan_config = {
        .ssid = NULL,  // Scan all SSIDs
        .bssid = NULL,
        .channel = 0,  // All channels
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        // Use defaults - ESP-IDF requires this when BLE is active
        .scan_time.active.min = 0,
        .scan_time.active.max = 0,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);  // Blocking scan
    if (err != ESP_OK) {
        debug_log("WiFi: Scan failed: %s", esp_err_to_name(err));
        return 0;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    debug_log("WiFi: Found %d total APs", ap_count);

    if (ap_count == 0) {
        return 0;
    }

    // Get all scan results
    wifi_ap_record_t *ap_records = malloc(sizeof(wifi_ap_record_t) * ap_count);
    if (!ap_records) {
        debug_log("WiFi: Failed to allocate scan buffer");
        esp_wifi_scan_get_ap_records(&ap_count, NULL);  // Clear scan results
        return 0;
    }

    esp_wifi_scan_get_ap_records(&ap_count, ap_records);

    // Filter for matching SSID and copy to our buffer, sorted by RSSI
    s_scan_count = 0;
    for (int i = 0; i < ap_count && s_scan_count < MAX_SCAN_RESULTS; i++) {
        if (strcmp((char *)ap_records[i].ssid, target_ssid) == 0) {
            // Insert sorted by RSSI (strongest first)
            int insert_pos = s_scan_count;
            for (int j = 0; j < s_scan_count; j++) {
                if (ap_records[i].rssi > s_scan_results[j].rssi) {
                    insert_pos = j;
                    break;
                }
            }
            // Shift down
            for (int j = s_scan_count; j > insert_pos; j--) {
                s_scan_results[j] = s_scan_results[j-1];
            }
            s_scan_results[insert_pos] = ap_records[i];
            s_scan_count++;
        }
    }

    free(ap_records);

    // Log matching APs
    debug_log("WiFi: Found %d APs with SSID '%s':", s_scan_count, target_ssid);
    for (int i = 0; i < s_scan_count; i++) {
        debug_log("  [%d] BSSID=%02x:%02x:%02x:%02x:%02x:%02x RSSI=%d ch=%d",
                  i,
                  s_scan_results[i].bssid[0], s_scan_results[i].bssid[1],
                  s_scan_results[i].bssid[2], s_scan_results[i].bssid[3],
                  s_scan_results[i].bssid[4], s_scan_results[i].bssid[5],
                  s_scan_results[i].rssi, s_scan_results[i].primary);
    }

    return s_scan_count;
}

esp_err_t wifi_transfer_connect(const char *ssid, const char *password,
                                 const char *host_ip, uint16_t port,
                                 const char *bssid)
{
    ESP_LOGI(TAG, "wifi_transfer_connect called: initialized=%d, state=%d",
             s_initialized, s_state);

    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized - wifi_transfer_init() was not called");
        return ESP_ERR_INVALID_STATE;
    }

    // If already connected or connecting, disconnect first to allow reconnection
    // This handles Flutter app restarts gracefully
    if (s_state != WIFI_TRANSFER_DISCONNECTED) {
        ESP_LOGI(TAG, "Auto-disconnecting from previous session (state=%d)", s_state);
        wifi_transfer_disconnect();
        // Give a moment for cleanup
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Store connection info
    strncpy(s_host_ip, host_ip, sizeof(s_host_ip) - 1);
    s_host_port = port;

    set_state(WIFI_TRANSFER_CONNECTING);

    // Initialize WiFi stack on demand
    esp_err_t ret = init_wifi_stack();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init WiFi stack: %s", esp_err_to_name(ret));
        set_state(WIFI_TRANSFER_ERROR);
        return ret;
    }

    // Erase stored WiFi credentials from NVS to prevent auto-connecting to old networks
    // This must be done BEFORE setting mode, as restore() resets everything
    esp_err_t nvs_err = esp_wifi_restore();
    if (nvs_err == ESP_OK) {
        debug_log("WiFi: Erased stored credentials from NVS");
    } else {
        debug_log("WiFi: esp_wifi_restore returned %d", nvs_err);
    }

    // Set STA mode AFTER restore (restore resets the mode)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    debug_log("WiFi: Set mode to STA");

    // Disable WiFi power save for more reliable connection with BLE active
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    // Also set empty config to clear any remaining cached settings
    wifi_config_t empty_config = {0};
    esp_wifi_set_config(WIFI_IF_STA, &empty_config);
    debug_log("WiFi: Cleared cached config");

    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        cleanup_wifi_stack();
        set_state(WIFI_TRANSFER_ERROR);
        return ret;
    }
    s_wifi_started = true;

    // Wait for hotspot to be fully visible before scanning
    // Android LocalOnlyHotspot may take several seconds to start advertising
    debug_log("WiFi: Waiting 4s for hotspot to become visible...");
    vTaskDelay(pdMS_TO_TICKS(4000));

    // Scan for networks with this SSID to find all matching APs
    // This helps when multiple networks have similar names (e.g., AndroidShare_xxxx)
    int ap_count = scan_for_ssid(ssid);

    // If BSSID provided and valid, try that first (skip scan results)
    uint8_t target_bssid[6];
    bool have_target_bssid = bssid && parse_mac_address(bssid, target_bssid);
    if (have_target_bssid) {
        debug_log("WiFi: Have target BSSID from app: %s", bssid);
    }

    // Try each AP in order (by RSSI, strongest first)
    // The local hotspot should be the strongest signal
    // If we exhaust all APs, rescan and try again (hotspot may have started late)
    int scan_retry = 0;
    const int max_scan_retries = 3;

rescan_and_try:
    s_current_ap_index = 0;
    int max_attempts = (ap_count > 0) ? ap_count : 1;
    debug_log("WiFi: Will try %d APs (scan attempt %d/%d)", max_attempts, scan_retry + 1, max_scan_retries);

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        // Configure WiFi
        wifi_config_t wifi_config = {0};
        strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        wifi_config.sta.threshold.authmode = strlen(password) > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        wifi_config.sta.pmf_cfg.capable = true;
        wifi_config.sta.pmf_cfg.required = false;

        // Set BSSID to target specific AP
        if (have_target_bssid) {
            // Use app-provided BSSID
            memcpy(wifi_config.sta.bssid, target_bssid, 6);
            wifi_config.sta.bssid_set = true;
            debug_log("WiFi: Targeting app BSSID %s", bssid);
            // Only try app-provided BSSID once
            have_target_bssid = false;
        } else if (ap_count > 0 && s_current_ap_index < ap_count) {
            // Use BSSID from scan results
            memcpy(wifi_config.sta.bssid, s_scan_results[s_current_ap_index].bssid, 6);
            wifi_config.sta.bssid_set = true;
            debug_log("WiFi: Trying AP #%d BSSID=%02x:%02x:%02x:%02x:%02x:%02x RSSI=%d",
                      s_current_ap_index,
                      wifi_config.sta.bssid[0], wifi_config.sta.bssid[1],
                      wifi_config.sta.bssid[2], wifi_config.sta.bssid[3],
                      wifi_config.sta.bssid[4], wifi_config.sta.bssid[5],
                      s_scan_results[s_current_ap_index].rssi);
            s_current_ap_index++;
        } else {
            wifi_config.sta.bssid_set = false;
            debug_log("WiFi: Connecting without BSSID targeting");
        }

        ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
            continue;
        }

        // Clear event bits
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        s_retry_count = 0;

        // Start connection
        debug_log("WiFi: Connecting to SSID=%s (attempt %d/%d)", ssid, attempt + 1, max_attempts);
        ret = esp_wifi_connect();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // Wait for connection
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                                pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

        if (bits & WIFI_CONNECTED_BIT) {
            // Get our IP address and gateway from DHCP
            esp_netif_ip_info_t ip_info;
            esp_netif_get_ip_info(s_sta_netif, &ip_info);
            debug_log("WiFi: Got IP=" IPSTR ", gateway=" IPSTR,
                      IP2STR(&ip_info.ip), IP2STR(&ip_info.gw));

            // Validate we connected to the correct network by checking gateway matches
            // Convert gateway IP to string for comparison with app-provided IP
            char gw_str[16];
            snprintf(gw_str, sizeof(gw_str), IPSTR, IP2STR(&ip_info.gw));

            // Check if gateway matches the app-provided host IP
            if (strcmp(gw_str, s_host_ip) != 0) {
                debug_log("WiFi: Wrong network! Gateway=%s, expected=%s - trying next AP",
                          gw_str, s_host_ip);
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;  // Try next AP
            }

            debug_log("WiFi: Correct hotspot network (gateway matches %s)", s_host_ip);

            set_state(WIFI_TRANSFER_CONNECTED);

            // Connect TCP
            ret = tcp_connect();
            if (ret == ESP_OK) {
                set_state(WIFI_TRANSFER_READY);
                s_tx_bytes = 0;
                s_rx_bytes = 0;
                s_tx_frames = 0;
                return ESP_OK;
            } else {
                debug_log("TCP: Failed, trying next AP");
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;  // Try next AP
            }
        } else {
            debug_log("WiFi: Connection failed/timeout (bits=0x%lx), trying next AP",
                      (unsigned long)bits);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    // All APs in current scan failed - try rescanning
    // Wait longer each time to give the hotspot more time to become visible
    scan_retry++;
    if (scan_retry < max_scan_retries) {
        int wait_ms = 3000 + (scan_retry * 2000);  // 3s, 5s, 7s
        debug_log("WiFi: All APs failed, waiting %dms then rescanning (attempt %d/%d)...",
                  wait_ms, scan_retry + 1, max_scan_retries);
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        ap_count = scan_for_ssid(ssid);
        goto rescan_and_try;  // Try even if ap_count is 0 (will connect without BSSID)
    }

    // All attempts failed
    debug_log("WiFi: All connection attempts failed");
    cleanup_wifi_stack();
    set_state(WIFI_TRANSFER_ERROR);
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_transfer_disconnect(void)
{
    ESP_LOGI(TAG, "Disconnecting...");

    // Close TCP socket
    if (s_tcp_socket >= 0) {
        shutdown(s_tcp_socket, SHUT_RDWR);
        close(s_tcp_socket);
        s_tcp_socket = -1;
    }

    // Wait for RX task to exit
    if (s_rx_task) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Disconnect WiFi and clean up stack
    if (s_wifi_started) {
        esp_wifi_disconnect();
    }
    cleanup_wifi_stack();

    set_state(WIFI_TRANSFER_DISCONNECTED);

    ESP_LOGI(TAG, "Disconnected. Stats: TX %lu bytes (%lu frames), RX %lu bytes",
             (unsigned long)s_tx_bytes, (unsigned long)s_tx_frames, (unsigned long)s_rx_bytes);

    return ESP_OK;
}

esp_err_t wifi_transfer_send(const uint8_t *data, size_t len)
{
    if (s_state != WIFI_TRANSFER_READY || s_tcp_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int sent = send(s_tcp_socket, data, len, 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "TCP send error: %d", errno);
        return ESP_FAIL;
    }

    s_tx_bytes += sent;
    return ESP_OK;
}

esp_err_t wifi_transfer_send_can_frame(uint32_t can_id, const uint8_t *data, uint8_t dlc)
{
    if (s_state != WIFI_TRANSFER_READY || s_tcp_socket < 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // Binary format: [4 bytes ID][1 byte DLC][8 bytes data] = 13 bytes
    uint8_t frame[13];
    frame[0] = (can_id >> 24) & 0xFF;
    frame[1] = (can_id >> 16) & 0xFF;
    frame[2] = (can_id >> 8) & 0xFF;
    frame[3] = can_id & 0xFF;
    frame[4] = dlc;
    memcpy(&frame[5], data, 8);

    int sent = send(s_tcp_socket, frame, sizeof(frame), 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "TCP send CAN frame error: %d", errno);
        return ESP_FAIL;
    }

    s_tx_bytes += sent;
    s_tx_frames++;
    return ESP_OK;
}

bool wifi_transfer_is_ready(void)
{
    return s_state == WIFI_TRANSFER_READY && s_tcp_socket >= 0;
}

wifi_transfer_state_t wifi_transfer_get_state(void)
{
    return s_state;
}

void wifi_transfer_get_stats(uint32_t *tx_bytes, uint32_t *rx_bytes, uint32_t *tx_frames)
{
    if (tx_bytes) *tx_bytes = s_tx_bytes;
    if (rx_bytes) *rx_bytes = s_rx_bytes;
    if (tx_frames) *tx_frames = s_tx_frames;
}

void wifi_transfer_deinit(void)
{
    if (!s_initialized) {
        return;
    }

    wifi_transfer_disconnect();

    s_initialized = false;
    ESP_LOGI(TAG, "WiFi transfer deinitialized");
}

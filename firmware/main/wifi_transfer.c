/**
 * @file wifi_transfer.c
 * @brief WiFi-based high-speed data transfer implementation
 */

#include "wifi_transfer.h"

#include <string.h>
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

// Connection timeout (ms)
#define WIFI_CONNECT_TIMEOUT_MS   15000
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

// Statistics
static uint32_t s_tx_bytes = 0;
static uint32_t s_rx_bytes = 0;
static uint32_t s_tx_frames = 0;

// WiFi retry count
static int s_retry_count = 0;
#define MAX_WIFI_RETRY  3

// -----------------------------------------------------------------------------
// Internal functions
// -----------------------------------------------------------------------------

static void set_state(wifi_transfer_state_t new_state)
{
    if (s_state != new_state) {
        ESP_LOGI(TAG, "State: %d -> %d", s_state, new_state);
        s_state = new_state;
        if (s_state_callback) {
            s_state_callback(new_state);
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

            case WIFI_EVENT_STA_CONNECTED:
                ESP_LOGI(TAG, "WiFi connected to hotspot");
                s_retry_count = 0;
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                ESP_LOGW(TAG, "WiFi disconnected");
                if (s_state == WIFI_TRANSFER_CONNECTING && s_retry_count < MAX_WIFI_RETRY) {
                    s_retry_count++;
                    ESP_LOGI(TAG, "Retry WiFi connection (%d/%d)", s_retry_count, MAX_WIFI_RETRY);
                    esp_wifi_connect();
                } else {
                    xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
                    if (s_state == WIFI_TRANSFER_READY || s_state == WIFI_TRANSFER_CONNECTED) {
                        // Lost connection while active
                        set_state(WIFI_TRANSFER_DISCONNECTED);
                    }
                }
                break;

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

    ESP_LOGI(TAG, "Connecting to %s:%d...", s_host_ip, s_host_port);

    int err = connect(s_tcp_socket, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (err != 0) {
        ESP_LOGE(TAG, "TCP connect failed: %d", errno);
        close(s_tcp_socket);
        s_tcp_socket = -1;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "TCP connected!");

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

    // Create event group
    s_wifi_event_group = xEventGroupCreate();
    if (!s_wifi_event_group) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_ERR_NO_MEM;
    }

    // Initialize TCP/IP stack (might already be done)
    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create default event loop (might already exist)
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Create station interface
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) {
        ESP_LOGE(TAG, "Failed to create STA netif");
        return ESP_FAIL;
    }

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register event handlers
    esp_event_handler_instance_t instance_wifi;
    esp_event_handler_instance_t instance_ip;

    ret = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL, &instance_wifi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register WiFi event handler");
        return ret;
    }

    ret = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL, &instance_ip);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register IP event handler");
        return ret;
    }

    // Set WiFi mode to station
    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start WiFi (but don't connect yet)
    ret = esp_wifi_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "WiFi transfer initialized");
    return ESP_OK;
}

void wifi_transfer_set_state_callback(wifi_transfer_state_callback_t callback)
{
    s_state_callback = callback;
}

esp_err_t wifi_transfer_connect(const char *ssid, const char *password,
                                 const char *host_ip, uint16_t port)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_state != WIFI_TRANSFER_DISCONNECTED) {
        ESP_LOGW(TAG, "Already connected or connecting");
        return ESP_ERR_INVALID_STATE;
    }

    // Store connection info
    strncpy(s_host_ip, host_ip, sizeof(s_host_ip) - 1);
    s_host_port = port;

    // Configure WiFi
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    esp_err_t ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Clear event bits
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;

    set_state(WIFI_TRANSFER_CONNECTING);

    // Start connection
    ESP_LOGI(TAG, "Connecting to SSID: %s", ssid);
    ret = esp_wifi_connect();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(ret));
        set_state(WIFI_TRANSFER_ERROR);
        return ret;
    }

    // Wait for connection
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected, establishing TCP connection...");
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
            set_state(WIFI_TRANSFER_ERROR);
            esp_wifi_disconnect();
            return ret;
        }
    } else {
        ESP_LOGE(TAG, "WiFi connection failed or timed out");
        set_state(WIFI_TRANSFER_ERROR);
        return ESP_ERR_TIMEOUT;
    }
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

    // Disconnect WiFi
    esp_wifi_disconnect();

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

    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_sta_netif) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }

    if (s_wifi_event_group) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "WiFi transfer deinitialized");
}

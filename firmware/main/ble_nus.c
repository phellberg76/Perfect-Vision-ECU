/**
 * @file ble_nus.c
 * @brief BLE Nordic UART Service implementation using ESP-IDF NimBLE
 */

#include "ble_nus.h"
#include "status_led.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE_NUS";

// Nordic UART Service UUIDs (128-bit)
static const ble_uuid128_t nus_service_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);

static const ble_uuid128_t nus_rx_char_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);

static const ble_uuid128_t nus_tx_char_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

// CAN stream characteristic (binary CAN frames, Notify)
static const ble_uuid128_t nus_can_char_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x04, 0x00, 0x40, 0x6e);

// GPS data characteristic (JSON GPS updates, Notify)
static const ble_uuid128_t nus_gps_char_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x05, 0x00, 0x40, 0x6e);

// Connection state
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_current_mtu = 23;  // Default BLE MTU, updated on MTU exchange
static uint16_t s_tx_attr_handle;
static uint16_t s_can_attr_handle;
static uint16_t s_gps_attr_handle;
static bool s_connected = false;

// Callbacks
static ble_rx_callback_t s_rx_callback = NULL;
static ble_connection_callback_t s_connection_callback = NULL;
static ble_binary_rx_callback_t s_binary_rx_callback = NULL;

// Binary mode flag
static bool s_binary_mode = false;

// RX buffer for assembling multi-packet messages
#define RX_BUFFER_SIZE 2048
static char s_rx_buffer[RX_BUFFER_SIZE];
static size_t s_rx_buffer_len = 0;

// Static buffer for binary data
#define BINARY_RX_BUFFER_SIZE 512
static uint8_t s_binary_rx_buffer[BINARY_RX_BUFFER_SIZE];

// Device name with MAC address
static char s_device_name[32];

// Forward declarations
static int ble_gap_event_handler(struct ble_gap_event *event, void *arg);
static void ble_host_task(void *param);
static void ble_on_sync(void);
static void ble_on_reset(int reason);

// Address type
static uint8_t ble_addr_type;
static int ble_gatt_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);

// GATT service definition
static const struct ble_gatt_svc_def nus_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                // TX characteristic (ESP32 -> App, Notify)
                .uuid = &nus_tx_char_uuid.u,
                .access_cb = ble_gatt_chr_access,
                .val_handle = &s_tx_attr_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                // RX characteristic (App -> ESP32, Write)
                .uuid = &nus_rx_char_uuid.u,
                .access_cb = ble_gatt_chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                // CAN stream characteristic (ESP32 -> App, Notify)
                .uuid = &nus_can_char_uuid.u,
                .access_cb = ble_gatt_chr_access,
                .val_handle = &s_can_attr_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                // GPS data characteristic (ESP32 -> App, Notify)
                .uuid = &nus_gps_char_uuid.u,
                .access_cb = ble_gatt_chr_access,
                .val_handle = &s_gps_attr_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {
                0, // Terminator
            },
        },
    },
    {
        0, // Terminator
    },
};

static int ble_gatt_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const ble_uuid_t *uuid = ctxt->chr->uuid;

    // Check if this is the RX characteristic (write from app)
    if (ble_uuid_cmp(uuid, &nus_rx_char_uuid.u) == 0) {
        if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);

            // Binary mode: pass raw data directly to callback
            bool json_in_binary_mode = false;
            if (s_binary_mode && s_binary_rx_callback != NULL) {
                if (len > 0 && len <= BINARY_RX_BUFFER_SIZE) {
                    ble_hs_mbuf_to_flat(ctxt->om, s_binary_rx_buffer, len, NULL);

                    // Check if this looks like a JSON command
                    bool is_json = (len >= 6 &&
                                    s_binary_rx_buffer[0] == '{' &&
                                    s_binary_rx_buffer[1] == '"' &&
                                    s_binary_rx_buffer[2] == 'c' &&
                                    s_binary_rx_buffer[3] == 'm' &&
                                    s_binary_rx_buffer[4] == 'd');
                    if (!is_json) {
                        s_binary_rx_callback(s_binary_rx_buffer, len);
                        return 0;
                    }
                    json_in_binary_mode = true;
                }
            }

            // Text mode: buffer and look for newlines
            if (len > 0 && s_rx_buffer_len + len < RX_BUFFER_SIZE - 1) {
                if (json_in_binary_mode) {
                    memcpy(s_rx_buffer + s_rx_buffer_len, s_binary_rx_buffer, len);
                } else {
                    ble_hs_mbuf_to_flat(ctxt->om, s_rx_buffer + s_rx_buffer_len,
                                        len, NULL);
                }
                s_rx_buffer_len += len;
                s_rx_buffer[s_rx_buffer_len] = '\0';

                // Check for complete message (newline-terminated)
                char *newline = strchr(s_rx_buffer, '\n');
                while (newline != NULL) {
                    *newline = '\0';

                    if (s_rx_callback != NULL && s_rx_buffer_len > 0) {
                        s_rx_callback(s_rx_buffer, strlen(s_rx_buffer));
                    }

                    size_t consumed = newline - s_rx_buffer + 1;
                    size_t remaining = s_rx_buffer_len - consumed;
                    if (remaining > 0) {
                        memmove(s_rx_buffer, newline + 1, remaining);
                    }
                    s_rx_buffer_len = remaining;
                    s_rx_buffer[s_rx_buffer_len] = '\0';

                    newline = strchr(s_rx_buffer, '\n');
                }
            }
            return 0;
        }
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static void ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting advertisement data: rc=%d", rc);
        return;
    }

    // Set scan response with service UUID
    struct ble_hs_adv_fields rsp_fields;
    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.uuids128 = (ble_uuid128_t[]) { nus_service_uuid };
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting scan response: rc=%d", rc);
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(20);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(40);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error starting advertisement: rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "Advertising started as '%s'", name);
}

static int ble_gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_connected = true;
            s_rx_buffer_len = 0;
            ESP_LOGI(TAG, "BLE client connected, handle=%d", s_conn_handle);

            // Set preferred MTU - client will initiate exchange
            // (We're a GATT server so we can't initiate MTU exchange)
            ble_att_set_preferred_mtu(512);

            // Request faster connection parameters for higher throughput
            // Min interval: 7.5ms, Max interval: 15ms, Latency: 0, Timeout: 2s
            struct ble_gap_upd_params conn_params = {
                .itvl_min = BLE_GAP_INITIAL_CONN_ITVL_MIN,  // 7.5ms
                .itvl_max = 12,  // 15ms (units of 1.25ms)
                .latency = 0,
                .supervision_timeout = 200,  // 2 seconds (units of 10ms)
                .min_ce_len = 0,
                .max_ce_len = 0,
            };
            int rc = ble_gap_update_params(s_conn_handle, &conn_params);
            if (rc != 0) {
                ESP_LOGW(TAG, "Connection param update failed: rc=%d", rc);
            }

            if (s_connection_callback) {
                s_connection_callback(true);
            }
        } else {
            ESP_LOGW(TAG, "Connection failed, status=%d", event->connect.status);
            ble_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE client disconnected, reason=%d",
                 event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_connected = false;
        s_rx_buffer_len = 0;
        s_current_mtu = 23;  // Reset to default
        if (s_connection_callback) {
            s_connection_callback(false);
        }
        ble_advertise();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "Advertising complete");
        ble_advertise();
        break;

    case BLE_GAP_EVENT_MTU:
        s_current_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU updated: conn_handle=%d, mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe event: handle=%d, attr_handle=%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle);
        break;

    default:
        break;
    }

    return 0;
}

static void ble_on_sync(void)
{
    int rc = ble_hs_id_infer_auto(0, &ble_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error determining address type: rc=%d", rc);
        return;
    }

    ble_advertise();
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_nus_init(const char *device_name_prefix, ble_rx_callback_t rx_callback)
{
    esp_err_t ret;
    int rc;

    s_rx_callback = rx_callback;

    // Get MAC address and build device name
    uint8_t mac[6];
    ret = esp_read_mac(mac, ESP_MAC_BT);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read BT MAC, using base MAC");
        esp_read_mac(mac, ESP_MAC_BASE);
    }
    snprintf(s_device_name, sizeof(s_device_name), "%s_%02X%02X%02X",
             device_name_prefix, mac[3], mac[4], mac[5]);

    ESP_LOGI(TAG, "BLE device name: %s", s_device_name);

    // Initialize NimBLE
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NimBLE: %d", ret);
        return ret;
    }

    // Configure NimBLE host
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    // Initialize GATT services
    ble_svc_gap_init();
    ble_svc_gatt_init();

    // Set device name
    rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error setting device name: rc=%d", rc);
        return ESP_FAIL;
    }

    // Register GATT services
    rc = ble_gatts_count_cfg(nus_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error counting GATT config: rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_gatts_add_svcs(nus_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error adding GATT services: rc=%d", rc);
        return ESP_FAIL;
    }

    // Start NimBLE host task
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE NUS initialized, device name: %s", s_device_name);
    return ESP_OK;
}

esp_err_t ble_nus_send(const char *data, size_t len)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    if (len == 0) {
        len = strlen(data);
    }

    // Allocate buffer with space for newline terminator (avoid separate notification)
    char *buf = malloc(len + 1);
    if (buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate send buffer");
        return ESP_ERR_NO_MEM;
    }
    memcpy(buf, data, len);
    buf[len] = '\n';
    size_t total_len = len + 1;

    // Use MTU-based chunk size (MTU - 3 for ATT header)
    const size_t chunk_size = (s_current_mtu > 3) ? (s_current_mtu - 3) : 20;
    size_t offset = 0;
    esp_err_t result = ESP_OK;

    while (offset < total_len) {
        size_t send_len = (total_len - offset > chunk_size) ? chunk_size : (total_len - offset);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf + offset, send_len);
        if (om == NULL) {
            ESP_LOGE(TAG, "Failed to allocate mbuf");
            result = ESP_ERR_NO_MEM;
            break;
        }

        int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_attr_handle, om);
        if (rc != 0) {
            ESP_LOGE(TAG, "Error sending notification: rc=%d", rc);
            status_led_signal_error();
            result = ESP_FAIL;
            break;
        }

        offset += send_len;

        if (offset < total_len) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }

    free(buf);
    return result;
}

esp_err_t ble_nus_send_binary(const uint8_t *data, size_t len)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t chunk_size = 180;
    size_t offset = 0;

    while (offset < len) {
        size_t send_len = (len - offset > chunk_size) ? chunk_size : (len - offset);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + offset, send_len);
        if (om == NULL) {
            ESP_LOGE(TAG, "Failed to allocate mbuf");
            return ESP_ERR_NO_MEM;
        }

        int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_attr_handle, om);
        if (rc != 0) {
            ESP_LOGE(TAG, "Error sending notification: rc=%d", rc);
            status_led_signal_error();
            return ESP_FAIL;
        }

        offset += send_len;

        if (offset < len) {
            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }

    return ESP_OK;
}

esp_err_t ble_nus_send_can(const uint8_t *data, size_t len)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mbuf for CAN");
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_can_attr_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error sending CAN notification: rc=%d", rc);
        status_led_signal_error();
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t ble_nus_send_gps(const char *json, size_t len)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }

    if (json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (len == 0) {
        len = strlen(json);
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(json, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "Failed to allocate mbuf for GPS");
        return ESP_ERR_NO_MEM;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_gps_attr_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Error sending GPS notification: rc=%d", rc);
        status_led_signal_error();
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool ble_nus_is_connected(void)
{
    return s_connected;
}

uint16_t ble_nus_get_conn_handle(void)
{
    return s_conn_handle;
}

void ble_nus_set_connection_callback(ble_connection_callback_t callback)
{
    s_connection_callback = callback;
}

void ble_nus_deinit(void)
{
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    }
    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

void ble_nus_set_binary_mode(bool enabled, ble_binary_rx_callback_t callback)
{
    s_binary_mode = enabled;
    s_binary_rx_callback = callback;
    s_rx_buffer_len = 0;
    s_rx_buffer[0] = '\0';
    ESP_LOGI(TAG, "Binary mode %s", enabled ? "enabled" : "disabled");
}

bool ble_nus_is_binary_mode(void)
{
    return s_binary_mode;
}

const char* ble_nus_get_device_name(void)
{
    return s_device_name;
}

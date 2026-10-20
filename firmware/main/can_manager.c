/**
 * @file can_manager.c
 * @brief CAN bus manager implementation
 */

#include "can_manager.h"
#include "can_buffer.h"
#include "pin_config.h"
#include "mcp2515.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// Suppress legacy TWAI deprecation warning (TODO: migrate to new esp_twai.h API)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#include "driver/twai.h"
#pragma GCC diagnostic pop
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "can_manager";

// Task handles
static TaskHandle_t s_can0_task_handle = NULL;
static TaskHandle_t s_can1_task_handle = NULL;

// Running state
static volatile bool s_running = false;

// Statistics
static uint32_t s_can0_rx_count = 0;
static uint32_t s_can0_error_count = 0;
static uint32_t s_can1_rx_count = 0;
static uint32_t s_can1_error_count = 0;

// MCP2515 handle
static mcp2515_handle_t s_mcp2515_handle = NULL;

// -----------------------------------------------------------------------------
// CAN0 (TWAI) Task
// -----------------------------------------------------------------------------
static void can0_task(void *arg)
{
    ESP_LOGI(TAG, "CAN0 (TWAI) task started");
    uint32_t loop_count = 0;

    while (s_running) {
        twai_message_t rx_msg;

        // Periodic status log every 5 seconds
        if ((loop_count++ % 500) == 0) {
            twai_status_info_t status;
            twai_get_status_info(&status);
            ESP_LOGI(TAG, "CAN0 status: state=%d rx=%lu tx=%lu rx_err=%lu tx_err=%lu bus_err=%lu arb_lost=%lu",
                     status.state, (unsigned long)s_can0_rx_count,
                     (unsigned long)status.msgs_to_tx,
                     (unsigned long)status.rx_error_counter,
                     (unsigned long)status.tx_error_counter,
                     (unsigned long)status.bus_error_count,
                     (unsigned long)status.arb_lost_count);

            // Auto-recover from bus-off state
            if (status.state == TWAI_STATE_BUS_OFF) {
                ESP_LOGW(TAG, "CAN0 bus-off detected, initiating recovery...");
                twai_initiate_recovery();
            } else if (status.state == TWAI_STATE_RECOVERING) {
                ESP_LOGI(TAG, "CAN0 recovering from bus-off...");
            }
        }

        // Wait for message with 10ms timeout
        esp_err_t ret = twai_receive(&rx_msg, pdMS_TO_TICKS(10));

        if (ret == ESP_OK) {
            s_can0_rx_count++;

            ESP_LOGI(TAG, "CAN0 RX: ID=0x%lX DLC=%d Data=[%02X %02X %02X %02X %02X %02X %02X %02X]",
                     (unsigned long)rx_msg.identifier, rx_msg.data_length_code,
                     rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3],
                     rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);

            // Build buffer message
            can_message_t buf_msg = {
                .channel = CAN_CHANNEL_0,
                .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
                .id = rx_msg.identifier,
                .flags = 0,
                .dlc = rx_msg.data_length_code,
            };

            if (rx_msg.extd) {
                buf_msg.flags |= CAN_FLAG_EXTENDED_ID;
            }
            if (rx_msg.rtr) {
                buf_msg.flags |= CAN_FLAG_RTR;
            }

            memcpy(buf_msg.data, rx_msg.data, 8);

            // Push to shared buffer
            can_buffer_push(&buf_msg);

        } else if (ret != ESP_ERR_TIMEOUT) {
            s_can0_error_count++;
            ESP_LOGW(TAG, "CAN0 receive error: %s (count=%lu)", esp_err_to_name(ret),
                     (unsigned long)s_can0_error_count);
        }
    }

    ESP_LOGI(TAG, "CAN0 task stopped");
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------
// CAN1 (MCP2515) Task
// -----------------------------------------------------------------------------
static void can1_task(void *arg)
{
    ESP_LOGI(TAG, "CAN1 (MCP2515) task started");
    uint32_t loop_count = 0;

    while (s_running) {
        mcp2515_message_t rx_msg;

        // Periodic status log every 5 seconds
        if ((loop_count++ % 500) == 0) {
            uint8_t mcp_status = 0;
            mcp2515_read_status(s_mcp2515_handle, &mcp_status);
            ESP_LOGI(TAG, "CAN1 status: mcp_status=0x%02X rx=%lu err=%lu",
                     mcp_status, (unsigned long)s_can1_rx_count,
                     (unsigned long)s_can1_error_count);

        }

        // Check for received message
        esp_err_t ret = mcp2515_receive(s_mcp2515_handle, &rx_msg, pdMS_TO_TICKS(10));

        if (ret == ESP_OK) {
            s_can1_rx_count++;

            ESP_LOGI(TAG, "CAN1 RX: ID=0x%lX DLC=%d Data=[%02X %02X %02X %02X %02X %02X %02X %02X]",
                     (unsigned long)rx_msg.id, rx_msg.dlc,
                     rx_msg.data[0], rx_msg.data[1], rx_msg.data[2], rx_msg.data[3],
                     rx_msg.data[4], rx_msg.data[5], rx_msg.data[6], rx_msg.data[7]);

            // Build buffer message
            can_message_t buf_msg = {
                .channel = CAN_CHANNEL_1,
                .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
                .id = rx_msg.id,
                .flags = 0,
                .dlc = rx_msg.dlc,
            };

            if (rx_msg.extended) {
                buf_msg.flags |= CAN_FLAG_EXTENDED_ID;
            }
            if (rx_msg.rtr) {
                buf_msg.flags |= CAN_FLAG_RTR;
            }

            memcpy(buf_msg.data, rx_msg.data, 8);

            // Push to shared buffer
            can_buffer_push(&buf_msg);

        } else if (ret != ESP_ERR_TIMEOUT) {
            s_can1_error_count++;
            ESP_LOGW(TAG, "CAN1 receive error: %s (count=%lu)", esp_err_to_name(ret),
                     (unsigned long)s_can1_error_count);
        }
    }

    ESP_LOGI(TAG, "CAN1 task stopped");
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------
// TWAI (CAN0) Initialization
// -----------------------------------------------------------------------------
static esp_err_t init_twai(can_bitrate_t bitrate)
{
    // Log GPIO states before init
    gpio_set_direction(CAN0_RX_GPIO, GPIO_MODE_INPUT);
    int rx_level = gpio_get_level(CAN0_RX_GPIO);
    ESP_LOGI(TAG, "TWAI pre-init: RX GPIO %d level = %d (should be 1 if bus idle/recessive)",
             CAN0_RX_GPIO, rx_level);

    ESP_LOGI(TAG, "TWAI config: bitrate=%lubps, TX=%d, RX=%d",
             (unsigned long)bitrate, CAN0_TX_GPIO, CAN0_RX_GPIO);

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        CAN0_TX_GPIO, CAN0_RX_GPIO, TWAI_MODE_NORMAL);
    g_config.rx_queue_len = 32;
    g_config.tx_queue_len = 8;

    // Select timing based on bitrate
    twai_timing_config_t t_config;
    switch (bitrate) {
        case CAN_BITRATE_125K:
            t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS();
            break;
        case CAN_BITRATE_250K:
            t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS();
            break;
        case CAN_BITRATE_500K:
            t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS();
            break;
        case CAN_BITRATE_1M:
            t_config = (twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS();
            break;
        default:
            ESP_LOGE(TAG, "Invalid bitrate");
            return ESP_ERR_INVALID_ARG;
    }

    // Accept all messages (no filtering)
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t ret = twai_driver_install(&g_config, &t_config, &f_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "TWAI driver installed (TX=%d, RX=%d)", CAN0_TX_GPIO, CAN0_RX_GPIO);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// MCP2515 (CAN1) Initialization
// -----------------------------------------------------------------------------
static esp_err_t init_mcp2515(can_bitrate_t bitrate)
{
    ESP_LOGI(TAG, "MCP2515 config: crystal=%luHz, bitrate=%lubps",
             (unsigned long)MCP2515_CRYSTAL_FREQ_HZ, (unsigned long)bitrate);
    ESP_LOGI(TAG, "MCP2515 SPI pins: MOSI=%d, MISO=%d, CLK=%d, CS=%d, INT=%d",
             MCP2515_MOSI_GPIO, MCP2515_MISO_GPIO, MCP2515_CLK_GPIO,
             MCP2515_CS_GPIO, MCP2515_INT_GPIO);

    mcp2515_config_t config = {
        .spi_host = MCP2515_SPI_HOST,
        .mosi_gpio = MCP2515_MOSI_GPIO,
        .miso_gpio = MCP2515_MISO_GPIO,
        .clk_gpio = MCP2515_CLK_GPIO,
        .cs_gpio = MCP2515_CS_GPIO,
        .int_gpio = MCP2515_INT_GPIO,
        .spi_freq_hz = MCP2515_SPI_FREQ_HZ,
        .crystal_freq_hz = MCP2515_CRYSTAL_FREQ_HZ,
        .bitrate = bitrate,
    };

    esp_err_t ret = mcp2515_init(&config, &s_mcp2515_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MCP2515 init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "MCP2515 initialized (CS=%d, INT=%d)", MCP2515_CS_GPIO, MCP2515_INT_GPIO);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// TWAI Loopback Self-Test
// -----------------------------------------------------------------------------
static esp_err_t twai_loopback_test(void)
{
    ESP_LOGI(TAG, "Running TWAI loopback self-test...");

    // Reconfigure TWAI in loopback mode temporarily
    twai_driver_uninstall();

    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(
        CAN0_TX_GPIO, CAN0_RX_GPIO, TWAI_MODE_NO_ACK);  // No-ack mode for self-test
    g_config.rx_queue_len = 8;
    g_config.tx_queue_len = 4;

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_250KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t ret = twai_driver_install(&g_config, &t_config, &f_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback test: driver install failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = twai_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback test: start failed: %s", esp_err_to_name(ret));
        twai_driver_uninstall();
        return ret;
    }

    // Send a test message
    twai_message_t tx_msg = {
        .identifier = 0x7FF,
        .data_length_code = 4,
        .data = {0xDE, 0xAD, 0xBE, 0xEF, 0, 0, 0, 0},
        .self = 1,  // Self-reception request
    };

    ret = twai_transmit(&tx_msg, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback test: transmit failed: %s", esp_err_to_name(ret));
        twai_stop();
        twai_driver_uninstall();
        return ret;
    }

    // Try to receive it back
    twai_message_t rx_msg;
    ret = twai_receive(&rx_msg, pdMS_TO_TICKS(100));

    twai_stop();
    twai_driver_uninstall();

    if (ret == ESP_OK) {
        if (rx_msg.identifier == 0x7FF && rx_msg.data[0] == 0xDE) {
            ESP_LOGI(TAG, "Loopback test: PASSED - TWAI controller is working");
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "Loopback test: received unexpected data");
            return ESP_ERR_INVALID_RESPONSE;
        }
    } else {
        ESP_LOGW(TAG, "Loopback test: no self-reception (ret=%s) - this may be normal",
                 esp_err_to_name(ret));
        return ESP_OK;  // Don't fail, self-reception might not work on all configs
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

esp_err_t can_manager_init(can_bitrate_t bitrate)
{
    ESP_LOGI(TAG, "Initializing CAN manager at %lu bps", (unsigned long)bitrate);

    // Initialize CAN buffer first
    esp_err_t ret = can_buffer_init();
    if (ret != ESP_OK) {
        return ret;
    }

    // Run TWAI self-test first (in loopback mode)
    twai_loopback_test();

    // Initialize TWAI (CAN0) in normal mode
    ret = init_twai(bitrate);
    if (ret != ESP_OK) {
        return ret;
    }

    // Initialize MCP2515 (CAN1)
    ret = init_mcp2515(bitrate);
    if (ret != ESP_OK) {
        twai_driver_uninstall();
        return ret;
    }

    ESP_LOGI(TAG, "CAN manager initialized successfully");
    return ESP_OK;
}

esp_err_t can_manager_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "CAN manager already running");
        return ESP_OK;
    }

    // Start TWAI
    esp_err_t ret = twai_start();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start MCP2515
    ret = mcp2515_start(s_mcp2515_handle);
    if (ret != ESP_OK) {
        twai_stop();
        ESP_LOGE(TAG, "MCP2515 start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_running = true;

    // Create receive tasks
    xTaskCreate(can0_task, "can0_task", 4096, NULL, 10, &s_can0_task_handle);
    xTaskCreate(can1_task, "can1_task", 4096, NULL, 10, &s_can1_task_handle);

    ESP_LOGI(TAG, "CAN manager started");
    return ESP_OK;
}

esp_err_t can_manager_stop(void)
{
    if (!s_running) {
        return ESP_OK;
    }

    s_running = false;

    // Wait for tasks to finish
    vTaskDelay(pdMS_TO_TICKS(100));

    twai_stop();
    mcp2515_stop(s_mcp2515_handle);

    ESP_LOGI(TAG, "CAN manager stopped");
    return ESP_OK;
}

bool can_manager_is_running(void)
{
    return s_running;
}

void can_manager_get_stats(can_channel_t channel, uint32_t *rx_count, uint32_t *error_count)
{
    if (channel == CAN_CHANNEL_0) {
        if (rx_count) *rx_count = s_can0_rx_count;
        if (error_count) *error_count = s_can0_error_count;
    } else {
        if (rx_count) *rx_count = s_can1_rx_count;
        if (error_count) *error_count = s_can1_error_count;
    }
}

void can_manager_reset_stats(void)
{
    s_can0_rx_count = 0;
    s_can0_error_count = 0;
    s_can1_rx_count = 0;
    s_can1_error_count = 0;
    ESP_LOGI(TAG, "CAN statistics reset");
}

esp_err_t can_manager_send(can_channel_t channel, uint32_t id,
                           const uint8_t *data, uint8_t len, bool extended_id)
{
    if (len > 8) {
        return ESP_ERR_INVALID_ARG;
    }

    if (channel == CAN_CHANNEL_0) {
        twai_message_t msg = {
            .identifier = id,
            .data_length_code = len,
            .extd = extended_id,
            .rtr = false,
            .ss = false,
            .self = false,
        };
        if (data && len > 0) {
            memcpy(msg.data, data, len);
        }
        return twai_transmit(&msg, pdMS_TO_TICKS(100));
    } else {
        mcp2515_message_t msg = {
            .id = id,
            .dlc = len,
            .extended = extended_id,
            .rtr = false,
        };
        if (data && len > 0) {
            memcpy(msg.data, data, len);
        }
        return mcp2515_send(s_mcp2515_handle, &msg, pdMS_TO_TICKS(100));
    }
}

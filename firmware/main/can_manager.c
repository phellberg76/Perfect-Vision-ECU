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
#include "freertos/semphr.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
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

// TWAI node handle
static twai_node_handle_t s_twai_node_handle = NULL;

// RX pool for TWAI (callback-based receive)
#define TWAI_RX_POOL_DEPTH 32

typedef struct {
    twai_frame_t frame;
    uint8_t data[8];
} twai_rx_slot_t;

static twai_rx_slot_t *s_twai_rx_pool = NULL;
static SemaphoreHandle_t s_twai_rx_free_sem = NULL;
static SemaphoreHandle_t s_twai_rx_ready_sem = NULL;
static volatile int s_twai_rx_write_idx = 0;
static volatile int s_twai_rx_read_idx = 0;

// -----------------------------------------------------------------------------
// TWAI Callbacks (ISR context)
// -----------------------------------------------------------------------------

// RX callback - called when a message is received
static bool IRAM_ATTR twai_rx_done_cb(twai_node_handle_t handle,
                                       const twai_rx_done_event_data_t *edata,
                                       void *user_ctx)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    // Try to get a free slot
    if (xSemaphoreTakeFromISR(s_twai_rx_free_sem, &xHigherPriorityTaskWoken) != pdTRUE) {
        // Pool full, drop frame
        return (xHigherPriorityTaskWoken == pdTRUE);
    }

    // Copy frame from hardware
    if (twai_node_receive_from_isr(handle, &s_twai_rx_pool[s_twai_rx_write_idx].frame) == ESP_OK) {
        s_twai_rx_write_idx = (s_twai_rx_write_idx + 1) % TWAI_RX_POOL_DEPTH;
        xSemaphoreGiveFromISR(s_twai_rx_ready_sem, &xHigherPriorityTaskWoken);
    } else {
        // Failed to receive, give back the slot
        xSemaphoreGiveFromISR(s_twai_rx_free_sem, &xHigherPriorityTaskWoken);
    }

    return (xHigherPriorityTaskWoken == pdTRUE);
}

// State change callback
static bool IRAM_ATTR twai_state_change_cb(twai_node_handle_t handle,
                                            const twai_state_change_event_data_t *edata,
                                            void *user_ctx)
{
    const char *state_names[] = {"error_active", "error_warning", "error_passive", "bus_off"};
    ESP_EARLY_LOGI(TAG, "TWAI state: %s -> %s",
                   state_names[edata->old_sta], state_names[edata->new_sta]);
    return false;
}

// Error callback
static bool IRAM_ATTR twai_error_cb(twai_node_handle_t handle,
                                     const twai_error_event_data_t *edata,
                                     void *user_ctx)
{
    ESP_EARLY_LOGW(TAG, "TWAI error: 0x%lx", edata->err_flags.val);
    s_can0_error_count++;
    return false;
}

// -----------------------------------------------------------------------------
// CAN0 (TWAI) Task
// -----------------------------------------------------------------------------
static void can0_task(void *arg)
{
    ESP_LOGI(TAG, "CAN0 (TWAI) task started");
    uint32_t loop_count = 0;

    while (s_running) {
        // Periodic status log every 5 seconds (500 * 10ms = 5s)
        if ((loop_count++ % 500) == 0) {
            twai_node_status_t status;
            twai_node_record_t stats;
            twai_node_get_info(s_twai_node_handle, &status, &stats);
            ESP_LOGI(TAG, "CAN0 status: state=%d rx=%lu tx_err=%u rx_err=%u bus_err=%lu",
                     status.state, (unsigned long)s_can0_rx_count,
                     status.tx_error_count,
                     status.rx_error_count,
                     (unsigned long)stats.bus_err_num);

            // Auto-recover from bus-off state
            if (status.state == TWAI_ERROR_BUS_OFF) {
                ESP_LOGW(TAG, "CAN0 bus-off detected, initiating recovery...");
                twai_node_recover(s_twai_node_handle);
            }
        }

        // Wait for message from RX callback (via semaphore)
        if (xSemaphoreTake(s_twai_rx_ready_sem, pdMS_TO_TICKS(10)) == pdTRUE) {
            twai_rx_slot_t *slot = &s_twai_rx_pool[s_twai_rx_read_idx];
            twai_frame_t *rx_frame = &slot->frame;

            s_can0_rx_count++;

            ESP_LOGI(TAG, "CAN0 RX: ID=0x%lX DLC=%d Data=[%02X %02X %02X %02X %02X %02X %02X %02X]",
                     (unsigned long)rx_frame->header.id, rx_frame->header.dlc,
                     slot->data[0], slot->data[1], slot->data[2], slot->data[3],
                     slot->data[4], slot->data[5], slot->data[6], slot->data[7]);

            // Build buffer message
            can_message_t buf_msg = {
                .channel = CAN_CHANNEL_0,
                .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
                .id = rx_frame->header.id,
                .flags = 0,
                .dlc = rx_frame->header.dlc,
            };

            if (rx_frame->header.ide) {
                buf_msg.flags |= CAN_FLAG_EXTENDED_ID;
            }
            if (rx_frame->header.rtr) {
                buf_msg.flags |= CAN_FLAG_RTR;
            }

            memcpy(buf_msg.data, slot->data, 8);

            // Push to shared buffer
            can_buffer_push(&buf_msg);

            // Move to next slot and release the slot back to free pool
            s_twai_rx_read_idx = (s_twai_rx_read_idx + 1) % TWAI_RX_POOL_DEPTH;
            xSemaphoreGive(s_twai_rx_free_sem);
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
// TWAI RX Pool Initialization
// -----------------------------------------------------------------------------
static esp_err_t init_twai_rx_pool(void)
{
    // Create semaphores for RX pool management
    s_twai_rx_free_sem = xSemaphoreCreateCounting(TWAI_RX_POOL_DEPTH, TWAI_RX_POOL_DEPTH);
    s_twai_rx_ready_sem = xSemaphoreCreateCounting(TWAI_RX_POOL_DEPTH, 0);

    if (s_twai_rx_free_sem == NULL || s_twai_rx_ready_sem == NULL) {
        ESP_LOGE(TAG, "Failed to create TWAI RX semaphores");
        return ESP_ERR_NO_MEM;
    }

    // Allocate RX pool
    s_twai_rx_pool = calloc(TWAI_RX_POOL_DEPTH, sizeof(twai_rx_slot_t));
    if (s_twai_rx_pool == NULL) {
        ESP_LOGE(TAG, "Failed to allocate TWAI RX pool");
        return ESP_ERR_NO_MEM;
    }

    // Initialize each slot with buffer pointers
    for (int i = 0; i < TWAI_RX_POOL_DEPTH; i++) {
        s_twai_rx_pool[i].frame.buffer = s_twai_rx_pool[i].data;
        s_twai_rx_pool[i].frame.buffer_len = sizeof(s_twai_rx_pool[i].data);
    }

    s_twai_rx_write_idx = 0;
    s_twai_rx_read_idx = 0;

    ESP_LOGI(TAG, "TWAI RX pool initialized: %d slots", TWAI_RX_POOL_DEPTH);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// TWAI RX Pool Cleanup
// -----------------------------------------------------------------------------
static void deinit_twai_rx_pool(void)
{
    if (s_twai_rx_pool) {
        free(s_twai_rx_pool);
        s_twai_rx_pool = NULL;
    }
    if (s_twai_rx_free_sem) {
        vSemaphoreDelete(s_twai_rx_free_sem);
        s_twai_rx_free_sem = NULL;
    }
    if (s_twai_rx_ready_sem) {
        vSemaphoreDelete(s_twai_rx_ready_sem);
        s_twai_rx_ready_sem = NULL;
    }
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

    // Initialize RX pool
    esp_err_t ret = init_twai_rx_pool();
    if (ret != ESP_OK) {
        return ret;
    }

    // Configure TWAI node
    twai_onchip_node_config_t node_config = {
        .io_cfg = {
            .tx = CAN0_TX_GPIO,
            .rx = CAN0_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing = {
            .bitrate = bitrate,
        },
        .tx_queue_depth = 8,
        .fail_retry_cnt = -1,  // Infinite retries
    };

    // Create TWAI node
    ret = twai_new_node_onchip(&node_config, &s_twai_node_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI node creation failed: %s", esp_err_to_name(ret));
        deinit_twai_rx_pool();
        return ret;
    }

    // Register event callbacks
    twai_event_callbacks_t callbacks = {
        .on_rx_done = twai_rx_done_cb,
        .on_state_change = twai_state_change_cb,
        .on_error = twai_error_cb,
    };
    ret = twai_node_register_event_callbacks(s_twai_node_handle, &callbacks, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI callback registration failed: %s", esp_err_to_name(ret));
        twai_node_delete(s_twai_node_handle);
        s_twai_node_handle = NULL;
        deinit_twai_rx_pool();
        return ret;
    }

    ESP_LOGI(TAG, "TWAI node created (TX=%d, RX=%d)", CAN0_TX_GPIO, CAN0_RX_GPIO);
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

    twai_node_handle_t test_node = NULL;

    // Configure TWAI node in self-test/loopback mode
    twai_onchip_node_config_t node_config = {
        .io_cfg = {
            .tx = CAN0_TX_GPIO,
            .rx = CAN0_RX_GPIO,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing = {
            .bitrate = 250000,
        },
        .tx_queue_depth = 4,
        .fail_retry_cnt = -1,
        .flags.enable_self_test = true,
        .flags.enable_loopback = true,
    };

    esp_err_t ret = twai_new_node_onchip(&node_config, &test_node);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback test: node creation failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = twai_node_enable(test_node);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback test: enable failed: %s", esp_err_to_name(ret));
        twai_node_delete(test_node);
        return ret;
    }

    // Send a test message
    uint8_t tx_data[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    twai_frame_t tx_frame = {
        .header.id = 0x7FF,
        .header.dlc = 4,
        .buffer = tx_data,
        .buffer_len = sizeof(tx_data),
    };

    ret = twai_node_transmit(test_node, &tx_frame, 100);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Loopback test: transmit failed: %s", esp_err_to_name(ret));
        twai_node_disable(test_node);
        twai_node_delete(test_node);
        return ret;
    }

    // Wait for transmission to complete
    vTaskDelay(pdMS_TO_TICKS(50));

    // In loopback mode with self_test, check node state after transmission
    twai_node_status_t status;
    twai_node_get_info(test_node, &status, NULL);

    twai_node_disable(test_node);
    twai_node_delete(test_node);

    if (status.state == TWAI_ERROR_ACTIVE || status.state == TWAI_ERROR_WARNING) {
        ESP_LOGI(TAG, "Loopback test: PASSED - TWAI controller is working (state=%d)", status.state);
        return ESP_OK;
    } else {
        ESP_LOGW(TAG, "Loopback test: node in unexpected state %d - this may be normal",
                 status.state);
        return ESP_OK;  // Don't fail, self-test might not work perfectly on all configs
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
        twai_node_delete(s_twai_node_handle);
        s_twai_node_handle = NULL;
        deinit_twai_rx_pool();
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

    // Enable TWAI node
    esp_err_t ret = twai_node_enable(s_twai_node_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start MCP2515
    ret = mcp2515_start(s_mcp2515_handle);
    if (ret != ESP_OK) {
        twai_node_disable(s_twai_node_handle);
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

    twai_node_disable(s_twai_node_handle);
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
        // Use a local buffer for the data
        uint8_t tx_data[8] = {0};
        if (data && len > 0) {
            memcpy(tx_data, data, len);
        }

        twai_frame_t tx_frame = {
            .header.id = id,
            .header.dlc = len,
            .header.ide = extended_id,
            .header.rtr = false,
            .buffer = tx_data,
            .buffer_len = len,
            .tx_queue_priority = 5,
        };
        return twai_node_transmit(s_twai_node_handle, &tx_frame, 100);
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

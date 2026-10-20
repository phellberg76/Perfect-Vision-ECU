/**
 * @file mcp2515.c
 * @brief MCP2515 CAN Controller Driver Implementation
 */

#include "mcp2515.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include <string.h>

static const char *TAG = "mcp2515";

// MCP2515 SPI Instructions
#define MCP2515_RESET           0xC0
#define MCP2515_READ            0x03
#define MCP2515_WRITE           0x02
#define MCP2515_RTS             0x80
#define MCP2515_RTS_TXB0        0x81
#define MCP2515_RTS_TXB1        0x82
#define MCP2515_RTS_TXB2        0x84
#define MCP2515_READ_STATUS     0xA0
#define MCP2515_RX_STATUS       0xB0
#define MCP2515_BIT_MODIFY      0x05
#define MCP2515_READ_RX0        0x90
#define MCP2515_READ_RX1        0x94
#define MCP2515_LOAD_TX0        0x40
#define MCP2515_LOAD_TX1        0x42
#define MCP2515_LOAD_TX2        0x44

// MCP2515 Registers
#define REG_CANSTAT     0x0E
#define REG_CANCTRL     0x0F
#define REG_CNF3        0x28
#define REG_CNF2        0x29
#define REG_CNF1        0x2A
#define REG_CANINTE     0x2B
#define REG_CANINTF     0x2C
#define REG_TXB0CTRL    0x30
#define REG_TXB1CTRL    0x40
#define REG_TXB2CTRL    0x50
#define REG_RXB0CTRL    0x60
#define REG_RXB1CTRL    0x70

// CANCTRL bits
#define CANCTRL_REQOP_MASK  0xE0
#define CANCTRL_REQOP_SHIFT 5

// CANSTAT bits
#define CANSTAT_OPMOD_MASK  0xE0
#define CANSTAT_OPMOD_SHIFT 5

// CANINTF bits
#define CANINTF_RX0IF   0x01
#define CANINTF_RX1IF   0x02
#define CANINTF_TX0IF   0x04
#define CANINTF_TX1IF   0x08
#define CANINTF_TX2IF   0x10
#define CANINTF_ERRIF   0x20
#define CANINTF_WAKIF   0x40
#define CANINTF_MERRF   0x80

// TXBnCTRL bits
#define TXBCTRL_TXREQ   0x08
#define TXBCTRL_TXP     0x03

// RXBnCTRL bits
#define RXBCTRL_RXM_MASK    0x60
#define RXBCTRL_RXM_ANY     0x60    // Receive any message

// Internal handle structure
struct mcp2515_handle {
    spi_device_handle_t spi;
    gpio_num_t int_gpio;
    SemaphoreHandle_t mutex;
    bool started;
};

// -----------------------------------------------------------------------------
// SPI Communication Helpers
// -----------------------------------------------------------------------------

static esp_err_t spi_write_byte(mcp2515_handle_t handle, uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
    };
    return spi_device_polling_transmit(handle->spi, &t);
}

static esp_err_t spi_transfer(mcp2515_handle_t handle, const uint8_t *tx, uint8_t *rx, size_t len)
{
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(handle->spi, &t);
}

static esp_err_t mcp2515_read_reg(mcp2515_handle_t handle, uint8_t reg, uint8_t *value)
{
    uint8_t tx[3] = {MCP2515_READ, reg, 0x00};
    uint8_t rx[3] = {0};

    esp_err_t ret = spi_transfer(handle, tx, rx, 3);
    if (ret == ESP_OK) {
        *value = rx[2];
    }
    return ret;
}

static esp_err_t mcp2515_write_reg(mcp2515_handle_t handle, uint8_t reg, uint8_t value)
{
    uint8_t tx[3] = {MCP2515_WRITE, reg, value};
    return spi_transfer(handle, tx, NULL, 3);
}

static esp_err_t mcp2515_bit_modify(mcp2515_handle_t handle, uint8_t reg, uint8_t mask, uint8_t value)
{
    uint8_t tx[4] = {MCP2515_BIT_MODIFY, reg, mask, value};
    return spi_transfer(handle, tx, NULL, 4);
}

// -----------------------------------------------------------------------------
// Timing Configuration
// -----------------------------------------------------------------------------

typedef struct {
    uint8_t cnf1;
    uint8_t cnf2;
    uint8_t cnf3;
} mcp2515_timing_t;

static bool get_timing_config(uint32_t crystal_hz, uint32_t bitrate, mcp2515_timing_t *timing)
{
    // Pre-calculated timing values for common configurations
    // Based on sample point ~75%

    if (crystal_hz == 16000000) {  // 16 MHz crystal
        switch (bitrate) {
            case 125000:    // 125 kbps
                timing->cnf1 = 0x07;  // SJW=1, BRP=7 -> TQ = 1us
                timing->cnf2 = 0xB1;  // BTLMODE=1, SAM=0, PHSEG1=6, PRSEG=1
                timing->cnf3 = 0x05;  // PHSEG2=5
                return true;
            case 250000:    // 250 kbps
                timing->cnf1 = 0x03;  // SJW=1, BRP=3 -> TQ = 0.5us
                timing->cnf2 = 0xB1;
                timing->cnf3 = 0x05;
                return true;
            case 500000:    // 500 kbps
                timing->cnf1 = 0x01;  // SJW=1, BRP=1 -> TQ = 0.25us
                timing->cnf2 = 0xB1;
                timing->cnf3 = 0x05;
                return true;
            case 1000000:   // 1 Mbps
                timing->cnf1 = 0x00;  // SJW=1, BRP=0 -> TQ = 0.125us
                timing->cnf2 = 0xB1;
                timing->cnf3 = 0x05;
                return true;
        }
    }
    else if (crystal_hz == 8000000) {  // 8 MHz crystal
        switch (bitrate) {
            case 125000:  // TQ=0.5µs, 16 TQ/bit
                timing->cnf1 = 0x01;  // SJW=1, BRP=1
                timing->cnf2 = 0xB1;  // BTLMODE=1, SAM=0, PHSEG1=6, PRSEG=1
                timing->cnf3 = 0x05;  // PHSEG2=5
                return true;
            case 250000:  // TQ=0.25µs, 16 TQ/bit
                timing->cnf1 = 0x00;  // SJW=1, BRP=0
                timing->cnf2 = 0xB1;
                timing->cnf3 = 0x05;
                return true;
            case 500000:  // TQ=0.25µs, 8 TQ/bit
                timing->cnf1 = 0x00;  // SJW=1, BRP=0
                timing->cnf2 = 0x91;  // BTLMODE=1, SAM=0, PHSEG1=2, PRSEG=1
                timing->cnf3 = 0x02;  // PHSEG2=2
                return true;
        }
    }

    ESP_LOGE(TAG, "Unsupported timing: crystal=%luHz, bitrate=%lubps",
             (unsigned long)crystal_hz, (unsigned long)bitrate);
    return false;
}

// -----------------------------------------------------------------------------
// Public API Implementation
// -----------------------------------------------------------------------------

esp_err_t mcp2515_init(const mcp2515_config_t *config, mcp2515_handle_t *out_handle)
{
    if (config == NULL || out_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Allocate handle
    struct mcp2515_handle *handle = calloc(1, sizeof(struct mcp2515_handle));
    if (handle == NULL) {
        return ESP_ERR_NO_MEM;
    }

    handle->int_gpio = config->int_gpio;
    handle->started = false;

    // Create mutex
    handle->mutex = xSemaphoreCreateMutex();
    if (handle->mutex == NULL) {
        free(handle);
        return ESP_ERR_NO_MEM;
    }

    // Configure SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = config->mosi_gpio,
        .miso_io_num = config->miso_gpio,
        .sclk_io_num = config->clk_gpio,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 64,
    };

    esp_err_t ret = spi_bus_initialize(config->spi_host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {  // Already initialized is OK
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return ret;
    }

    // Add SPI device
    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = config->spi_freq_hz,
        .mode = 0,  // CPOL=0, CPHA=0
        .spics_io_num = config->cs_gpio,
        .queue_size = 1,
        .flags = 0,
    };

    ret = spi_bus_add_device(config->spi_host, &dev_cfg, &handle->spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return ret;
    }

    // Configure interrupt GPIO (input)
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << config->int_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    // Reset MCP2515
    ESP_LOGI(TAG, "Sending SPI reset command...");
    ret = mcp2515_reset(handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI reset command failed: %s", esp_err_to_name(ret));
        spi_bus_remove_device(handle->spi);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return ret;
    }
    ESP_LOGI(TAG, "SPI reset command sent OK");

    // Wait for reset
    vTaskDelay(pdMS_TO_TICKS(10));

    // Verify we're in config mode - this confirms SPI communication is working
    uint8_t canstat = 0;
    ret = mcp2515_read_reg(handle, REG_CANSTAT, &canstat);
    ESP_LOGI(TAG, "SPI read CANSTAT: ret=%s, value=0x%02X (expected 0x80 for config mode)",
             esp_err_to_name(ret), canstat);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI read failed - MCP2515 not responding! Check wiring: MOSI, MISO, CLK, CS");
        spi_bus_remove_device(handle->spi);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return ret;
    }

    if (canstat == 0x00 || canstat == 0xFF) {
        ESP_LOGE(TAG, "MCP2515 returned 0x%02X - likely not connected or powered!", canstat);
        spi_bus_remove_device(handle->spi);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return ESP_ERR_INVALID_RESPONSE;
    }

    if ((canstat & CANSTAT_OPMOD_MASK) != MCP2515_MODE_CONFIG) {
        ESP_LOGE(TAG, "MCP2515 not in config mode after reset: 0x%02X", canstat);
        spi_bus_remove_device(handle->spi);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "MCP2515 SPI communication verified OK");

    // Configure timing
    mcp2515_timing_t timing;
    if (!get_timing_config(config->crystal_freq_hz, config->bitrate, &timing)) {
        spi_bus_remove_device(handle->spi);
        vSemaphoreDelete(handle->mutex);
        free(handle);
        return ESP_ERR_NOT_SUPPORTED;
    }

    mcp2515_write_reg(handle, REG_CNF1, timing.cnf1);
    mcp2515_write_reg(handle, REG_CNF2, timing.cnf2);
    mcp2515_write_reg(handle, REG_CNF3, timing.cnf3);

    // Verify timing registers were written correctly
    uint8_t cnf1_read, cnf2_read, cnf3_read;
    mcp2515_read_reg(handle, REG_CNF1, &cnf1_read);
    mcp2515_read_reg(handle, REG_CNF2, &cnf2_read);
    mcp2515_read_reg(handle, REG_CNF3, &cnf3_read);
    ESP_LOGI(TAG, "Timing registers: CNF1=0x%02X (wrote 0x%02X), CNF2=0x%02X (wrote 0x%02X), CNF3=0x%02X (wrote 0x%02X)",
             cnf1_read, timing.cnf1, cnf2_read, timing.cnf2, cnf3_read, timing.cnf3);

    if (cnf1_read != timing.cnf1 || cnf2_read != timing.cnf2 || cnf3_read != timing.cnf3) {
        ESP_LOGW(TAG, "Timing register mismatch - SPI write may have failed!");
    }

    // Configure RX buffers to receive any message
    mcp2515_write_reg(handle, REG_RXB0CTRL, RXBCTRL_RXM_ANY);
    mcp2515_write_reg(handle, REG_RXB1CTRL, RXBCTRL_RXM_ANY);

    // Enable interrupts (RX only for now)
    mcp2515_write_reg(handle, REG_CANINTE, CANINTF_RX0IF | CANINTF_RX1IF);

    // Verify RX buffer config
    uint8_t rxb0ctrl, rxb1ctrl, caninte;
    mcp2515_read_reg(handle, REG_RXB0CTRL, &rxb0ctrl);
    mcp2515_read_reg(handle, REG_RXB1CTRL, &rxb1ctrl);
    mcp2515_read_reg(handle, REG_CANINTE, &caninte);
    ESP_LOGI(TAG, "RX config: RXB0CTRL=0x%02X, RXB1CTRL=0x%02X, CANINTE=0x%02X",
             rxb0ctrl, rxb1ctrl, caninte);

    ESP_LOGI(TAG, "MCP2515 initialized successfully");
    *out_handle = handle;
    return ESP_OK;
}

esp_err_t mcp2515_deinit(mcp2515_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (handle->started) {
        mcp2515_stop(handle);
    }

    spi_bus_remove_device(handle->spi);
    vSemaphoreDelete(handle->mutex);
    free(handle);

    return ESP_OK;
}

esp_err_t mcp2515_start(mcp2515_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = mcp2515_set_mode(handle, MCP2515_MODE_NORMAL);
    if (ret == ESP_OK) {
        handle->started = true;
        ESP_LOGI(TAG, "MCP2515 started in normal mode");
    }
    return ret;
}

esp_err_t mcp2515_stop(mcp2515_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    handle->started = false;
    esp_err_t ret = mcp2515_set_mode(handle, MCP2515_MODE_CONFIG);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "MCP2515 stopped");
    }
    return ret;
}

esp_err_t mcp2515_send(mcp2515_handle_t handle, const mcp2515_message_t *msg,
                       TickType_t timeout_ticks)
{
    if (handle == NULL || msg == NULL || msg->dlc > 8) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(handle->mutex, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Find free TX buffer
    uint8_t txctrl;
    int tx_buf = -1;
    const uint8_t ctrl_regs[] = {REG_TXB0CTRL, REG_TXB1CTRL, REG_TXB2CTRL};
    const uint8_t load_cmds[] = {MCP2515_LOAD_TX0, MCP2515_LOAD_TX1, MCP2515_LOAD_TX2};
    const uint8_t rts_cmds[] = {MCP2515_RTS_TXB0, MCP2515_RTS_TXB1, MCP2515_RTS_TXB2};

    for (int i = 0; i < 3; i++) {
        mcp2515_read_reg(handle, ctrl_regs[i], &txctrl);
        if ((txctrl & TXBCTRL_TXREQ) == 0) {
            tx_buf = i;
            break;
        }
    }

    if (tx_buf < 0) {
        xSemaphoreGive(handle->mutex);
        return ESP_ERR_TIMEOUT;
    }

    // Build TX data
    uint8_t tx_data[14];
    tx_data[0] = load_cmds[tx_buf];

    if (msg->extended) {
        // Extended ID (29-bit)
        tx_data[1] = (msg->id >> 21) & 0xFF;                    // SIDH
        tx_data[2] = ((msg->id >> 13) & 0xE0) | 0x08 |          // SIDL (EXIDE=1)
                     ((msg->id >> 16) & 0x03);
        tx_data[3] = (msg->id >> 8) & 0xFF;                     // EID8
        tx_data[4] = msg->id & 0xFF;                            // EID0
    } else {
        // Standard ID (11-bit)
        tx_data[1] = (msg->id >> 3) & 0xFF;                     // SIDH
        tx_data[2] = (msg->id << 5) & 0xE0;                     // SIDL
        tx_data[3] = 0;                                          // EID8
        tx_data[4] = 0;                                          // EID0
    }

    tx_data[5] = msg->dlc | (msg->rtr ? 0x40 : 0);              // DLC
    memcpy(&tx_data[6], msg->data, 8);

    // Send data
    esp_err_t ret = spi_transfer(handle, tx_data, NULL, 14);
    if (ret != ESP_OK) {
        xSemaphoreGive(handle->mutex);
        return ret;
    }

    // Request to send
    ret = spi_write_byte(handle, rts_cmds[tx_buf]);

    xSemaphoreGive(handle->mutex);
    return ret;
}

esp_err_t mcp2515_receive(mcp2515_handle_t handle, mcp2515_message_t *msg,
                          TickType_t timeout_ticks)
{
    if (handle == NULL || msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    TickType_t start = xTaskGetTickCount();

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(5)) != pdTRUE) {
            continue;
        }

        // Check interrupt flags
        uint8_t intf;
        mcp2515_read_reg(handle, REG_CANINTF, &intf);

        uint8_t rx_cmd = 0;
        uint8_t clear_flag = 0;

        if (intf & CANINTF_RX0IF) {
            rx_cmd = MCP2515_READ_RX0;
            clear_flag = CANINTF_RX0IF;
        } else if (intf & CANINTF_RX1IF) {
            rx_cmd = MCP2515_READ_RX1;
            clear_flag = CANINTF_RX1IF;
        }

        if (rx_cmd == 0) {
            xSemaphoreGive(handle->mutex);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        // Read RX buffer
        uint8_t tx_data[14] = {rx_cmd};
        uint8_t rx_data[14] = {0};

        esp_err_t ret = spi_transfer(handle, tx_data, rx_data, 14);
        if (ret != ESP_OK) {
            xSemaphoreGive(handle->mutex);
            return ret;
        }

        // Clear interrupt flag
        mcp2515_bit_modify(handle, REG_CANINTF, clear_flag, 0x00);

        xSemaphoreGive(handle->mutex);

        // Parse received data
        uint8_t sidh = rx_data[1];
        uint8_t sidl = rx_data[2];
        uint8_t eid8 = rx_data[3];
        uint8_t eid0 = rx_data[4];
        uint8_t dlc = rx_data[5];

        msg->extended = (sidl & 0x08) != 0;
        msg->rtr = (dlc & 0x40) != 0;
        msg->dlc = dlc & 0x0F;
        if (msg->dlc > 8) msg->dlc = 8;

        if (msg->extended) {
            msg->id = ((uint32_t)sidh << 21) |
                      ((uint32_t)(sidl & 0xE0) << 13) |
                      ((uint32_t)(sidl & 0x03) << 16) |
                      ((uint32_t)eid8 << 8) |
                      (uint32_t)eid0;
        } else {
            msg->id = ((uint32_t)sidh << 3) | ((sidl >> 5) & 0x07);
        }

        memcpy(msg->data, &rx_data[6], 8);

        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t mcp2515_set_mode(mcp2515_handle_t handle, mcp2515_mode_t mode)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(handle->mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    // Write mode to CANCTRL
    esp_err_t ret = mcp2515_bit_modify(handle, REG_CANCTRL, CANCTRL_REQOP_MASK, mode);
    if (ret != ESP_OK) {
        xSemaphoreGive(handle->mutex);
        return ret;
    }

    // Wait for mode change
    for (int i = 0; i < 100; i++) {
        uint8_t canstat;
        ret = mcp2515_read_reg(handle, REG_CANSTAT, &canstat);
        if (ret == ESP_OK && (canstat & CANSTAT_OPMOD_MASK) == mode) {
            xSemaphoreGive(handle->mutex);
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    xSemaphoreGive(handle->mutex);
    ESP_LOGE(TAG, "Mode change timeout");
    return ESP_ERR_TIMEOUT;
}

esp_err_t mcp2515_reset(mcp2515_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    return spi_write_byte(handle, MCP2515_RESET);
}

esp_err_t mcp2515_read_status(mcp2515_handle_t handle, uint8_t *status)
{
    if (handle == NULL || status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t tx[2] = {MCP2515_READ_STATUS, 0x00};
    uint8_t rx[2] = {0};

    esp_err_t ret = spi_transfer(handle, tx, rx, 2);
    if (ret == ESP_OK) {
        *status = rx[1];
    }
    return ret;
}

/**
 * @file can_buffer.c
 * @brief Thread-safe CAN message buffer implementation
 */

#include "can_buffer.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "can_buffer";

static can_message_t s_buffer[CAN_BUFFER_SIZE];
static volatile size_t s_head = 0;
static volatile size_t s_tail = 0;
static volatile size_t s_count = 0;
static SemaphoreHandle_t s_mutex = NULL;
static uint32_t s_overflow_count = 0;
static uint32_t s_total_count = 0;

esp_err_t can_buffer_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_overflow_count = 0;
    s_total_count = 0;

    ESP_LOGI(TAG, "CAN buffer initialized with %d slots", CAN_BUFFER_SIZE);
    return ESP_OK;
}

bool can_buffer_push(const can_message_t *msg)
{
    if (msg == NULL || s_mutex == NULL) {
        return false;
    }

    bool success = false;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_total_count++;

        if (s_count < CAN_BUFFER_SIZE) {
            memcpy(&s_buffer[s_head], msg, sizeof(can_message_t));
            s_head = (s_head + 1) % CAN_BUFFER_SIZE;
            s_count++;
            success = true;
        } else {
            s_overflow_count++;
            // Log overflow periodically
            if ((s_overflow_count % 100) == 1) {
                ESP_LOGW(TAG, "Buffer overflow! Count: %lu", (unsigned long)s_overflow_count);
            }
        }

        xSemaphoreGive(s_mutex);
    }

    return success;
}

size_t can_buffer_count(void)
{
    size_t count = 0;

    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        count = s_count;
        xSemaphoreGive(s_mutex);
    }

    return count;
}

size_t can_buffer_read(can_message_t *dest, size_t max_count)
{
    if (dest == NULL || s_mutex == NULL || max_count == 0) {
        return 0;
    }

    size_t read_count = 0;

    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        while (read_count < max_count && s_count > 0) {
            memcpy(&dest[read_count], &s_buffer[s_tail], sizeof(can_message_t));
            s_tail = (s_tail + 1) % CAN_BUFFER_SIZE;
            s_count--;
            read_count++;
        }

        xSemaphoreGive(s_mutex);
    }

    return read_count;
}

void can_buffer_clear(void)
{
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_head = 0;
        s_tail = 0;
        s_count = 0;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Buffer cleared");
    }
}

void can_buffer_get_stats(uint32_t *overflow_count, uint32_t *total_count)
{
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        if (overflow_count != NULL) {
            *overflow_count = s_overflow_count;
        }
        if (total_count != NULL) {
            *total_count = s_total_count;
        }
        xSemaphoreGive(s_mutex);
    }
}

void can_buffer_reset_stats(void)
{
    if (s_mutex != NULL && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_overflow_count = 0;
        s_total_count = 0;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Buffer statistics reset");
    }
}

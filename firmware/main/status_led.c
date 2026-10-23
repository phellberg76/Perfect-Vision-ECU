/**
 * @file status_led.c
 * @brief Status LED control implementation
 */

#include "status_led.h"
#include "pin_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "status_led";

// Current LED state
static volatile status_led_state_t s_current_state = STATUS_LED_OFF;
static volatile status_led_state_t s_previous_state = STATUS_LED_OFF;

// LED hardware state
static volatile bool s_led_on = false;

// Timer for blinking
static TimerHandle_t s_blink_timer = NULL;

// Error timeout timer
static TimerHandle_t s_error_timer = NULL;

// Error state duration (ms)
#define ERROR_STATE_DURATION_MS 2000

// Flag to indicate pending error signal from ISR
static volatile bool s_error_pending = false;

// -----------------------------------------------------------------------------
// Internal functions
// -----------------------------------------------------------------------------

static void IRAM_ATTR set_led(bool on)
{
    s_led_on = on;
    gpio_set_level(STATUS_LED_GPIO, on ? 1 : 0);
}

static void blink_timer_callback(TimerHandle_t timer)
{
    // Check for pending error from ISR context
    if (s_error_pending) {
        s_error_pending = false;

        // Don't nest error states - save original state
        if (s_current_state != STATUS_LED_BLINK_10HZ) {
            s_previous_state = s_current_state;
        }
        s_current_state = STATUS_LED_BLINK_10HZ;

        // Switch to 10Hz blink (50ms period)
        xTimerChangePeriod(s_blink_timer, pdMS_TO_TICKS(50), 0);

        // Reset error timeout
        xTimerReset(s_error_timer, 0);
    }

    // Toggle LED
    set_led(!s_led_on);
}

static void error_timer_callback(TimerHandle_t timer)
{
    // Revert to previous state after error timeout
    ESP_LOGI(TAG, "Error timeout, reverting to previous state");

    s_current_state = s_previous_state;

    // Update blink timer period based on state
    switch (s_current_state) {
    case STATUS_LED_OFF:
        xTimerStop(s_blink_timer, 0);
        set_led(false);
        break;

    case STATUS_LED_SOLID:
        xTimerStop(s_blink_timer, 0);
        set_led(true);
        break;

    case STATUS_LED_BLINK_1HZ:
        xTimerChangePeriod(s_blink_timer, pdMS_TO_TICKS(500), 0);
        xTimerStart(s_blink_timer, 0);
        break;

    case STATUS_LED_BLINK_2HZ:
        xTimerChangePeriod(s_blink_timer, pdMS_TO_TICKS(250), 0);
        xTimerStart(s_blink_timer, 0);
        break;

    case STATUS_LED_BLINK_10HZ:
        // Stay in error state
        break;
    }
}

static void update_blink_timer(void)
{
    if (s_blink_timer == NULL) {
        return;
    }

    // Stop timer first
    xTimerStop(s_blink_timer, 0);

    switch (s_current_state) {
    case STATUS_LED_OFF:
        set_led(false);
        break;

    case STATUS_LED_SOLID:
        set_led(true);
        break;

    case STATUS_LED_BLINK_1HZ:
        // 1Hz = 500ms period (on 250ms, off 250ms)
        set_led(true);
        xTimerChangePeriod(s_blink_timer, pdMS_TO_TICKS(500), 0);
        xTimerStart(s_blink_timer, 0);
        break;

    case STATUS_LED_BLINK_2HZ:
        // 2Hz = 250ms period (on 125ms, off 125ms)
        set_led(true);
        xTimerChangePeriod(s_blink_timer, pdMS_TO_TICKS(250), 0);
        xTimerStart(s_blink_timer, 0);
        break;

    case STATUS_LED_BLINK_10HZ:
        // 10Hz = 100ms period (on 50ms, off 50ms)
        set_led(true);
        xTimerChangePeriod(s_blink_timer, pdMS_TO_TICKS(50), 0);
        xTimerStart(s_blink_timer, 0);
        break;
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

esp_err_t status_led_init(void)
{
    ESP_LOGI(TAG, "Initializing status LED on GPIO %d", STATUS_LED_GPIO);

    // Configure GPIO
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    esp_err_t ret = gpio_config(&led_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO: %s", esp_err_to_name(ret));
        return ret;
    }

    // Start with LED off
    set_led(false);

    // Create blink timer (auto-reload)
    s_blink_timer = xTimerCreate("led_blink", pdMS_TO_TICKS(500), pdTRUE,
                                  NULL, blink_timer_callback);
    if (s_blink_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create blink timer");
        return ESP_ERR_NO_MEM;
    }

    // Create error timeout timer (one-shot)
    s_error_timer = xTimerCreate("led_error", pdMS_TO_TICKS(ERROR_STATE_DURATION_MS),
                                  pdFALSE, NULL, error_timer_callback);
    if (s_error_timer == NULL) {
        ESP_LOGE(TAG, "Failed to create error timer");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Status LED initialized");
    return ESP_OK;
}

void status_led_set_state(status_led_state_t state)
{
    if (state == s_current_state) {
        return;
    }

    ESP_LOGI(TAG, "LED state: %d -> %d", s_current_state, state);
    s_current_state = state;
    update_blink_timer();
}

status_led_state_t status_led_get_state(void)
{
    return s_current_state;
}

void IRAM_ATTR status_led_signal_error(void)
{
    // Check if we're in ISR context
    if (xPortInIsrContext()) {
        // Set flag for timer callback to handle
        s_error_pending = true;

        // Ensure blink timer is running to process the flag
        BaseType_t xHigherPriorityTaskWoken = pdFALSE;
        if (s_blink_timer != NULL) {
            xTimerStartFromISR(s_blink_timer, &xHigherPriorityTaskWoken);
        }
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
        return;
    }

    // Task context - handle directly
    if (s_current_state != STATUS_LED_BLINK_10HZ) {
        s_previous_state = s_current_state;
    }

    // Switch to error blink
    status_led_set_state(STATUS_LED_BLINK_10HZ);

    // Reset/start error timeout timer
    if (s_error_timer != NULL) {
        xTimerReset(s_error_timer, 0);
    }
}

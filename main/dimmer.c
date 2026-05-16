#include "dimmer.h"

#include <inttypes.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dimmer";

#define ZERO_CROSS_GPIO           GPIO_NUM_1
#define TRIAC_GATE_GPIO           GPIO_NUM_2

#define TIMER_RESOLUTION_HZ       1000000U
#define DEFAULT_HALF_CYCLE_US     10000U
#define MIN_HALF_CYCLE_US         7000U
#define MAX_HALF_CYCLE_US         12000U
#define ZERO_CROSS_DEBOUNCE_US    4000U

#define ZERO_CROSS_TRIGGER_LEVEL  0
#define MIN_FIRE_DELAY_US         300U
#define END_GUARD_US              700U
#define GATE_PULSE_US             500U

#define DIAGNOSTIC_LOG_MS         1000U

typedef enum {
    GATE_STATE_IDLE = 0,
    GATE_STATE_WAIT_FIRE,
    GATE_STATE_WAIT_OFF,
} gate_state_t;

static gptimer_handle_t s_phase_timer;

static volatile uint32_t s_level;
static volatile uint32_t s_half_cycle_us = DEFAULT_HALF_CYCLE_US;
static volatile uint64_t s_last_zero_cross_count;
static volatile uint32_t s_zero_cross_count;
static volatile uint32_t s_zero_cross_rise_count;
static volatile uint32_t s_zero_cross_fall_count;
static volatile uint32_t s_gate_pulse_count;
static volatile gate_state_t s_gate_state = GATE_STATE_IDLE;

static uint32_t IRAM_ATTR level_to_delay_us(uint32_t level, uint32_t half_cycle_us)
{
    if (level >= DIMMER_MAX_LEVEL) {
        return MIN_FIRE_DELAY_US;
    }

    uint32_t latest_fire_us = half_cycle_us - END_GUARD_US - GATE_PULSE_US;
    if (latest_fire_us <= MIN_FIRE_DELAY_US) {
        return MIN_FIRE_DELAY_US;
    }

    uint32_t dimming_window_us = latest_fire_us - MIN_FIRE_DELAY_US;
    uint32_t active_window_us = (level * dimming_window_us) / DIMMER_MAX_LEVEL;

    return latest_fire_us - active_window_us;
}

static void IRAM_ATTR zero_cross_isr(void *arg)
{
    gptimer_handle_t timer = (gptimer_handle_t)arg;
    uint64_t now = 0;

    if (gptimer_get_raw_count(timer, &now) != ESP_OK) {
        return;
    }

    int level = gpio_get_level(ZERO_CROSS_GPIO);
    if (level == 0) {
        s_zero_cross_fall_count++;
    } else {
        s_zero_cross_rise_count++;
    }

    if (level != ZERO_CROSS_TRIGGER_LEVEL) {
        return;
    }

    uint64_t elapsed_us = now - s_last_zero_cross_count;
    if (elapsed_us < ZERO_CROSS_DEBOUNCE_US) {
        return;
    }

    if ((elapsed_us >= MIN_HALF_CYCLE_US) && (elapsed_us <= MAX_HALF_CYCLE_US)) {
        uint32_t measured_half_cycle_us = (uint32_t)elapsed_us;
        s_half_cycle_us = ((s_half_cycle_us * 7U) + measured_half_cycle_us) / 8U;
    }

    s_last_zero_cross_count = now;
    s_zero_cross_count++;

    gpio_set_level(TRIAC_GATE_GPIO, 0);

    uint32_t current_level = s_level;
    if (current_level == 0U) {
        s_gate_state = GATE_STATE_IDLE;
        (void)gptimer_set_alarm_action(timer, NULL);
        return;
    }

    if (current_level > DIMMER_MAX_LEVEL) {
        current_level = DIMMER_MAX_LEVEL;
    }

    gptimer_alarm_config_t alarm_config = {
        .alarm_count = now + level_to_delay_us(current_level, s_half_cycle_us),
    };

    s_gate_state = GATE_STATE_WAIT_FIRE;
    (void)gptimer_set_alarm_action(timer, &alarm_config);
}

static bool IRAM_ATTR phase_timer_alarm_cb(gptimer_handle_t timer,
                                           const gptimer_alarm_event_data_t *edata,
                                           void *user_ctx)
{
    (void)user_ctx;

    if (s_gate_state == GATE_STATE_WAIT_FIRE) {
        gpio_set_level(TRIAC_GATE_GPIO, 1);
        s_gate_pulse_count++;

        gptimer_alarm_config_t alarm_config = {
            .alarm_count = edata->count_value + GATE_PULSE_US,
        };

        s_gate_state = GATE_STATE_WAIT_OFF;
        (void)gptimer_set_alarm_action(timer, &alarm_config);
        return false;
    }

    gpio_set_level(TRIAC_GATE_GPIO, 0);
    s_gate_state = GATE_STATE_IDLE;
    (void)gptimer_set_alarm_action(timer, NULL);

    return false;
}

static esp_err_t configure_phase_timer(void)
{
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = TIMER_RESOLUTION_HZ,
    };
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&timer_config, &s_phase_timer), TAG, "create timer");

    gptimer_event_callbacks_t callbacks = {
        .on_alarm = phase_timer_alarm_cb,
    };
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s_phase_timer, &callbacks, NULL),
                        TAG, "register timer callback");
    ESP_RETURN_ON_ERROR(gptimer_enable(s_phase_timer), TAG, "enable timer");
    ESP_RETURN_ON_ERROR(gptimer_start(s_phase_timer), TAG, "start timer");

    return ESP_OK;
}

static esp_err_t configure_io(void)
{
    gpio_config_t gate_config = {
        .pin_bit_mask = BIT64(TRIAC_GATE_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gate_config), TAG, "configure gate gpio");
    ESP_RETURN_ON_ERROR(gpio_set_level(TRIAC_GATE_GPIO, 0), TAG, "set gate low");

    gpio_config_t zero_cross_config = {
        .pin_bit_mask = BIT64(ZERO_CROSS_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&zero_cross_config), TAG, "configure zero-cross gpio");

    ESP_RETURN_ON_ERROR(gpio_install_isr_service(ESP_INTR_FLAG_IRAM), TAG, "install gpio isr service");
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(ZERO_CROSS_GPIO, zero_cross_isr, s_phase_timer),
                        TAG, "add zero-cross isr");

    return ESP_OK;
}

esp_err_t dimmer_init(uint32_t initial_level)
{
    ESP_LOGI(TAG, "LTV_OUT GPIO=%d, MOC GPIO=%d", ZERO_CROSS_GPIO, TRIAC_GATE_GPIO);

    ESP_RETURN_ON_ERROR(configure_phase_timer(), TAG, "configure phase timer");
    ESP_RETURN_ON_ERROR(configure_io(), TAG, "configure dimmer io");

    dimmer_set_level(initial_level);
    return ESP_OK;
}

void dimmer_set_level(uint32_t level)
{
    if (level > DIMMER_MAX_LEVEL) {
        level = DIMMER_MAX_LEVEL;
    }

    s_level = level;

    if ((level == 0U) && (s_phase_timer != NULL)) {
        gpio_set_level(TRIAC_GATE_GPIO, 0);
        s_gate_state = GATE_STATE_IDLE;
        (void)gptimer_set_alarm_action(s_phase_timer, NULL);
    }
}

uint32_t dimmer_get_level(void)
{
    return s_level;
}

void dimmer_poll(void)
{
    static TickType_t last_log_tick;
    static uint32_t last_zc_count;
    static uint32_t last_gate_count;
    static bool initialized;

    TickType_t now_tick = xTaskGetTickCount();
    if (!initialized) {
        last_log_tick = now_tick;
        initialized = true;
        return;
    }

    if ((now_tick - last_log_tick) < pdMS_TO_TICKS(DIAGNOSTIC_LOG_MS)) {
        return;
    }

    uint32_t zc_count = s_zero_cross_count;
    uint32_t gate_count = s_gate_pulse_count;
    uint32_t zc_delta = zc_count - last_zc_count;
    uint32_t gate_delta = gate_count - last_gate_count;

    last_zc_count = zc_count;
    last_gate_count = gate_count;
    last_log_tick = now_tick;

    ESP_LOGI(TAG,
             "level=%" PRIu32 "/%u gpio_level=%d half=%" PRIu32 "us "
             "zc=%" PRIu32 " (+%" PRIu32 ") rise=%" PRIu32
             " fall=%" PRIu32 " gate=%" PRIu32 " (+%" PRIu32 ")",
             s_level,
             DIMMER_MAX_LEVEL,
             gpio_get_level(ZERO_CROSS_GPIO),
             s_half_cycle_us,
             zc_count,
             zc_delta,
             s_zero_cross_rise_count,
             s_zero_cross_fall_count,
             gate_count,
             gate_delta);

    if (zc_delta == 0) {
        ESP_LOGW(TAG, "No accepted zero-cross edges on GPIO%d in the last %u ms",
                 ZERO_CROSS_GPIO, DIAGNOSTIC_LOG_MS);
    }
}

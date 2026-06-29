#include "render_clock.h"

#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "render_clock";

static gptimer_handle_t s_timer = NULL;
static TaskHandle_t s_render_task = NULL;

// GPTimer alarm ISR. Runs on the core that called gptimer_enable() (i.e. the
// render task's core), so the notify + wake stay core-local with no cross-core
// latency. Kept in IRAM and minimal: just a counting task notification.
static bool IRAM_ATTR render_clock_on_alarm(gptimer_handle_t timer,
                                            const gptimer_alarm_event_data_t *edata,
                                            void *user_ctx)
{
    (void)timer;
    (void)edata;
    (void)user_ctx;
    BaseType_t higher_prio_woken = pdFALSE;
    // Counting give: each missed/queued tick increments the notification value,
    // so render_clock_wait() can report a backlog (>1) as an overrun signal.
    vTaskNotifyGiveFromISR(s_render_task, &higher_prio_woken);
    return higher_prio_woken == pdTRUE;  // request context switch if needed
}

esp_err_t render_clock_start(uint32_t period_ticks)
{
    if (s_timer != NULL) {
        return ESP_OK;  // already started
    }

    s_render_task = xTaskGetCurrentTaskHandle();

    const gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 3 * 1000 * 1000,  // 3 MHz => 1 tick ≈ 0.333 µs
        .intr_priority = 0,                // auto-select
    };
    esp_err_t err = gptimer_new_timer(&timer_config, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_new_timer failed: %s", esp_err_to_name(err));
        s_timer = NULL;
        return err;
    }

    const gptimer_event_callbacks_t cbs = {
        .on_alarm = render_clock_on_alarm,
    };
    err = gptimer_register_event_callbacks(s_timer, &cbs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_register_event_callbacks failed: %s", esp_err_to_name(err));
        goto fail;
    }

    // Enabling from the render task's context registers the ISR on this core.
    err = gptimer_enable(s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_enable failed: %s", esp_err_to_name(err));
        goto fail;
    }

    const gptimer_alarm_config_t alarm_config = {
        .alarm_count = period_ticks,     // fire every period_ticks (counter at 3 MHz)
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    err = gptimer_set_alarm_action(s_timer, &alarm_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_set_alarm_action failed: %s", esp_err_to_name(err));
        gptimer_disable(s_timer);
        goto fail;
    }

    err = gptimer_start(s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_start failed: %s", esp_err_to_name(err));
        gptimer_disable(s_timer);
        goto fail;
    }

    ESP_LOGI(TAG, "render master clock started: %u ticks period (3 MHz) on core %d",
             (unsigned)period_ticks, xPortGetCoreID());
    return ESP_OK;

fail:
    gptimer_del_timer(s_timer);
    s_timer = NULL;
    return err;
}

uint32_t render_clock_wait(void)
{
    // Block until >=1 tick, return the accumulated count and clear it to 0.
    // The caller renders exactly ONE block regardless of the returned value.
    return ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void render_clock_stop(void)
{
    if (s_timer == NULL) {
        return;
    }
    gptimer_stop(s_timer);
    gptimer_disable(s_timer);
    gptimer_del_timer(s_timer);
    s_timer = NULL;
    s_render_task = NULL;
}

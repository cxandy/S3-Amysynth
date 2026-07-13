#include "sdkconfig.h"

// This file is the default GPTimer-backed render clock. The alternate,
// opt-in I2S-backed implementation of the same render_clock.h API lives in
// render_clock_i2s.c, active only when CONFIG_RENDER_CLOCK_I2S_ENABLE=y.
// Exactly one of the two files defines these symbols for a given build.
#if !CONFIG_RENDER_CLOCK_I2S_ENABLE

#include "render_clock.h"

#include <inttypes.h>

#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "render_clock";

// Requested counter resolution. The GPTimer prescaler is an INTEGER divider of
// the source clock (see gptimer_select_periph_clock(): prescale = src / res,
// truncated), so only resolutions that divide the source exactly are granted as
// asked. On the S3 the default source is APB @ 80 MHz and the minimum prescale
// is 2, so 40 MHz is both the highest achievable resolution and an exact one
// (80 / 2). It is deliberately NOT 3 MHz: 80/3 truncates to a prescale of 26,
// silently granting 3,076,923 Hz - 2.56% fast - which paced every render block
// (and therefore the whole sequencer clock) 2.56% early.
//
// A perfectly exact block period is impossible here regardless: it would need a
// resolution that is an integer multiple of 187.5 Hz, and no integer divisor of
// 80 MHz yields one. At 40 MHz the rounded period is off by ~1.6 ppm, far below
// the S3-vs-host crystal tolerance that the USB ring already has to cope with.
#define RENDER_CLOCK_RESOLUTION_HZ (40 * 1000 * 1000)

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

esp_err_t render_clock_start(uint32_t block_frames, uint32_t sample_rate_hz)
{
    if (s_timer != NULL) {
        return ESP_OK;  // already started
    }

    if (block_frames == 0 || sample_rate_hz == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    s_render_task = xTaskGetCurrentTaskHandle();

    const gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = RENDER_CLOCK_RESOLUTION_HZ,
        .intr_priority = 0,                // auto-select
    };
    esp_err_t err = gptimer_new_timer(&timer_config, &s_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_new_timer failed: %s", esp_err_to_name(err));
        s_timer = NULL;
        return err;
    }

    // Derive the alarm period from the resolution the driver actually GRANTED,
    // never from the one requested above. They match on the S3 today, but a
    // different clock source, a different SoC (source frequency and minimum
    // divider both change), or DFS on a PM-enabled build can each make them
    // diverge, and the driver reports that only as a log warning.
    uint32_t granted_resolution_hz = 0;
    err = gptimer_get_resolution(s_timer, &granted_resolution_hz);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gptimer_get_resolution failed: %s", esp_err_to_name(err));
        goto fail;
    }
    if (granted_resolution_hz != (uint32_t)RENDER_CLOCK_RESOLUTION_HZ) {
        ESP_LOGW(TAG, "requested %d Hz, granted %" PRIu32 " Hz - deriving the block "
                 "period from the granted rate",
                 (int)RENDER_CLOCK_RESOLUTION_HZ, granted_resolution_hz);
    }

    // Round rather than truncate: a half-tick bias is free to avoid, and at low
    // granted resolutions truncation is the difference between a few ppm and a
    // few hundred.
    const uint32_t period_ticks =
        (uint32_t)((((uint64_t)block_frames * granted_resolution_hz) + (sample_rate_hz / 2))
                   / (uint64_t)sample_rate_hz);
    // alarm_count 0 with auto-reload would re-arm instantly and livelock the
    // core in the alarm ISR. Only reachable via an absurd resolution/rate ratio,
    // but period_ticks is derived from queried state now, so it gets a guard.
    if (period_ticks == 0) {
        ESP_LOGE(TAG, "period_ticks == 0 (resolution %" PRIu32 " Hz too low for %" PRIu32
                 " frames @ %" PRIu32 " Hz)", granted_resolution_hz, block_frames, sample_rate_hz);
        err = ESP_ERR_INVALID_STATE;
        goto fail;
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
        .alarm_count = period_ticks,     // counter runs at granted_resolution_hz
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

    ESP_LOGI(TAG, "render master clock started: %" PRIu32 " ticks @ %" PRIu32 " Hz "
             "(%" PRIu32 " ns block period) on core %d",
             period_ticks, granted_resolution_hz,
             (uint32_t)(((uint64_t)period_ticks * 1000000000ULL) / granted_resolution_hz),
             xPortGetCoreID());
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

#endif  // !CONFIG_RENDER_CLOCK_I2S_ENABLE

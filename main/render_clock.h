#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * render_clock — the master clock for the audio render loop.
 *
 * Emits exactly one "render now" signal per audio block period (16000 ticks at
 * 3 MHz = 5333.333 µs for 256 samples @ 48 kHz), independent of the FreeRTOS
 * tick rate. Phase 1 is backed by a GPTimer whose alarm ISR is pinned to the
 * render task's core and wakes it via a FreeRTOS task notification.
 *
 * Phase 2 (future I2S): delete the GPTimer and let the blocking
 * i2s_channel_write(..., portMAX_DELAY) be the clock (DMA backpressure paces
 * render, exactly like the shorepine/amy_dual_core_esp32 reference). The render
 * task body stays the same; only this seam changes.
 *
 * STRICT 1:1: the caller renders exactly ONE block per render_clock_wait()
 * return, never the backlog, so AMY's total_blocks stays locked to realtime and
 * the sequencer tempo (derived from amy_sysclock()) cannot drift.
 */

/**
 * @brief Create and start the render master clock.
 *
 * MUST be called from the render task itself (the GPTimer alarm ISR is
 * registered on the core that enables the timer, and the notification target is
 * the calling task), so the ISR and the woken task share a core.
 *
 * @param period_ticks  Audio block period in GPTimer ticks (16000 at 3 MHz =
 *                      5333.333 µs for 256 samples @ 48 kHz).
 * @return ESP_OK on success.
 */
esp_err_t render_clock_start(uint32_t period_ticks);

/**
 * @brief Block until the next render tick.
 *
 * @return Accumulated tick count since the last wait. Normally 1. A value >1
 *         means a tick elapsed while the previous block was still rendering
 *         (an overrun): the caller should still render exactly ONE block and
 *         treat the excess purely as a diagnostic signal.
 */
uint32_t render_clock_wait(void);

/**
 * @brief Stop and delete the render master clock (not used in normal run).
 */
void render_clock_stop(void);

#ifdef __cplusplus
}
#endif

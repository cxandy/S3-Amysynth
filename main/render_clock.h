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
 * Emits exactly one "render now" signal per audio block period (256 samples
 * @ 48 kHz = 5333.333 µs), independent of the FreeRTOS tick rate. Two backends
 * implement this header, selected by CONFIG_RENDER_CLOCK_I2S_ENABLE (default
 * OFF):
 *
 *  - render_clock.c (default): GPTimer alarm ISR on the render task's core,
 *    waking it via a FreeRTOS task notification.
 *
 *  - render_clock_i2s.c (opt-in): I2S TX channel with no GPIO assigned;
 *    render_clock_wait() blocks on i2s_channel_write() and DMA backpressure
 *    paces render. Its DMA queue depth (CONFIG_RENDER_CLOCK_I2S_DMA_DESC_NUM)
 *    gives look-ahead slack the GPTimer path lacks. The render task body is
 *    unchanged either way; only this seam differs.
 *
 * STRICT 1:1: the caller renders exactly ONE block per render_clock_wait()
 * return, never the backlog, so AMY's total_blocks stays locked to realtime and
 * the sequencer tempo (from amy_sysclock()) cannot drift. Both backends.
 */

/**
 * @brief Create and start the render master clock.
 *
 * MUST be called from the render task itself: both backends register their ISR
 * on the calling core, and the notification target is the calling task, so ISR
 * and woken task share a core.
 *
 * @param block_frames    Audio block size in sample frames (AMY_BLOCK_SIZE).
 * @param sample_rate_hz  Audio sample rate in Hz (AMY_SAMPLE_RATE). Each
 *                        backend derives its own hardware period from these.
 * @return ESP_OK on success.
 */
esp_err_t render_clock_start(uint32_t block_frames, uint32_t sample_rate_hz);

/**
 * @brief Block until the next render tick.
 *
 * @return GPTimer backend: accumulated ticks since the last wait, normally 1.
 *         >1 means a tick elapsed mid-render (overrun): still render exactly
 *         ONE block, treat the excess as diagnostic only.
 *         I2S backend: always 1 - i2s_channel_write() has no counting
 *         analogue, so it cannot report overrun this way.
 */
uint32_t render_clock_wait(void);

/**
 * @brief Stop and delete the render master clock (not used in normal run).
 */
void render_clock_stop(void);

#ifdef __cplusplus
}
#endif

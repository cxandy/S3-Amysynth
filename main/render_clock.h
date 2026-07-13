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
 * implement this same header, selected at build time by
 * CONFIG_RENDER_CLOCK_I2S_ENABLE (default OFF):
 *
 *  - render_clock.c (default): a GPTimer whose alarm ISR is pinned to the
 *    render task's core and wakes it via a FreeRTOS task notification.
 *
 *  - render_clock_i2s.c (opt-in, CONFIG_RENDER_CLOCK_I2S_ENABLE=y): an I2S TX
 *    channel with every GPIO left unassigned (no pin mux change, no physical
 *    signal). render_clock_wait() blocks on i2s_channel_write(...,
 *    portMAX_DELAY); DMA backpressure paces render, exactly like the
 *    shorepine/amy_dual_core_esp32 reference. Its DMA queue depth
 *    (CONFIG_RENDER_CLOCK_I2S_DMA_DESC_NUM) gives the render task look-ahead
 *    slack the GPTimer path does not have -- see docs/handoff/i2s-clock-source.md
 *    for the full design + impact trace. The render task body in main.c is
 *    unchanged either way; only this seam changes.
 *
 * STRICT 1:1: the caller renders exactly ONE block per render_clock_wait()
 * return, never the backlog, so AMY's total_blocks stays locked to realtime and
 * the sequencer tempo (derived from amy_sysclock()) cannot drift. This holds
 * for both backends.
 */

/**
 * @brief Create and start the render master clock.
 *
 * MUST be called from the render task itself: the GPTimer backend's alarm
 * ISR is registered on the core that enables the timer (the calling task's
 * core), and the notification target is the calling task, so the ISR and the
 * woken task share a core; the I2S backend's DMA-complete ISR is likewise
 * registered on the calling core.
 *
 * @param block_frames    Audio block size in sample frames (AMY_BLOCK_SIZE).
 * @param sample_rate_hz  Audio sample rate in Hz (AMY_SAMPLE_RATE). Each
 *                        backend derives its own hardware-specific period
 *                        from these two values (e.g. the GPTimer backend
 *                        converts them into a tick count against the counter
 *                        resolution the timer driver actually granted).
 * @return ESP_OK on success.
 */
esp_err_t render_clock_start(uint32_t block_frames, uint32_t sample_rate_hz);

/**
 * @brief Block until the next render tick.
 *
 * @return GPTimer backend: accumulated tick count since the last wait.
 *         Normally 1. A value >1 means a tick elapsed while the previous
 *         block was still rendering (an overrun): the caller should still
 *         render exactly ONE block and treat the excess purely as a
 *         diagnostic signal.
 *         I2S backend: always 1 -- i2s_channel_write() has no counting
 *         analogue to the GPTimer's counting task notification, so this
 *         backend cannot report an overrun count this way (see
 *         docs/handoff/i2s-clock-source.md).
 */
uint32_t render_clock_wait(void);

/**
 * @brief Stop and delete the render master clock (not used in normal run).
 */
void render_clock_stop(void);

#ifdef __cplusplus
}
#endif

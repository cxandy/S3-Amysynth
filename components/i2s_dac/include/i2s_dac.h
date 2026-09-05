#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * i2s_dac — PCM5102 standalone audio output over I2S.
 *
 * Drives the I2S TX channel that feeds the board's PCM5102 DAC (the
 * BCK / LRCK / DIN lines). The render task writes each stereo block to it
 * after amy_update(); DMA backpressure paces nothing here — the GPTimer
 * render clock owns the cadence and i2s_dac_write() just keeps the DAC fed
 * (a full DMA queue drops the block rather than stalling render).
 *
 * Pins (hardware "DAC PCM5102": BCK=IO5, LRCK=IO7, DIN=IO6) and DMA depth
 * come from Kconfig.
 */

esp_err_t i2s_dac_init(uint32_t sample_rate_hz, size_t frames_per_block);

esp_err_t i2s_dac_write_stereo(const int16_t *frames, size_t frame_count);

bool i2s_dac_ready(void);

#ifdef __cplusplus
}
#endif
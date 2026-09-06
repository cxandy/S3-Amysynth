#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Three analog performance controls (filter cutoff / master volume / FX)
 * sampled from the ESP32-S3 ADC1. Wiring comes from the "Performance Pots"
 * Kconfig menu; each role's GPIO defaults to -1 (disabled).
 *
 * A low-priority task samples the enabled pins and pushes each value into the
 * synth only when it moves past a small deadband. The main sample loop or the
 * audio/UI tasks never blocks on the pots. With the wireless synth compiled
 * in, the FX knob is a CC1 mod wheel for the live-play slot (osc-0 vibrato). */

esp_err_t perf_pots_init(void);

esp_err_t perf_pots_deinit(void);

#ifdef __cplusplus
}
#endif
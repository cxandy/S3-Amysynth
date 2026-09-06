#pragma once
#include "esp_err.h"
#include "amy.h"   /* amy_config_t, AMY_MIDI_IS_UART */

#ifdef __cplusplus
extern "C" {
#endif

/* The UART is AMY's: this sets the amy_cfg MIDI fields for the DIN port
 * (in) and the clock-out stream (out) from the User Hardware Kconfig.
 * Must run before amy_start(); a pin -1 leaves AMY's UART MIDI off. */
void midi_din_prepare(amy_config_t *cfg);

/* DIN-5 MIDI input: reads a spare UART RX at 31250 8-N-1 in a low-priority
 * task and feeds the shared midi_core parser, so inbound notes land on the
 * same live-play voice as BLE MIDI (single sink, wired by the app).
 * No-op when CONFIG_AMYSYNTH_MIDI_IN_GPIO is -1. */
esp_err_t midi_din_init(void);

#ifdef __cplusplus
}
#endif

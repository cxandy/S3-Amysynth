#pragma once
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DIN-5 MIDI input: reads a spare UART RX at 31250 8-N-1 in a low-priority
 * task and feeds the shared midi_core parser, so inbound notes land on the
 * same live-play voice as BLE MIDI (single sink, wired by the app).
 * No-op when CONFIG_AMYSYNTH_MIDI_IN_GPIO is -1. */
esp_err_t midi_din_init(void);

#ifdef __cplusplus
}
#endif
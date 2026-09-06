#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MIDI clock slave: locks the sequencer's tempo to an external master's
 * MIDI timing clock (0xF8) and follows its song start/stop (0xFA/0xFC).
 *
 * Wired into midi_core's sink (main.c), so whatever transport is live - the
 * DIN port or BLE MIDI - feeds the same hook. No-op when the Kconfig switch
 * is off; the callbacks are cheap (timer read + float EMA) because they run
 * inside the parser's spinlock on the transport task; the heavyweight
 * sequencer_core_set_bpm/set_playing work is flushed by this component's own
 * low-priority task a few times a second. */

/* One timing-clock tick (0xF8). */
void midi_clock_slave_tick(void);

/* External song start/continue (0xFA / 0xFB) and stop (0xFC). Only honoured
 * while a live clock stream has actually been seen. */
void midi_clock_slave_start(void);
void midi_clock_slave_stop(void);

/* Create the flushing task. Safe to call once at startup; no-op when the
 * feature is disabled. */
esp_err_t midi_clock_slave_init(void);

#ifdef __cplusplus
}
#endif
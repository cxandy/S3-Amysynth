#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Fixed starter FM/ALGO presets (patches 267-270) ─────────────────────────
 * Four fixed-parameter voices on fm_voice_configure_track(), all algorithm 0:
 * two independent chains, indices 0-3 a serial modulator stack into a carrier
 * at index 3, indices 4-5 a modulator->carrier pair (algorithms.c). Presets
 * vary only ratio/level/feedback; unused operators are silenced with level=0
 * rather than omitted, since AMY's table always wires all 6 slots.
 *
 * Starting points, not final voicings - the exact ratios/levels are a
 * best-effort approximation pending on-hardware listening. */
void fm_preset_configure_track(uint8_t synth_id, uint16_t patch,
                               uint16_t num_voices);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Fixed starter FM/ALGO presets (patches 267-270) ─────────────────────────
 * Four fixed-parameter voices built on fm_voice_configure_track(), all using
 * algorithm 0 (two independent chains: operator-array indices 0-3 form a
 * 4-deep serial modulator stack feeding a carrier at index 3, indices 4-5
 * form a simple modulator->carrier pair — see algorithms.c and the worked
 * trace in docs/handoff/a2-fm-dx7.md). Presets vary only ratio/level/feedback
 * per operator; unused operators are silenced via level=0 rather than
 * omitted, since AMY's algorithm table always wires all 6 operator slots.
 *
 * These are starting points, not final voicings: without on-hardware
 * listening the exact ratios/levels are a best-effort FM sound-design
 * approximation (see docs/handoff/a2-fm-dx7.md for what's tunable). */
void fm_preset_configure_track(uint8_t synth_id, uint16_t patch,
                               uint16_t num_voices);

#ifdef __cplusplus
}
#endif

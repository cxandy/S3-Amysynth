#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Multi-oscillator bass presets (patches 264-266) ─────────────────────────
 * Three two-osc bass presets selectable as melodic layer patches; both oscs
 * chain via chained_osc for MIDI note propagation. The caller's
 * sequencer_configure_melodic_envelope() intentionally overwrites the osc-0
 * envelope - the bass character comes from the oscillator structure and
 * filter, not the ADSR.
 *
 * 264: Classic Sub-Heavy Detune - PULSE + detuned SAW_DOWN, LPF24, env sweep
 * 265: Solid Sine-Reinforced    - SINE sub + SAW_DOWN, high-Q LPF24 at 80 Hz
 * 266: FM DX7-Style             - SINE carrier + sub-octave SINE, DX7 envs
 *
 * Attack floor: every eg0_times[0] here is >= 2 ms. */
void bass_preset_configure_track(uint8_t synth_id, uint16_t patch,
                                 uint16_t num_voices);

#ifdef __cplusplus
}
#endif

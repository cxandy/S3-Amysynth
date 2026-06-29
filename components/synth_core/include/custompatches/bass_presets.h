#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Multi-oscillator bass presets (patches 264–266) ─────────────────────────
 * Three two-osc bass presets selectable as melodic layer patches.
 * Each uses oscs_per_voice=2; both oscs chain via chained_osc for MIDI note
 * propagation. Osc-0 envelope is overwritten by the caller's
 * sequencer_configure_melodic_envelope() — that is intentional; the bass
 * character comes from the oscillator structure and filter, not the ADSR.
 *
 * 264: Classic Sub-Heavy Detune  — PULSE + detuned SAW_DOWN, LPF24, env sweep
 * 265: Solid Sine-Reinforced     — SINE sub + SAW_DOWN, high-Q LPF24 at 80 Hz
 * 266: FM DX7-Style              — SINE carrier + sub-octave SINE, DX7 envs
 *
 * Attack floor: all eg0_times[0] in this file are >= 2 ms. */
void bass_preset_configure_track(uint8_t synth_id, uint16_t patch,
                                 uint16_t num_voices);

#ifdef __cplusplus
}
#endif

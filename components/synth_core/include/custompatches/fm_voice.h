#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Generic 6-operator DX7-style FM/ALGO voice engine ───────────────────────
 * Wraps AMY's ALGO wave type (algorithms.c's 33 fixed 6-operator routing
 * tables) as one melodic voice: an ALGO control osc (osc 0) plus six SINE
 * operators (osc 1..6), so oscs_per_voice is always 7. Every AMY algorithm
 * wires all 6 operator slots, so this fixed layout needs no per-algorithm
 * special-casing.
 *
 * Index mapping: op_ratio[i]/op_level[i] <-> algo_source[i] <-> osc (i + 1).
 * algorithms.c authors slot 0 as DX7 "operator 6" and slot 5 as "operator 1";
 * this engine does not renumber, so reading a real DX7 chart means slot 5 =
 * OP1, slot 4 = OP2, etc. The UI labels rows by array index (OP 0..5).
 *
 * Per-operator "level" is that operator's amp_coefs[COEF_CONST]: for a
 * modulator it IS the FM modulation index/brightness, for a final carrier it
 * multiplies the shared voice envelope on osc 0. Level 0 silences the operator
 * (and degrades whatever a modulator fed into an unmodulated tone), like a DX7
 * output-level knob - the mechanism presets use to run fewer than 6 audible
 * partials without per-algorithm routing knowledge.
 *
 * All 6 operators share ONE amplitude envelope (osc 0's eg0, driven by the
 * row's per-track ADSR editor) plus a fixed short default baked in at
 * configure time. Real DX7 patches give every operator its own envelope; that
 * is out of scope here. */

#define FM_NUM_OPS 6

typedef struct {
    uint8_t algorithm;              /* 0..32 (AMY algorithms[] index)        */
    float   op_ratio[FM_NUM_OPS];   /* per-operator frequency ratio          */
    float   op_level[FM_NUM_OPS];   /* per-operator output level, 0..1       */
    float   feedback;               /* 0..~1.2; only audible if the op with
                                      * FB_IN in the chosen algorithm has a
                                      * nonzero level */
} fm_voice_t;

/* The single live-editable "custom" FM voice (SEQ_PATCH_FM_CUSTOM), owned by
 * this module. The FM UI screen mutates it directly (the s_fx / amy_fx.h
 * convention) then calls sequencer_core_fm_voice_changed() to push to AMY. */
extern fm_voice_t s_fm_voice;

/* Fill *v with a safe, audible default (2-op chain: op index 4 modulates
 * index 5, algorithm 0; the rest silenced). Used to initialise s_fm_voice and
 * by fm_presets.c as a shared starting point. */
void fm_voice_default(fm_voice_t *v);

/* Full (re)configure of one synth slot as a 7-osc FM/ALGO voice. Call after
 * allocating/reallocating the synth (patch switch, layer creation). */
void fm_voice_configure_track(uint8_t synth_id, uint16_t num_voices,
                              const fm_voice_t *voice);

/* Live update of an already-configured FM voice without reallocating the osc
 * pool. Cheap enough for every encoder tick from the FM UI screen. */
void fm_voice_push_live(uint8_t synth_id, const fm_voice_t *voice);

#ifdef __cplusplus
}
#endif

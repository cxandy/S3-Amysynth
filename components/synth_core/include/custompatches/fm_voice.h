#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Generic 6-operator DX7-style FM/ALGO voice engine ───────────────────────
 * Wraps AMY's ALGO wave type (amy.h ALGORITHM/algo_source[], algorithms.c's
 * 33 fixed 6-operator routing tables) as a single melodic "voice": one AMY
 * ALGO control osc (osc 0, relative) plus six SINE operator oscs (osc 1..6,
 * relative), so oscs_per_voice is always 7 regardless of which algorithm is
 * selected — every one of AMY's 33 algorithms wires all 6 operator slots, so
 * this fixed layout is correct-by-construction for any algorithm index; no
 * per-algorithm special-casing is needed here.
 *
 * Array index -> AMY algo_source[] slot -> relative osc:
 *   op_ratio[i]/op_level[i]  <->  algo_source[i]  <->  osc (i + 1)
 * algorithms.c authors slot 0 as DX7 "operator 6" and slot 5 as "operator 1"
 * (transcribed from the MFSA DX7 tables); this engine does not renumber that,
 * so cross-referencing a real DX7 algorithm chart means reading slot 5 as
 * OP1, slot 4 as OP2, etc. The UI screen labels rows by array index (OP 0..5)
 * to keep the mapping obvious in code; relabeling to DX7 numbering is a
 * cosmetic follow-up (see docs/handoff).
 *
 * Per-operator "level" is that operator's own amp_coefs[COEF_CONST] — for an
 * operator that writes to a modulation bus this is literally the FM
 * modulation index/brightness; for an operator that is a final carrier
 * (writes directly to the voice output) it multiplies the shared voice
 * envelope on osc 0. Setting an operator's level to 0 silences it (and, for
 * a modulator, degrades whatever it fed into a plain unmodulated tone) —
 * exactly like a real DX7's operator output level knob, and the mechanism
 * this engine relies on to let presets use fewer than 6 audible partials
 * without needing per-algorithm routing knowledge.
 *
 * All 6 operators share ONE amplitude envelope (osc 0's own eg0, driven by
 * the row's existing per-track ADSR editor — see seq_core_editors.c) plus a
 * fixed short default envelope baked in at configure time for the operators
 * themselves. Genuine DX7 patches give every operator its own independent
 * envelope; that is out of scope for this vertical slice (see docs/handoff
 * for the deferred work list). */

#define FM_NUM_OPS 6

typedef struct {
    uint8_t algorithm;              /* 0..32 (AMY algorithms[] index)        */
    float   op_ratio[FM_NUM_OPS];   /* per-operator frequency ratio          */
    float   op_level[FM_NUM_OPS];   /* per-operator output level, 0..1       */
    float   feedback;               /* 0..~1.2; only audible if the op with
                                      * FB_IN in the chosen algorithm has a
                                      * nonzero level */
} fm_voice_t;

/* The single live-editable "custom" FM voice (SEQ_PATCH_FM_CUSTOM). Owned by
 * this module; the FM UI screen (ui_screen_fm.c) mutates it directly (mirrors
 * the s_fx / amy_fx.h convention already used for the global FX cache), then
 * calls sequencer_core_fm_voice_changed() to push the edit to AMY. */
extern fm_voice_t s_fm_voice;

/* Fill *v with a safe, audible default (2-op chain: op index 4 modulates
 * index 5, algorithm 0; the rest silenced). Used to initialise s_fm_voice and
 * by fm_presets.c as a shared starting point. */
void fm_voice_default(fm_voice_t *v);

/* Full (re)configure of one synth slot as a 7-osc FM/ALGO voice: allocates
 * oscs_per_voice=7, wires osc 0 (ALGO, algorithm + algo_source[0..5] + shared
 * envelope) and oscs 1..6 (SINE operators, ratio + level + a short default
 * envelope). Call after allocating/reallocating the synth (e.g. on patch
 * switch or layer creation). */
void fm_voice_configure_track(uint8_t synth_id, uint16_t num_voices,
                              const fm_voice_t *voice);

/* Live update of an already-configured FM voice: pushes algorithm/feedback to
 * osc 0 and ratio/level to oscs 1..6 without reallocating oscs_per_voice.
 * Cheap enough to call on every encoder tick from the FM UI screen. */
void fm_voice_push_live(uint8_t synth_id, const fm_voice_t *voice);

#ifdef __cplusplus
}
#endif

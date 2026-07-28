#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Additive / partials voice engine (AMY BYO_PARTIALS) ─────────────────────
 * Wraps AMY's build-your-own partials wave type as one melodic voice: a
 * BYO_PARTIALS control osc (osc 0) plus N PARTIAL sine oscs (osc 1..N), so
 * oscs_per_voice is N+1. There is no algo_source[] routing: partials_note_on()
 * derives the child set from the parent's `preset` (= N) and osc adjacency
 * (child i at parent+1+i), and render_partials() drives each child's per-block
 * amplitude from the parent's post-envelope amp via COEF_VEL. So the parent
 * carries the shared amplitude envelope (the per-track ADSR editor targets
 * osc 0, as with the FM control osc) and the children carry the spectrum.
 *
 * Pitch: AMY's logfreq is in octaves, so a partial at ratio r gets
 * freq_coefs[COEF_CONST] = log2f(r) on top of COEF_NOTE tracking - inharmonic
 * (bell) ratios are as expressible as harmonics.
 *
 * Per-partial `level` is that partial's amp_coefs[COEF_CONST]; 0 silences it,
 * which is how presets use fewer than ADD_MAX_PARTIALS without changing the
 * osc layout. Optional `decay_ms` adds a local decay-to-zero EG0 so upper
 * partials ring down faster than the parent envelope (the bell preset relies
 * on this); 0 follows the parent alone. */

/* Hard ceiling on the partial count: a voice costs ADD_MAX_PARTIALS + 1 oscs
 * from the global pool, so 13 oscs/voice worst case keeps a 4-row x 4-voice
 * additive layer at 208 oscs, under budget. Compile-time fixed, like
 * FM_NUM_OPS. */
#define ADD_MAX_PARTIALS 12

typedef struct {
    uint8_t num_partials;               /* 1..ADD_MAX_PARTIALS (N)              */
    float   ratio[ADD_MAX_PARTIALS];    /* freq ratio of each partial (1,2,3..) */
    float   level[ADD_MAX_PARTIALS];    /* 0..1 relative level of each partial  */
    float   decay_ms[ADD_MAX_PARTIALS]; /* per-partial decay; 0 = follow parent */
} additive_voice_t;

/* The single live-editable "custom" additive voice
 * (SEQ_PATCH_ADDITIVE_CUSTOM), owned by this module. A UI screen mutates it
 * directly (the s_fm_voice / s_fx convention) then calls
 * sequencer_core_additive_voice_changed(). No such screen exists yet, so it
 * plays the drawbar-organ default. */
extern additive_voice_t s_additive_voice;

/* Drawbar-organ default: first 8 harmonics at 1/n roll-off, all following the
 * parent envelope. Shared starting point for s_additive_voice and presets. */
void additive_voice_default(additive_voice_t *v);

/* Full (re)configure of one synth slot as an (N+1)-osc additive voice. Call
 * after allocating/reallocating the synth (patch switch, layer creation). */
void additive_voice_configure_track(uint8_t synth_id, uint16_t num_voices,
                                    const additive_voice_t *voice);

/* Live update of an already-configured additive voice (ratio/level only),
 * cheap enough for every encoder tick. Changing num_partials is NOT supported
 * here - it changes oscs_per_voice and the parent's preset, so it needs a full
 * additive_voice_configure_track(). */
void additive_voice_push_live(uint8_t synth_id, const additive_voice_t *voice);

#ifdef __cplusplus
}
#endif

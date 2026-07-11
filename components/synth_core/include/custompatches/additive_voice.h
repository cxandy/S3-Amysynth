#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Additive / partials voice engine (AMY BYO_PARTIALS) ─────────────────────
 * Wraps AMY's build-your-own partials wave type as a single melodic "voice":
 * one BYO_PARTIALS control osc (osc 0, relative) plus N PARTIAL sine oscs
 * (osc 1..N, relative), so oscs_per_voice is N+1. Unlike the FM/ALGO voice
 * there is no algo_source[] routing table: at note-on AMY's
 * partials_note_on() derives the child set purely from the parent's `preset`
 * field (= N) and osc adjacency (child i lives at parent+1+i), and
 * render_partials() drives every child's per-block amplitude from the
 * parent's post-envelope amp via the velocity -> amp_coefs[COEF_VEL] path.
 * The parent therefore carries the voice's shared amplitude envelope (the
 * row's per-track ADSR editor targets osc 0, exactly as it does the FM
 * control osc); the children carry the spectrum.
 *
 * Pitch math: AMY's logfreq is in octaves, so a partial at frequency ratio r
 * gets freq_coefs[COEF_CONST] = log2f(r) on top of COEF_NOTE tracking —
 * harmonic 2 is +1.0 octave, harmonic 3 is +1.585, and inharmonic (bell)
 * ratios are equally expressible.
 *
 * Per-partial `level` is that partial's own amp_coefs[COEF_CONST]; a level of
 * 0 silences it, which is how presets use fewer than ADD_MAX_PARTIALS
 * partials without changing the osc layout. Optional per-partial `decay_ms`
 * adds a local decay-to-zero EG0 on that child so upper partials can ring
 * down faster than the parent envelope (the bell preset relies on this);
 * 0 means the partial follows the parent envelope alone. */

/* Hard ceiling on the partial count: a voice costs ADD_MAX_PARTIALS + 1 oscs
 * from the global pool (max_oscs = 250), so 13 oscs/voice worst case keeps a
 * 4-row x 4-voice additive layer at 208 oscs — under budget with headroom.
 * Fixed at compile time for the same reason FM_NUM_OPS is. */
#define ADD_MAX_PARTIALS 12

typedef struct {
    uint8_t num_partials;               /* 1..ADD_MAX_PARTIALS (N)              */
    float   ratio[ADD_MAX_PARTIALS];    /* freq ratio of each partial (1,2,3..) */
    float   level[ADD_MAX_PARTIALS];    /* 0..1 relative level of each partial  */
    float   decay_ms[ADD_MAX_PARTIALS]; /* per-partial decay; 0 = follow parent */
} additive_voice_t;

/* The single live-editable "custom" additive voice (SEQ_PATCH_ADDITIVE_CUSTOM).
 * Owned by this module; a future additive UI screen mutates it directly (the
 * s_fm_voice / s_fx convention), then calls
 * sequencer_core_additive_voice_changed() to push the edit to AMY. Until that
 * screen exists it plays the drawbar-organ default. */
extern additive_voice_t s_additive_voice;

/* Fill *v with the mellow drawbar-organ default: first 8 harmonics at 1/n
 * roll-off, all following the parent envelope. Used to initialise
 * s_additive_voice and by additive_presets.c as a shared starting point. */
void additive_voice_default(additive_voice_t *v);

/* Full (re)configure of one synth slot as an (N+1)-osc additive voice:
 * allocates oscs_per_voice = N+1, wires osc 0 (BYO_PARTIALS, preset = N,
 * shared envelope) and oscs 1..N (PARTIAL sines, ratio + level + optional
 * per-partial decay). Call after allocating/reallocating the synth (patch
 * switch or layer creation). */
void additive_voice_configure_track(uint8_t synth_id, uint16_t num_voices,
                                    const additive_voice_t *voice);

/* Live update of an already-configured additive voice: pushes ratio/level to
 * oscs 1..N without reallocating oscs_per_voice. Cheap enough to call on
 * every encoder tick. Changing num_partials live is NOT supported here — it
 * changes oscs_per_voice and the parent's preset, so it needs a full
 * additive_voice_configure_track() (same constraint FM has on its fixed
 * operator layout). */
void additive_voice_push_live(uint8_t synth_id, const additive_voice_t *voice);

#ifdef __cplusplus
}
#endif

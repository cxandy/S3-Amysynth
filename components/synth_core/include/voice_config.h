#pragma once

/* Shared voice-config layer — extraction target for backlog item 14.
 * Phase 0 lands only the leaf utilities and canonical constants; the
 * builder/LFO topology (voice_build_wave / voice_apply_native_lfo /
 * voice_params_t) follows in HW-A/B-gated phases (see
 * specs/spec-14-shared-voice-config.md). */

#include "seq_model.h"     /* seq_env_t, lfo_wave_t */
#include <stdbool.h>
#include <stdint.h>

/* Canonical LFO-target depth scalars. Single source of truth, was duplicated
 * across arp_core.c and seq_core_editors.c. */
#define VOICE_LFO_DEPTH_FILTER 3.0f
#define VOICE_LFO_DEPTH_AMP    0.5f
#define VOICE_LFO_DEPTH_PITCH  1.0f
#define VOICE_LFO_DEPTH_SCAN   0.5f

/* lfo_wave_t -> AMY wave constant (hoisted; identical in both copies). */
uint16_t voice_lfo_wave_to_amy(lfo_wave_t wave);

/* KS/NOISE onset-transient attack floor (2 ms); KS also zeroes sustain. */
void voice_env_apply_ks_noise_floor(seq_env_t *env, bool is_ks, bool is_noise);

/* ── Shared WAVE-voice skeleton ──────────────────────────────────────────
 * The canonical "N voices, osc0 = note-following carrier" build used by the
 * arp, the drone, and the melodic sequencer. voice_build_wave() sends two
 * events: the pool definition and the osc0 skeleton. It deliberately does
 * NOT touch osc1, envelopes, filters, or mod routing — each engine layers
 * its own specialization (native LFO carrier, PULSE stutter gate, sweep
 * filter) on top as follow-up deltas.
 *
 * AMY does not reset the osc pool when the same num_voices/oscs_per_voice
 * is re-sent, so rebuilding through this function never glitches held
 * voices; that same property is why per-target COEF_MOD state must be
 * cleared explicitly on reconfigure (see voice_apply_native_lfo). */
typedef struct {
    uint8_t  synth;
    uint8_t  num_voices;
    uint8_t  oscs_per_voice;       /* 2 when osc1 is reserved as a carrier */
    uint16_t wave;                 /* AMY wave constant for osc0           */
    float    osc0_amp_const;       /* arp/melodic 1.0; drone const_sent    */
    float    osc0_amp_vel;         /* arp/melodic 1.0; drone 0.0           */
    bool     ks_feedback_authored; /* authored Q drives KS string decay    */
    float    ks_feedback_q;        /* seq_filter_t resonance when authored */
    int16_t  wt_preset;            /* >=0 => e->preset (WAVETABLE); else -1 */
} voice_wave_cfg_t;

/* Core-0 / UI-task only; pushes through amy_helpers (never amy_queue_lock). */
void voice_build_wave(const voice_wave_cfg_t *cfg);

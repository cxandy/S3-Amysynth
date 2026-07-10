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

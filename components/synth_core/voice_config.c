#include "voice_config.h"
#include "amy.h"            /* wave constants, COEF_* indices */
#include "amy_helpers.h"    /* shared scratch-event begin/send */
#include "sequencer_core.h" /* sequencer_core_ks_feedback_from_q */

uint16_t voice_lfo_wave_to_amy(lfo_wave_t wave)
{
    switch (wave) {
        case LFO_WAVE_SINE:     return SINE;
        case LFO_WAVE_TRIANGLE: return TRIANGLE;
        case LFO_WAVE_SAW_UP:   return SAW_UP;
        case LFO_WAVE_SAW_DOWN: return SAW_DOWN;
        case LFO_WAVE_SQUARE:   return PULSE;
        default:                return SINE;   /* LFO_WAVE_RANDOM -> SINE fallback */
    }
}

void voice_env_apply_ks_noise_floor(seq_env_t *env, bool is_ks, bool is_noise)
{
    if (!env) return;
    if (is_ks) {
        env->attack_ms   = 2;   /* onset transient suppressed by attack ramp */
        env->sustain_pct = 0;   /* string body carried by KS feedback decay  */
    } else if (is_noise) {
        env->attack_ms   = 2;   /* onset transient suppressed by attack ramp */
    }
}

void voice_build_wave(const voice_wave_cfg_t *cfg)
{
    if (!cfg) return;

    /* Pool definition. Re-sending an unchanged shape is a no-op in AMY, so
     * callers may rebuild freely without resetting live voices. */
    amy_event *e = amy_helpers_event_begin();
    e->synth          = cfg->synth;
    e->num_voices     = cfg->num_voices;
    e->oscs_per_voice = cfg->oscs_per_voice;
    amy_helpers_event_send(e);

    /* osc 0: note-following carrier (COEF_NOTE=1), EG0-gated amplitude. */
    e = amy_helpers_event_begin();
    e->synth = cfg->synth;
    e->osc   = 0;
    e->wave  = cfg->wave;
    if (cfg->wt_preset >= 0) e->preset = cfg->wt_preset;
    if (cfg->wave == KS) {
        /* Authored Q drives KS string decay once the user has dialed it;
         * otherwise keep the fixed default until they touch Q. */
        e->feedback = cfg->ks_feedback_authored
            ? sequencer_core_ks_feedback_from_q(cfg->ks_feedback_q)
            : 0.9f;
    }
    e->freq_coefs[COEF_NOTE] = 1.0f;
    e->amp_coefs[COEF_CONST] = cfg->osc0_amp_const;
    e->amp_coefs[COEF_VEL]   = cfg->osc0_amp_vel;
    e->amp_coefs[COEF_EG0]   = 1.0f;
    amy_helpers_event_send(e);
}

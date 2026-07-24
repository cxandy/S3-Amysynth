#include "voice_config.h"
#include "amy.h"            /* wave constants, COEF_* indices */
#include "amy_helpers.h"    /* shared scratch-event begin/send */
#include "sequencer_core.h" /* sequencer_core_ks_feedback_from_q */
#include <string.h>

void voice_params_init_defaults(voice_params_t *vp)
{
    if (!vp) return;
    memset(vp, 0, sizeof(*vp));
    vp->amp_trim = 1.0f;   /* unity — the one non-zero default */
}

uint16_t voice_lfo_wave_to_amy(lfo_wave_t wave)
{
    switch (wave) {
        case LFO_WAVE_SINE:     return SINE;
        case LFO_WAVE_TRIANGLE: return TRIANGLE;
        case LFO_WAVE_SAW_UP:   return SAW_UP;
        case LFO_WAVE_SAW_DOWN: return SAW_DOWN;
        case LFO_WAVE_SQUARE:   return PULSE;
        case LFO_WAVE_RANDOM:   return NOISE;  /* native S&H at the carrier rate */
        default:                return SINE;
    }
}

uint8_t voice_wob_depth_to_db(uint8_t wob_depth)
{
    if (wob_depth > 100u) wob_depth = 100u;
    return (uint8_t)(((unsigned)wob_depth * VOICE_WOB_DB_MAX + 50u) / 100u);
}

uint8_t voice_wob_db_to_depth(uint8_t db)
{
    if (db > VOICE_WOB_DB_MAX) db = VOICE_WOB_DB_MAX;
    return (uint8_t)(((unsigned)db * 100u + VOICE_WOB_DB_MAX / 2u) / VOICE_WOB_DB_MAX);
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
        /* Authored feedback drives KS string decay directly once dialed;
         * otherwise (or on the 0 "never set" sentinel) the fixed default. */
        float fb = cfg->ks_feedback;
        if (fb > 1.0f) fb = 1.0f;   /* > 1 would make the KS buffer diverge */
        e->feedback = (cfg->ks_feedback_authored && fb > 0.0f) ? fb : 0.9f;
    }
    e->freq_coefs[COEF_NOTE] = 1.0f;
    e->amp_coefs[COEF_CONST] = cfg->osc0_amp_const;
    e->amp_coefs[COEF_VEL]   = cfg->osc0_amp_vel;
    e->amp_coefs[COEF_EG0]   = 1.0f;
    amy_helpers_event_send(e);
}

void voice_apply_native_lfo(uint8_t synth, const seq_lfo_t *lfo, uint16_t bpm)
{
    amy_event *e;

    if (lfo && lfo->enabled && lfo->targets) {
        /* osc 0: wire mod_source to osc 1 (voice-local — AMY adds the base_osc
         * offset, so it resolves within each voice) and set the COEF_MOD depth
         * for every checked target, clearing every sibling first. One carrier
         * feeds all targets; each gets the shared depth scaled by its own
         * normalizing constant. */
        float d = (float)lfo->depth / 100.0f;
        e = amy_helpers_event_begin();
        e->synth      = synth;
        e->osc        = 0;
        e->mod_source = 1;
        e->filter_freq_coefs[COEF_MOD] = 0.0f;
        e->amp_coefs[COEF_MOD]         = 0.0f;
        e->freq_coefs[COEF_MOD]        = 0.0f;
        e->duty_coefs[COEF_MOD]        = 0.0f;
        e->pan_coefs[COEF_MOD]         = 0.0f;
        if (LFO_HAS_TGT(lfo, LFO_TARGET_FILTER)) e->filter_freq_coefs[COEF_MOD] = voice_lfo_filter_octaves(lfo);
        if (LFO_HAS_TGT(lfo, LFO_TARGET_AMP))    e->amp_coefs[COEF_MOD]         = d * VOICE_LFO_DEPTH_AMP;
        if (LFO_HAS_TGT(lfo, LFO_TARGET_PITCH))  e->freq_coefs[COEF_MOD]        = d * VOICE_LFO_DEPTH_PITCH;
        if (LFO_HAS_TGT(lfo, LFO_TARGET_SCAN))   e->duty_coefs[COEF_MOD]        = d * VOICE_LFO_DEPTH_SCAN;
        if (LFO_HAS_TGT(lfo, LFO_TARGET_PAN)) {
            /* Pan is [0,1], not bipolar: set the center baseline and swing
             * COEF_MOD around it in the same event. */
            e->pan_coefs[COEF_CONST] = 0.5f;
            e->pan_coefs[COEF_MOD]   = d * VOICE_LFO_DEPTH_PAN;
        }
        amy_helpers_event_send(e);

        /* osc 1: BPM-synced carrier — no pitch tracking, no velocity, no
         * envelope; amp CONST=1 so AMY computes a mod value every block.
         * WOBBLE (second-order LFO): osc2 is chained as osc1's own mod_source
         * (AMY resolves the relative index within the voice), targeting BOTH
         * the carrier's amplitude (= modulation depth breathing) and its
         * log-frequency (= rate wobble), per the chained-modulator semantics
         * (see AMY mod_osc_would_cause_loop / compute_mod_scale). Both coefs
         * are ALWAYS written so a stale wobble from a previous config can
         * never keep modulating after it is turned off (same clear-siblings
         * contract as the target coefs above). */
        float w = (float)lfo->wob_depth / 100.0f;
        e = amy_helpers_event_begin();
        e->synth                  = synth;
        e->osc                    = 1;
        e->wave                   = voice_lfo_wave_to_amy(lfo->wave);
        e->freq_coefs[COEF_CONST] = lfo_rate_to_hz(lfo->rate, bpm);
        e->freq_coefs[COEF_NOTE]  = 0.0f;
        e->freq_coefs[COEF_BEND]  = 0.0f;
        e->amp_coefs[COEF_CONST]  = 1.0f;
        e->amp_coefs[COEF_VEL]    = 0.0f;
        e->amp_coefs[COEF_EG0]    = 0.0f;
        e->mod_source             = 2;
        e->amp_coefs[COEF_MOD]    = w * VOICE_WOB_DEPTH_AMP;
        /* Reach toggle: depth-only leaves the carrier's rate steady. Written
         * unconditionally (zero, not skipped) so switching to depth-only can
         * never leave a previous rate coupling modulating. */
        e->freq_coefs[COEF_MOD]   = lfo->wob_depth_only
                                  ? 0.0f : w * VOICE_WOB_DEPTH_RATE;
        amy_helpers_event_send(e);

        /* osc 2: the wobble modulator itself — fixed TRIANGLE (smooth, no
         * steps on the depth/rate rails), BPM-synced, dormant at 0 %. */
        e = amy_helpers_event_begin();
        e->synth                  = synth;
        e->osc                    = 2;
        e->wave                   = TRIANGLE;
        e->freq_coefs[COEF_CONST] = lfo_rate_to_hz((lfo_rate_t)lfo->wob_rate, bpm);
        e->freq_coefs[COEF_NOTE]  = 0.0f;
        e->freq_coefs[COEF_BEND]  = 0.0f;
        e->amp_coefs[COEF_CONST]  = (lfo->wob_depth > 0) ? 1.0f : 0.0f;
        e->amp_coefs[COEF_VEL]    = 0.0f;
        e->amp_coefs[COEF_EG0]    = 0.0f;
        amy_helpers_event_send(e);
    } else {
        /* Disabled: clear the mod coupling, silence the carrier. */
        e = amy_helpers_event_begin();
        e->synth                       = synth;
        e->osc                         = 0;
        e->filter_freq_coefs[COEF_MOD] = 0.0f;
        e->amp_coefs[COEF_MOD]         = 0.0f;
        e->freq_coefs[COEF_MOD]        = 0.0f;
        e->duty_coefs[COEF_MOD]        = 0.0f;
        e->pan_coefs[COEF_MOD]         = 0.0f;
        amy_helpers_event_send(e);

        e = amy_helpers_event_begin();
        e->synth                 = synth;
        e->osc                   = 1;
        e->amp_coefs[COEF_CONST] = 0.0f;  /* dormant */
        e->amp_coefs[COEF_MOD]   = 0.0f;  /* clear wobble depth coupling */
        e->freq_coefs[COEF_MOD]  = 0.0f;  /* clear wobble rate coupling  */
        amy_helpers_event_send(e);

        e = amy_helpers_event_begin();
        e->synth                 = synth;
        e->osc                   = 2;
        e->amp_coefs[COEF_CONST] = 0.0f;  /* wobble modulator dormant */
        amy_helpers_event_send(e);
    }
}

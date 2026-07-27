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

/* ── Lazy LFO-sibling materialization state ──────────────────────────────
 * Per synth: has anything ever sent an event to the LFO sibling oscs
 * (osc1/osc2) since the pool was last provably reset? AMY allocates an
 * osc's ~532 B synth struct the first time an event addresses it
 * (ensure_osc_allocd at event→delta conversion), so the disabled-path
 * "park" events below would themselves BE the allocation. Skipping them
 * is safe exactly when this bit is clear: the oscs are then either NULL
 * or AMY-reset (pool-shape change posts RESET_OSC per osc), and both
 * states are silent and carry no mod coupling.
 *
 * The bit may only be CLEARED on provable freshness (a known pool-shape
 * change in voice_build_wave); everything ambiguous keeps it SET, because
 * a wrongly-set bit only costs the memory savings while a wrongly-clear
 * bit skips parking a live carrier (the stale-COEF_MOD DC-rail class).
 * The shape cache exists solely to make that freshness proof: 0 means
 * "unknown" (never built here, or a foreign patch configured the synth
 * since — see voice_lfo_mark_foreign), and unknown never clears the bit.
 *
 * Core-0 / UI-task only, like every entry point in this file. */
#define VOICE_LFO_SYNTH_MAX 128u

static uint16_t s_pool_shape[VOICE_LFO_SYNTH_MAX];        /* (voices<<8)|oscs; 0 = unknown */
static uint8_t  s_lfo_materialized[VOICE_LFO_SYNTH_MAX / 8u];

static inline bool lfo_materialized(uint8_t synth)
{
    if (synth >= VOICE_LFO_SYNTH_MAX) return true;  /* out of range: park always */
    return (s_lfo_materialized[synth >> 3] >> (synth & 7u)) & 1u;
}

static inline void lfo_set_materialized(uint8_t synth, bool on)
{
    if (synth >= VOICE_LFO_SYNTH_MAX) return;
    if (on) s_lfo_materialized[synth >> 3] |=  (uint8_t)(1u << (synth & 7u));
    else    s_lfo_materialized[synth >> 3] &= (uint8_t)~(1u << (synth & 7u));
}

bool voice_lfo_siblings_materialized(uint8_t synth)
{
    return lfo_materialized(synth);
}

void voice_lfo_mark_foreign(uint8_t synth)
{
    if (synth >= VOICE_LFO_SYNTH_MAX) return;
    s_pool_shape[synth] = 0;                 /* shape proof is gone */
    lfo_set_materialized(synth, true);       /* park always until proven fresh */
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

    /* A *known different* shape forces AMY to reallocate the voice oscs
     * (patches.c only no-ops on an identical shape) and RESET_OSC each new
     * one, so the LFO siblings are provably fresh: forget them. An unknown
     * cache (first build, or foreign config since) proves nothing. */
    if (cfg->synth < VOICE_LFO_SYNTH_MAX) {
        uint16_t shape = (uint16_t)(((uint16_t)cfg->num_voices << 8)
                                    | cfg->oscs_per_voice);
        if (s_pool_shape[cfg->synth] != 0 && s_pool_shape[cfg->synth] != shape)
            lfo_set_materialized(cfg->synth, false);
        s_pool_shape[cfg->synth] = shape;
    }

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

        lfo_set_materialized(synth, true);
    } else {
        /* Disabled: clear the mod coupling, silence the carrier. The osc0
         * event always goes out (the carrier exists and costs nothing). */
        e = amy_helpers_event_begin();
        e->synth                       = synth;
        e->osc                         = 0;
        e->filter_freq_coefs[COEF_MOD] = 0.0f;
        e->amp_coefs[COEF_MOD]         = 0.0f;
        e->freq_coefs[COEF_MOD]        = 0.0f;
        e->duty_coefs[COEF_MOD]        = 0.0f;
        e->pan_coefs[COEF_MOD]         = 0.0f;
        amy_helpers_event_send(e);

        /* Never-materialized siblings are NULL or AMY-reset — silent, no
         * coupling, nothing to park. Sending the events anyway would
         * allocate them and forfeit the lazy reservation. */
        if (!lfo_materialized(synth))
            return;

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

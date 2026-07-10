#include "sequencer_core/seq_core_internal.h"
#include "voice_config.h"

/* ── State definitions — owns LFO phase accumulators ────────────────── */
/* Software LFO state (phase accumulator, per-layer/track) */
float    s_lfo_phase[MAX_LAYERS][SEQ_TRACKS]; /* 0..1 normalized */
float    s_lfo_hz[MAX_LAYERS][SEQ_TRACKS];    /* Hz from rate+BPM; 0 = native or inactive */
float    s_lfo_rnd[MAX_LAYERS][SEQ_TRACKS];   /* S&H held value   */
uint32_t s_lfo_rng_state = 0xDEADBEEFu;

/* ── AMY native LFO helpers (wave patches only) ──────────────────────── */

#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO

#define lfo_wave_to_amy voice_lfo_wave_to_amy

/* True when the given authored track should use AMY native LFO.
 * Caller is responsible for ensuring the layer uses a wave patch. */
static bool is_native_lfo_track(const seq_lfo_t *lfo)
{
    return lfo->enabled
           && lfo->target != LFO_TARGET_PAN
           && lfo->wave   != LFO_WAVE_RANDOM;
}

/* Push native LFO config to one track's AMY synth (wave-patch layers only).
 * Handles both activation (carrier active) and deactivation (osc 1 dormant,
 * COEF_MOD cleared) so the caller always reaches a consistent AMY state. */
static void melodic_configure_native_lfo_track(const seq_layer_t *layer, uint8_t track)
{
    const seq_lfo_t *lfo   = &layer->lfo[track];
    uint8_t          synth = layer->synth_id[track];

    if (is_native_lfo_track(lfo)) {
        float d = (float)lfo->depth / 100.0f;
        /* osc 0: wire mod_source to osc 1 and set COEF_MOD depth */
        amy_event *e = amy_helpers_event_begin();
        e->synth      = synth;
        e->osc        = 0;
        e->mod_source = 1;
        /* Clear every target's mod coef before selecting one so a prior target
         * (e.g. AMP, which rides AMY's convex dB-amp COEF_MOD path) cannot
         * persist across a target switch and rail the output — re-sending the
         * same voice count does not reset the osc pool. */
        e->filter_freq_coefs[COEF_MOD] = 0.0f;
        e->amp_coefs[COEF_MOD]         = 0.0f;
        e->freq_coefs[COEF_MOD]        = 0.0f;
        e->duty_coefs[COEF_MOD]        = 0.0f;
        switch (lfo->target) {
            case LFO_TARGET_FILTER: e->filter_freq_coefs[COEF_MOD] = d * VOICE_LFO_DEPTH_FILTER; break;
            case LFO_TARGET_AMP:    e->amp_coefs[COEF_MOD]         = d * VOICE_LFO_DEPTH_AMP;    break;
            case LFO_TARGET_PITCH:  e->freq_coefs[COEF_MOD]        = d * VOICE_LFO_DEPTH_PITCH;  break;
            case LFO_TARGET_SCAN:   e->duty_coefs[COEF_MOD]        = d * VOICE_LFO_DEPTH_SCAN;   break;
            default: break;
        }
        amy_helpers_event_send(e);

        /* osc 1: BPM-synced carrier (no pitch tracking, no velocity, no envelope) */
        e = amy_helpers_event_begin();
        e->synth                  = synth;
        e->osc                    = 1;
        e->wave                   = lfo_wave_to_amy(lfo->wave);
        e->freq_coefs[COEF_CONST] = lfo_rate_to_hz(lfo->rate, s_bpm);
        e->freq_coefs[COEF_NOTE]  = 0.0f;
        e->freq_coefs[COEF_BEND]  = 0.0f;
        e->amp_coefs[COEF_CONST]  = 1.0f;
        e->amp_coefs[COEF_VEL]    = 0.0f;
        e->amp_coefs[COEF_EG0]    = 0.0f;
        amy_helpers_event_send(e);
    } else {
        /* Disabled or PAN/RANDOM fallback: clear mod coupling, silence carrier */
        amy_event *e = amy_helpers_event_begin();
        e->synth                       = synth;
        e->osc                         = 0;
        e->filter_freq_coefs[COEF_MOD] = 0.0f;
        e->amp_coefs[COEF_MOD]         = 0.0f;
        e->freq_coefs[COEF_MOD]        = 0.0f;
        e->duty_coefs[COEF_MOD]        = 0.0f;
        amy_helpers_event_send(e);

        e = amy_helpers_event_begin();
        e->synth                 = synth;
        e->osc                   = 1;
        e->amp_coefs[COEF_CONST] = 0.0f;  /* dormant */
        amy_helpers_event_send(e);
    }
}

#endif /* CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO */

/* ── Per-row melodic envelope (runtime-editable) ─────────────────────────── */

/* Push the given row's stored envelope to that row's OWN AMY synth. */
void sequencer_configure_melodic_envelope_track(uint8_t layer_idx, uint8_t track)
{
#if CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED
    const seq_layer_t *layer = &s_layers[layer_idx];
    seq_env_t env = *seq_layer_env(layer_idx, track);
    voice_env_apply_ks_noise_floor(&env,
                                   layer->patch == SEQ_PATCH_KS,
                                   layer->patch == SEQ_PATCH_NOISE);
    float sustain = (float)env.sustain_pct / 100.0f;

    amy_event *e = amy_helpers_event_begin();
    e->synth = layer->synth_id[track];
    e->bp_is_set[0] = 1;
    e->eg_type[0] = env.eg_type;
    e->eg0_times[0] = env.attack_ms;
    e->eg0_values[0] = 1.0f;
    e->eg0_times[1] = env.decay_ms;
    e->eg0_values[1] = sustain;
    e->eg0_times[2] = env.release_ms;
    e->eg0_values[2] = 0.0f;
    amy_helpers_event_send(e);
#else
    (void)layer_idx; (void)track;
#endif
}

bool sequencer_core_get_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         seq_env_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = *seq_layer_env(layer_idx, track);
    return true;
}

void sequencer_core_set_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         const seq_env_t *env)
{
    if (!env || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    seq_env_t *dst = seq_layer_env(layer_idx, track);
    dst->attack_ms   = SEQ_CLAMP_U32(env->attack_ms,  2, 60000);
    dst->decay_ms    = SEQ_CLAMP_U32(env->decay_ms,   0, 60000);
    dst->sustain_pct = SEQ_CLAMP_U8(env->sustain_pct, 0, 100);
    dst->release_ms  = SEQ_CLAMP_U32(env->release_ms, 0, 60000);
    dst->eg_type     = env->eg_type;

    /* Committing in the graph editor establishes this row's authority over the
     * patch's own envelope. From now on patch changes re-impose this custom env
     * (until the user re-authors). Each row owns its own synth, so the push
     * affects only this row. */
    layer->env_authored[track] = true;
    sequencer_configure_melodic_envelope_track(layer_idx, track);
    ESP_LOGI(TAG, "env L%u row%u -> A%u D%u S%u%% R%u (authored)",
             layer_idx, track, (unsigned)dst->attack_ms, (unsigned)dst->decay_ms,
             (unsigned)dst->sustain_pct, (unsigned)dst->release_ms);
#if CONFIG_SEQ_ENV_DEBUG_DUMP
    ESP_LOGW(TAG, "ENVDUMP sent to synth %u: eg_type=%u bp0=[%ums,1.0] "
                  "bp1=[%ums,%.3f] bp2=[%ums,0.0]",
             (unsigned)layer->synth_id[track], (unsigned)dst->eg_type,
             (unsigned)dst->attack_ms,
             (unsigned)dst->decay_ms, (double)dst->sustain_pct / 100.0,
             (unsigned)dst->release_ms);
#endif
}

/* ── Per-row second envelope (runtime-editable EG1) ──────────────────────── */

/* Push the given row's stored EG1 to that row's OWN AMY synth (osc 0 — the
 * same voice oscillator EG0 targets). No KS/NOISE onset-floor special case:
 * EG1 has no built-in role for those waveforms today. */
void sequencer_configure_melodic_envelope1_track(uint8_t layer_idx, uint8_t track)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    const seq_env_t   *env   = seq_layer_env1(layer_idx, track);
    sequencer_core_push_envelope_eg1(layer->synth_id[track], 0, env);
}

bool sequencer_core_get_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                          seq_env_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = *seq_layer_env1(layer_idx, track);
    return true;
}

void sequencer_core_set_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                          const seq_env_t *env)
{
    if (!env || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    seq_env_t *dst = seq_layer_env1(layer_idx, track);
    dst->attack_ms   = SEQ_CLAMP_U32(env->attack_ms,  2, 60000);
    dst->decay_ms    = SEQ_CLAMP_U32(env->decay_ms,   0, 60000);
    dst->sustain_pct = SEQ_CLAMP_U8(env->sustain_pct, 0, 100);
    dst->release_ms  = SEQ_CLAMP_U32(env->release_ms, 0, 60000);
    dst->eg_type     = env->eg_type;

    layer->env1_authored[track] = true;
    sequencer_configure_melodic_envelope1_track(layer_idx, track);
    ESP_LOGI(TAG, "env1 L%u row%u -> A%u D%u S%u%% R%u (authored)",
             layer_idx, track, (unsigned)dst->attack_ms, (unsigned)dst->decay_ms,
             (unsigned)dst->sustain_pct, (unsigned)dst->release_ms);
}

/* ── Per-row melodic filter (runtime-editable) ─────────────────────────── */

/* Map a Q value (same [0.51, 8.0] range enforced by sequencer_core_set_melodic_filter)
 * linearly onto AMY's KS oscillator feedback range [0.0, 1.0]. Q=8.0 -> feedback=1.0
 * is the verified-safe ceiling (lossless two-tap averaging filter, the classic
 * "infinite sustain" Karplus-Strong case); above 1.0 the KS buffer would diverge. */
float sequencer_core_ks_feedback_from_q(float q)
{
    float n = (q - 0.51f) / (8.0f - 0.51f);
    return SEQ_CLAMP_F32(n, 0.0f, 1.0f);
}

/* Push one row's stored filter to its own AMY synth. */
void sequencer_configure_melodic_filter_track(uint8_t layer_idx, uint8_t track)
{
    const seq_layer_t   *layer = &s_layers[layer_idx];
    const seq_filter_t  *f     = &layer->filter[track];
    amy_event *e = amy_helpers_event_begin();
    e->synth       = layer->synth_id[track];
    if (f->enabled) {
        e->filter_type = f->filter_type;
        e->filter_freq_coefs[COEF_CONST] = f->cutoff_hz;
        e->resonance = f->resonance;
        /* EG1 -> cutoff depth (octaves). Only wired when the user has dialed a
         * non-zero amount, so a plain melodic filter is byte-for-byte unchanged.
         * COEF_EG1 is the second (aux) envelope generator; on melodic tracks
         * nothing routes it to amplitude (no amp_coefs[COEF_EG1] anywhere), so
         * there is no amp/filter double-use to arbitrate — the amount==0 gate is
         * the only guard needed. Same convention as arp_core.c:253. */
        if (f->filter_env_amount != 0.0f) {
            e->filter_freq_coefs[COEF_EG1] = f->filter_env_amount;
        }
    } else {
        e->filter_type = FILTER_NONE;
    }
    if (layer->patch == SEQ_PATCH_KS) {
        e->feedback = sequencer_core_ks_feedback_from_q(f->resonance);
    }
    amy_helpers_event_send(e);

    /* Guarantee valid EG1 breakpoints on this synth whenever the filter env is
     * live, so filter_freq_coefs[COEF_EG1] modulates a real ramp instead of a
     * stuck unity gate (AMY treats a never-configured breakpoint set as a
     * permanent 1.0 — see arp_core.c:247-253). Uses the row's stored EG1
     * (authored shape, or the zeroed default). Skipped entirely when inert. */
    if (f->enabled && f->filter_env_amount != 0.0f) {
        sequencer_core_push_envelope_eg1(layer->synth_id[track], 0,
                                         seq_layer_env1(layer_idx, track));
    }
}

bool sequencer_core_get_melodic_filter(uint8_t layer_idx, uint8_t track,
                                       seq_filter_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = s_layers[layer_idx].filter[track];
    return true;
}

void sequencer_core_set_melodic_filter(uint8_t layer_idx, uint8_t track,
                                       const seq_filter_t *f)
{
    if (!f || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    seq_filter_t *dst = &layer->filter[track];
    dst->filter_type = (f->filter_type < 5) ? f->filter_type : FILTER_NONE;
    dst->cutoff_hz   = SEQ_CLAMP_F32(f->cutoff_hz,  65.0f, 8000.0f);
    dst->resonance   = SEQ_CLAMP_F32(f->resonance,  0.51f, 8.0f);
    dst->enabled     = f->enabled;
    dst->filter_env_amount = SEQ_CLAMP_F32(f->filter_env_amount, 0.0f, 8.0f);

    layer->filter_authored[track] = true;
    sequencer_configure_melodic_filter_track(layer_idx, track);
    ESP_LOGI(TAG, "filter L%u row%u -> type%u %.0fHz Q%.2f (authored)",
             layer_idx, track, dst->filter_type,
             (double)dst->cutoff_hz, (double)dst->resonance);
}

/* Generic filter push: shared by arp, drone (via synth_ui). */
void sequencer_core_push_filter(uint8_t synth, const seq_filter_t *f, bool is_ks)
{
    if (!f) return;
    amy_event *e = amy_helpers_event_begin();
    e->synth = synth;
    if (f->enabled) {
        e->filter_type = f->filter_type;
        e->filter_freq_coefs[COEF_CONST] = f->cutoff_hz;
        e->resonance = f->resonance;
    } else {
        e->filter_type = FILTER_NONE;
    }
    if (is_ks) {
        e->feedback = sequencer_core_ks_feedback_from_q(f->resonance);
    }
    amy_helpers_event_send(e);
}

void sequencer_core_set_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    const seq_lfo_t *lfo)
{
    if (!lfo || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    layer->lfo[track] = *lfo;
    if (layer->lfo[track].depth > 100) layer->lfo[track].depth = 100;
    layer->lfo_authored[track] = true;

#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    bool is_wave = sequencer_core_is_wave_patch(layer->patch);
    if (is_wave) {
        melodic_configure_native_lfo_track(layer, track);
        /* Restore static target value when disabled or on software-fallback path
         * (PAN/RANDOM): native clears COEF_MOD but doesn't push the neutral coef. */
        if (!lfo->enabled || !is_native_lfo_track(&layer->lfo[track])) {
            if (lfo->target == LFO_TARGET_FILTER)
                sequencer_core_push_filter(layer->synth_id[track], &layer->filter[track],
                                           layer->patch == SEQ_PATCH_KS);
            else
                lfo_push_target_neutral(layer->synth_id[track], lfo->target);
        }
        /* s_lfo_hz=0 for native tracks so the service loop skips them;
         * non-zero for PAN/RANDOM fallback so the service loop picks them up. */
        s_lfo_hz[layer_idx][track] = (lfo->enabled && is_native_lfo_track(&layer->lfo[track]))
                                     ? 0.0f
                                     : (lfo->enabled ? lfo_rate_to_hz(lfo->rate, s_bpm) : 0.0f);
        ESP_LOGI(TAG, "LFO L%u T%u %s %.2f Hz d=%u tgt=%u [native]",
                 layer_idx, track, lfo->enabled ? "ON" : "OFF",
                 (double)s_lfo_hz[layer_idx][track], lfo->depth, lfo->target);
        return;
    }
#endif

    /* Software path: non-wave patches, or native LFO disabled at compile time */
    if (!lfo->enabled) {
        if (lfo->target == LFO_TARGET_FILTER) {
            sequencer_core_push_filter(layer->synth_id[track], &layer->filter[track],
                                       layer->patch == SEQ_PATCH_KS);
        } else {
            lfo_push_target_neutral(layer->synth_id[track], lfo->target);
        }
        s_lfo_hz[layer_idx][track] = 0.0f;
    } else {
        s_lfo_hz[layer_idx][track] = lfo_rate_to_hz(lfo->rate, s_bpm);
    }
    ESP_LOGI(TAG, "LFO L%u T%u %s %.2f Hz d=%u tgt=%u",
             layer_idx, track, lfo->enabled ? "ON" : "OFF",
             (double)s_lfo_hz[layer_idx][track], lfo->depth, lfo->target);
}

bool sequencer_core_get_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    seq_lfo_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = s_layers[layer_idx].lfo[track];
    return true;
}

void sequencer_core_lfo_service(void)
{
    const float DT = 0.05f; /* 20 Hz */
    for (int li = 0; li < s_num_layers; li++) {
        for (int tr = 0; tr < SEQ_TRACKS; tr++) {
            if (!s_layers[li].lfo_authored[tr]) continue;
            const seq_lfo_t *lfo = &s_layers[li].lfo[tr];
            if (!lfo->enabled) continue;
            float hz = s_lfo_hz[li][tr];
            if (hz <= 0.0f) continue;

            float ph = s_lfo_phase[li][tr] + hz * DT;
            if (ph >= 1.0f) {
                ph -= 1.0f;
                if (lfo->wave == LFO_WAVE_RANDOM)
                    s_lfo_rnd[li][tr] = lfo_next_rand();
            }
            s_lfo_phase[li][tr] = ph;

            float val;
            switch (lfo->wave) {
                case LFO_WAVE_SINE:
                    val = sinf(2.0f * 3.14159265f * ph);          break;
                case LFO_WAVE_TRIANGLE:
                    val = (ph < 0.5f) ? (4.0f*ph - 1.0f)
                                      : (3.0f - 4.0f*ph);         break;
                case LFO_WAVE_SAW_UP:   val =  2.0f*ph - 1.0f;   break;
                case LFO_WAVE_SAW_DOWN: val =  1.0f - 2.0f*ph;   break;
                case LFO_WAVE_SQUARE:   val = (ph < 0.5f) ? 1.0f : -1.0f; break;
                case LFO_WAVE_RANDOM:   val = s_lfo_rnd[li][tr];  break;
                default:                val = 0.0f;                break;
            }

            float d   = (float)lfo->depth / 100.0f;
            uint8_t syn = s_layers[li].synth_id[tr];

            amy_event *e = amy_helpers_event_begin();
            e->synth = syn;
            switch (lfo->target) {
                case LFO_TARGET_FILTER: {
                    float base = (s_layers[li].filter[tr].enabled &&
                                  s_layers[li].filter[tr].cutoff_hz > 0.0f)
                                 ? s_layers[li].filter[tr].cutoff_hz : 1000.0f;
                    e->filter_freq_coefs[COEF_CONST] =
                        base * powf(2.0f, d * 3.0f * val);
                    break;
                }
                case LFO_TARGET_AMP:
                    e->amp_coefs[COEF_CONST] = 1.0f - d*(0.5f - 0.5f*val);
                    break;
                case LFO_TARGET_PITCH:
                    e->freq_coefs[COEF_CONST] = powf(2.0f, d * val);
                    break;
                case LFO_TARGET_PAN:
                    e->pan_coefs[COEF_CONST] = 0.5f + d*0.5f*val;
                    break;
                default: break;
            }
            amy_helpers_event_send(e);
        }
    }
}

/* ── Native LFO rebuild helpers ─────────────────────────────────────────────
 * Called after a patch/synth rebuild (sequencer_configure_synth) and after
 * BPM changes so that native LFO carrier state stays consistent with the
 * current layer patch and tempo. */

/* Re-apply the authored native LFO configuration for every track in a wave-patch
 * layer.  No-op for non-wave patches (software service loop handles those). */
void sequencer_configure_melodic_lfo(uint8_t layer_idx)
{
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    const seq_layer_t *layer = &s_layers[layer_idx];
    bool is_wave = sequencer_core_is_wave_patch(layer->patch);
    if (!is_wave) return;
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (!layer->lfo_authored[t]) continue;
        melodic_configure_native_lfo_track(layer, t);
        /* Keep s_lfo_hz in sync so the service loop skips native tracks */
        const seq_lfo_t *lfo = &layer->lfo[t];
        s_lfo_hz[layer_idx][t] =
            (lfo->enabled && is_native_lfo_track(lfo))
            ? 0.0f
            : (lfo->enabled ? lfo_rate_to_hz(lfo->rate, s_bpm) : 0.0f);
    }
#else
    (void)layer_idx;
#endif
}

/* Update the LFO carrier frequency on all active native-LFO tracks after a
 * BPM change.  Mirrors arp_core_refresh_lfo_freq() for the melodic layer. */
void melodic_lfo_refresh_native_freq(void)
{
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    for (int li = 0; li < s_num_layers; li++) {
        const seq_layer_t *layer = &s_layers[li];
        bool is_wave = sequencer_core_is_wave_patch(layer->patch);
        if (!is_wave) continue;
        for (int tr = 0; tr < SEQ_TRACKS; tr++) {
            if (!layer->lfo_authored[tr]) continue;
            const seq_lfo_t *lfo = &layer->lfo[tr];
            if (!is_native_lfo_track(lfo)) continue;
            amy_event *e = amy_helpers_event_begin();
            e->synth                  = layer->synth_id[tr];
            e->osc                    = 1;
            e->freq_coefs[COEF_CONST] = lfo_rate_to_hz(lfo->rate, s_bpm);
            amy_helpers_event_send(e);
        }
    }
#endif
}

/* ── Per-track amplitude trim (graph editor amp mode) ────────────────────────
 * amp_scale is a per-track multiplier applied to note velocity at emit time.
 * Default 1.0 (unity). Must be initialised to 1.0 in add_layer since
 * memset zeroes the struct.
 *
 * The setter re-emits all steps for the affected track immediately after
 * storing the value (same pattern as sequencer_core_set_track_repeat_rate).
 * This is necessary because steps are scheduled ahead-of-time with a period;
 * the sequencer does not re-emit each step on every tick during steady
 * playback, so a store-only change would be silent until an unrelated
 * re-emit event (patch change, play-stop, etc.). */

float sequencer_core_get_melodic_amp_scale(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 1.0f;
    return s_layers[layer_idx].amp_scale[track];
}

void sequencer_core_set_melodic_amp_scale(uint8_t layer_idx, uint8_t track,
                                          float v)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    s_layers[layer_idx].amp_scale[track] = v;
    /* Re-emit all steps so the new amplitude takes effect immediately. */
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t s = 0; s < layer->num_steps; s++)
        sequencer_emit_step(layer_idx, track, s);
}

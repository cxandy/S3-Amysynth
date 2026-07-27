#include "sequencer_core/seq_core_internal.h"
#include "voice_config.h"
#include "seq_clamp.h"

/* ── State definitions — owns LFO phase accumulators ────────────────── */
/* Software LFO state (phase accumulator, per-layer/track) */
float    s_lfo_phase[MAX_LAYERS][SEQ_TRACKS]; /* 0..1 normalized */
float    s_lfo_hz[MAX_LAYERS][SEQ_TRACKS];    /* Hz from rate+BPM; 0 = native or inactive */
float    s_lfo_rnd[MAX_LAYERS][SEQ_TRACKS];   /* S&H held value   */
uint32_t s_lfo_rng_state = 0xDEADBEEFu;

/* ── AMY native LFO helpers (wave patches only) ──────────────────────── */

#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO

/* True when the given authored track should use AMY native LFO.
 * Caller is responsible for ensuring the layer uses a wave patch.
 * PAN rides pan_coefs[COEF_MOD] around a 0.5 center baseline and RANDOM maps
 * to a native NOISE S&H carrier, so every enabled wave-patch LFO is native;
 * the 20 Hz software poll now serves non-wave patches only. */
static bool is_native_lfo_track(const seq_lfo_t *lfo)
{
    return lfo->enabled;
}

/* Push native LFO config to one track's AMY synth (layers whose patch has a
 * reserved carrier pair - wave build or bass presets). Handles both
 * activation (carrier active) and deactivation (carrier dormant, COEF_MOD
 * cleared) so the caller always reaches a consistent AMY state. */
static void melodic_configure_native_lfo_track(const seq_layer_t *layer, uint8_t track)
{
    const seq_lfo_t *lfo = &layer->vp[track].lfo;
    uint8_t carrier, coupled;
    if (!sequencer_core_lfo_native_layout(layer->patch, &carrier, &coupled))
        return;
    voice_apply_native_lfo_topo(layer->synth_id[track],
                                is_native_lfo_track(lfo) ? lfo : NULL, s_bpm,
                                carrier, coupled);
}

#endif /* CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO */

/* Restore the resting (neutral) coefficient for every target the LFO was
 * driving — called when the LFO is switched off so nothing stays modulated.
 * FILTER restores the track's own authored cutoff; the rest push a neutral
 * constant. Iterates the full target set (multi-target LFOs). */
static void lfo_restore_target_neutrals(const seq_layer_t *layer, uint8_t track,
                                        const seq_lfo_t *lfo)
{
    for (int t = 0; t < LFO_TARGET_COUNT; t++) {
        if (!(lfo->targets & LFO_TGT_BIT(t))) continue;
        if (t == LFO_TARGET_FILTER)
            sequencer_core_push_filter(layer->synth_id[track], &layer->vp[track].filter,
                                       layer->patch == SEQ_PATCH_KS);
        else
            lfo_push_target_neutral(layer->synth_id[track], (lfo_target_t)t);
    }
}

/* ── Per-row melodic envelope (runtime-editable) ─────────────────────────── */

/* Push the given row's stored envelope to that row's OWN AMY synth. */
void sequencer_configure_melodic_envelope_track(uint8_t layer_idx, uint8_t track)
{
#if CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED
    const seq_layer_t *layer = &s_layers[layer_idx];
    /* No KS/NOISE special-casing: the row's envelope applies verbatim to every
     * wave. The old forced onset floor + zeroed KS sustain made those patches
     * decay to silence regardless of what the user authored ("dull in use"). */
    seq_env_t env = *seq_layer_env(layer_idx, track);
    float sustain = (float)env.sustain_pct / 100.0f;

    amy_event *e = amy_helpers_event_begin();
    e->synth = layer->synth_id[track];
    e->bp_is_set[0] = 1;
    e->eg_type[0] = env.eg_type;
    e->eg0_times[0] = SEQ_CLAMP_U32(env.attack_ms,
                                    VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    e->eg0_values[0] = 1.0f;
    e->eg0_times[1] = env.decay_ms;
    e->eg0_values[1] = sustain;
    e->eg0_times[2] = SEQ_CLAMP_U32(env.release_ms,
                                    VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
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
    dst->attack_ms   = SEQ_CLAMP_U32(env->attack_ms,
                                     VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    dst->decay_ms    = SEQ_CLAMP_U32(env->decay_ms,   0, VOICE_ENV_TIME_MAX_MS);
    dst->sustain_pct = SEQ_CLAMP_U8(env->sustain_pct, 0, VOICE_ENV_SUSTAIN_MAX_PCT);
    dst->release_ms  = SEQ_CLAMP_U32(env->release_ms,
                                     VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    dst->eg_type     = env->eg_type;

    /* Committing in the graph editor establishes this row's authority over the
     * patch's own envelope. From now on patch changes re-impose this custom env
     * (until the user re-authors). Each row owns its own synth, so the push
     * affects only this row. */
    layer->vp[track].env_authored = true;
    sequencer_configure_melodic_envelope_track(layer_idx, track);
    ESP_LOGI(TAG, "env L%u T%u -> A%u D%u S%u%% R%u (authored)",
             layer_idx + 1u, track + 1u,
             (unsigned)dst->attack_ms, (unsigned)dst->decay_ms,
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
    dst->attack_ms   = SEQ_CLAMP_U32(env->attack_ms,
                                     VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    dst->decay_ms    = SEQ_CLAMP_U32(env->decay_ms,   0, VOICE_ENV_TIME_MAX_MS);
    dst->sustain_pct = SEQ_CLAMP_U8(env->sustain_pct, 0, VOICE_ENV_SUSTAIN_MAX_PCT);
    dst->release_ms  = SEQ_CLAMP_U32(env->release_ms,
                                     VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    dst->eg_type     = env->eg_type;

    layer->vp[track].env1_authored = true;
    sequencer_configure_melodic_envelope1_track(layer_idx, track);
    ESP_LOGI(TAG, "env1 L%u T%u -> A%u D%u S%u%% R%u (authored)",
             layer_idx + 1u, track + 1u,
             (unsigned)dst->attack_ms, (unsigned)dst->decay_ms,
             (unsigned)dst->sustain_pct, (unsigned)dst->release_ms);
}

/* ── Per-row melodic filter (runtime-editable) ─────────────────────────── */

/* LEGACY SNAPSHOT IMPORT ONLY. Older project files had no feedback field —
 * KS string decay was derived from the filter Q. This mapping ([0.51, 8.0] ->
 * [0, 1]) is kept solely so de_filter() can reconstruct the feedback those
 * files audibly had. Live apply paths use seq_filter_t.feedback directly. */
float sequencer_core_ks_feedback_from_q(float q)
{
    float n = (q - 0.51f) / (8.0f - 0.51f);
    return SEQ_CLAMP_F32(n, 0.0f, 1.0f);
}

/* Push one row's stored filter to its own AMY synth. */
/* Apply one filter config to a row's AMY synth. Shared by the stored-state
 * configure below and the live-preview push: the caller decides whether `f`
 * is the row's committed filter or an editor's scratch copy. */
static void melodic_filter_apply(uint8_t layer_idx, uint8_t track,
                                 const seq_filter_t *f)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
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
    /* KS string decay: the authored feedback value, pushed directly. 0 means
     * never authored — leave AMY's build-time 0.9 default in place. */
    if (layer->patch == SEQ_PATCH_KS && f->feedback > 0.0f) {
        e->feedback = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
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

void sequencer_configure_melodic_filter_track(uint8_t layer_idx, uint8_t track)
{
    melodic_filter_apply(layer_idx, track, &s_layers[layer_idx].vp[track].filter);
}

bool sequencer_core_get_melodic_filter(uint8_t layer_idx, uint8_t track,
                                       seq_filter_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = s_layers[layer_idx].vp[track].filter;
    return true;
}

void sequencer_core_set_melodic_filter(uint8_t layer_idx, uint8_t track,
                                       const seq_filter_t *f)
{
    if (!f || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    seq_filter_t *dst = &layer->vp[track].filter;
    dst->filter_type = (f->filter_type < 5) ? f->filter_type : FILTER_NONE;
    dst->cutoff_hz   = SEQ_CLAMP_F32(f->cutoff_hz,  65.0f, 8000.0f);
    dst->resonance   = SEQ_CLAMP_F32(f->resonance,  0.51f, 8.0f);
    dst->enabled     = f->enabled;
    dst->filter_env_amount = SEQ_CLAMP_F32(f->filter_env_amount, -8.0f, 8.0f);
    dst->feedback    = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);

    layer->vp[track].filter_authored = true;
    sequencer_configure_melodic_filter_track(layer_idx, track);
    ESP_LOGI(TAG, "filter L%u T%u -> type%u %.0fHz Q%.2f (authored)",
             layer_idx + 1u, track + 1u, dst->filter_type,
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
    /* KS string decay from the authored feedback field; 0 = never authored,
     * keep AMY's build-time 0.9 default. */
    if (is_ks && f->feedback > 0.0f) {
        e->feedback = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
    }
    amy_helpers_event_send(e);
}

void sequencer_core_set_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    const seq_lfo_t *lfo)
{
    if (!lfo || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    layer->vp[track].lfo = *lfo;
    if (layer->vp[track].lfo.depth > 100) layer->vp[track].lfo.depth = 100;
    layer->vp[track].lfo_authored = true;

#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    bool is_native = sequencer_core_lfo_native_layout(layer->patch, NULL, NULL);
    if (is_native) {
        melodic_configure_native_lfo_track(layer, track);
        /* Restore static target value when disabled or on software-fallback path
         * (PAN/RANDOM): native clears COEF_MOD but doesn't push the neutral coef. */
        if (!lfo->enabled || !is_native_lfo_track(&layer->vp[track].lfo)) {
            lfo_restore_target_neutrals(layer, track, lfo);
        }
        /* s_lfo_hz=0 for native tracks so the service loop skips them;
         * non-zero for PAN/RANDOM fallback so the service loop picks them up. */
        s_lfo_hz[layer_idx][track] = (lfo->enabled && is_native_lfo_track(&layer->vp[track].lfo))
                                     ? 0.0f
                                     : (lfo->enabled ? seq_lfo_sw_hz(lfo->rate, s_bpm) : 0.0f);
        ESP_LOGI(TAG, "LFO L%u T%u %s %.2f Hz d=%u tgt=0x%02x [native]",
                 layer_idx + 1u, track + 1u, lfo->enabled ? "ON" : "OFF",
                 (double)s_lfo_hz[layer_idx][track], lfo->depth, lfo->targets);
        return;
    }
#endif




    /* Software path: non-wave patches, or native LFO disabled at compile time */
    if (!lfo->enabled) {
        lfo_restore_target_neutrals(layer, track, lfo);
        s_lfo_hz[layer_idx][track] = 0.0f;
    } else {
        s_lfo_hz[layer_idx][track] = seq_lfo_sw_hz(lfo->rate, s_bpm);
    }
    ESP_LOGI(TAG, "LFO L%u T%u %s %.2f Hz d=%u tgt=0x%02x",
             layer_idx + 1u, track + 1u, lfo->enabled ? "ON" : "OFF",
             (double)s_lfo_hz[layer_idx][track], lfo->depth, lfo->targets);
}

bool sequencer_core_get_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    seq_lfo_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = s_layers[layer_idx].vp[track].lfo;
    return true;
}

void __attribute__((optimize("O3", "unroll-loops", "fast-math"))) sequencer_core_lfo_service(void) 
{
    const float DT = 0.05f; /* 20 Hz */
    for (int li = 0; li < s_num_layers; li++) {
        for (int tr = 0; tr < SEQ_TRACKS; tr++) {
            if (!s_layers[li].vp[tr].lfo_authored) continue;
            const seq_lfo_t *lfo = &s_layers[li].vp[tr].lfo;
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
            /* Multi-target: each checked target modulates its own independent
             * COEF_CONST from the same LFO value. (SCAN has no software analog —
             * it needs a wavetable voice, which always takes the native path.) */
            if (LFO_HAS_TGT(lfo, LFO_TARGET_FILTER)) {
                float base = (s_layers[li].vp[tr].filter.enabled &&
                              s_layers[li].vp[tr].filter.cutoff_hz > 0.0f)
                             ? s_layers[li].vp[tr].filter.cutoff_hz : 1000.0f;
                e->filter_freq_coefs[COEF_CONST] =
                    base * powf(2.0f, voice_lfo_filter_octaves(lfo) * val);
            }
            if (LFO_HAS_TGT(lfo, LFO_TARGET_AMP))
                e->amp_coefs[COEF_CONST] = 1.0f - d*(0.5f - 0.5f*val);
            if (LFO_HAS_TGT(lfo, LFO_TARGET_PITCH))
                e->freq_coefs[COEF_CONST] = powf(2.0f, d * val);
            if (LFO_HAS_TGT(lfo, LFO_TARGET_PAN))
                e->pan_coefs[COEF_CONST] = 0.5f + d*0.5f*val;
            amy_helpers_event_send(e);
        }
    }
}

/* ── Native LFO rebuild helpers ─────────────────────────────────────────────
 * Called after a patch/synth rebuild (sequencer_configure_synth) and after
 * BPM changes so that native LFO carrier state stays consistent with the
 * current layer patch and tempo. */

/* Re-apply the authored native LFO configuration for every track in a layer
 * whose patch reserves a carrier pair (wave build / bass presets). No-op
 * otherwise (software service loop handles those). */
void sequencer_configure_melodic_lfo(uint8_t layer_idx)
{
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (!sequencer_core_lfo_native_layout(layer->patch, NULL, NULL)) return;
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (!layer->vp[t].lfo_authored) continue;
        melodic_configure_native_lfo_track(layer, t);
        /* Keep s_lfo_hz in sync so the service loop skips native tracks */
        const seq_lfo_t *lfo = &layer->vp[t].lfo;
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
        uint8_t carrier;
        if (!sequencer_core_lfo_native_layout(layer->patch, &carrier, NULL))
            continue;
        for (int tr = 0; tr < SEQ_TRACKS; tr++) {
            if (!layer->vp[tr].lfo_authored) continue;
            const seq_lfo_t *lfo = &layer->vp[tr].lfo;
            if (!is_native_lfo_track(lfo)) continue;
            amy_event *e = amy_helpers_event_begin();
            e->synth                  = layer->synth_id[tr];
            e->osc                    = carrier;
            e->freq_coefs[COEF_CONST] = lfo_rate_to_hz(lfo->rate, s_bpm);
            amy_helpers_event_send(e);
            /* Keep the wobble modulator (carrier+1) BPM-synced as well. */
            e = amy_helpers_event_begin();
            e->synth                  = layer->synth_id[tr];
            e->osc                    = (uint8_t)(carrier + 1u);
            e->freq_coefs[COEF_CONST] = lfo_rate_to_hz((lfo_rate_t)lfo->wob_rate, s_bpm);
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
    return s_layers[layer_idx].vp[track].amp_trim;
}

void sequencer_core_set_melodic_amp_scale(uint8_t layer_idx, uint8_t track,
                                          float v)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    v = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
    s_layers[layer_idx].vp[track].amp_trim = v;
    /* Re-emit all steps so the new amplitude takes effect immediately. */
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t s = 0; s < layer->num_steps; s++)
        sequencer_emit_step(layer_idx, track, s);
}

/* ── Live-preview pushes (AMY only; the store is untouched) ──────────────────
 * The editors audition scratch values against the running engine while the
 * committed store stays the single source of truth. Cancel restores by
 * re-pushing the stored state (or reloading the layer's patch when the row was
 * never authored); confirm goes through the normal setters. Authored flags are
 * never modified by a preview. */

void sequencer_core_preview_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                             const seq_env_t *env)
{
    if (!env || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    const seq_layer_t *layer = &s_layers[layer_idx];
    sequencer_core_push_envelope(layer->synth_id[track], env);
}

void sequencer_core_preview_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                              const seq_env_t *env)
{
    if (!env || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    sequencer_core_push_envelope_eg1(s_layers[layer_idx].synth_id[track], 0, env);
}

void sequencer_core_preview_melodic_filter(uint8_t layer_idx, uint8_t track,
                                           const seq_filter_t *f)
{
    if (!f || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    /* Same clamps the committing setter applies, so the audition matches what
     * confirm would store. */
    seq_filter_t tmp = *f;
    tmp.filter_type       = (f->filter_type < 5) ? f->filter_type : FILTER_NONE;
    tmp.cutoff_hz         = SEQ_CLAMP_F32(f->cutoff_hz,  65.0f, 8000.0f);
    tmp.resonance         = SEQ_CLAMP_F32(f->resonance,  0.51f, 8.0f);
    tmp.filter_env_amount = SEQ_CLAMP_F32(f->filter_env_amount, -8.0f, 8.0f);
    tmp.feedback          = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
    melodic_filter_apply(layer_idx, track, &tmp);
}

bool sequencer_core_melodic_env_authored(uint8_t layer_idx, uint8_t track,
                                         uint8_t eg_index)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    const voice_params_t *vp = &s_layers[layer_idx].vp[track];
    return (eg_index == 1) ? vp->env1_authored : vp->env_authored;
}

bool sequencer_core_melodic_filter_authored(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    return s_layers[layer_idx].vp[track].filter_authored;
}

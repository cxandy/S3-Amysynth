#include "sequencer_core/seq_core_internal.h"

/* ── State definitions — owns LFO phase accumulators ────────────────── */
/* Software LFO state (phase accumulator, per-layer/track) */
float    s_lfo_phase[MAX_LAYERS][SEQ_TRACKS]; /* 0..1 normalized */
float    s_lfo_hz[MAX_LAYERS][SEQ_TRACKS];    /* Hz from rate+BPM */
float    s_lfo_rnd[MAX_LAYERS][SEQ_TRACKS];   /* S&H held value   */
uint32_t s_lfo_rng_state = 0xDEADBEEFu;

/* ── Per-row melodic envelope (runtime-editable) ─────────────────────────── */

/* Push the given row's stored envelope to that row's OWN AMY synth. */
void sequencer_configure_melodic_envelope_track(uint8_t layer_idx, uint8_t track)
{
#if CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED
    const seq_layer_t *layer = &s_layers[layer_idx];
    const seq_env_t   *env   = seq_layer_env(layer_idx, track);
    float sustain = (float)env->sustain_pct / 100.0f;
    uint32_t attack_ms = env->attack_ms;
    /* KS and NOISE excite via an onset transient; an attack ramp suppresses it. */
    if (layer->patch == SEQ_PATCH_KS || layer->patch == SEQ_PATCH_NOISE) {
        attack_ms = 2;  /* force floor */
    }

    amy_event *e = amy_helpers_event_begin();
    e->synth = layer->synth_id[track];
    e->bp_is_set[0] = 1;
    e->eg_type[0] = env->eg_type;
    e->eg0_times[0] = attack_ms;
    e->eg0_values[0] = 1.0f;
    e->eg0_times[1] = env->decay_ms;
    e->eg0_values[1] = sustain;
    e->eg0_times[2] = env->release_ms;
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

/* ── Per-row melodic filter (runtime-editable) ─────────────────────────── */

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
    } else {
        e->filter_type = FILTER_NONE;
    }
    amy_helpers_event_send(e);
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

    layer->filter_authored[track] = true;
    sequencer_configure_melodic_filter_track(layer_idx, track);
    ESP_LOGI(TAG, "filter L%u row%u -> type%u %.0fHz Q%.2f (authored)",
             layer_idx, track, dst->filter_type,
             (double)dst->cutoff_hz, (double)dst->resonance);
}

/* Generic filter push: shared by arp, drone (via synth_ui). */
void sequencer_core_push_filter(uint8_t synth, const seq_filter_t *f)
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

    if (!lfo->enabled) {
        /* Restore target to its stored static value rather than a hardcoded
         * constant — FILTER in particular has a user-set cutoff that must
         * survive enable/disable round-trips. */
        if (lfo->target == LFO_TARGET_FILTER) {
            sequencer_core_push_filter(layer->synth_id[track], &layer->filter[track]);
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

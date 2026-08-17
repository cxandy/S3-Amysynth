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

/* True when an authored track should use the AMY native LFO. Caller must
 * already know the layer's patch reserves a carrier pair. PAN rides
 * pan_coefs[COEF_MOD] around a 0.5 baseline and RANDOM maps to a native NOISE
 * S&H carrier, so every enabled LFO on such a patch is native; the 20 Hz
 * software poll serves the rest. */
static bool is_native_lfo_track(const seq_lfo_t *lfo)
{
    return lfo->enabled;
}

/* Push native LFO config to one track's AMY synth (patches with a reserved
 * carrier pair). Handles activation and deactivation (carrier dormant,
 * COEF_MOD cleared) so the caller always reaches a consistent AMY state. */
/* Apply an EXPLICIT lfo struct (stored or an editor's live scratch) to one
 * track's native topology; the wrapper below keeps the stored-state callers
 * unchanged. */
static void melodic_native_lfo_apply(const seq_layer_t *layer, uint8_t track,
                                     const seq_lfo_t *lfo)
{
    uint8_t carrier, coupled;
    if (!sequencer_core_lfo_native_layout(layer->patch, &carrier, &coupled))
        return;
    voice_apply_native_lfo_topo(layer->synth_id[track],
                                is_native_lfo_track(lfo) ? lfo : NULL, s_bpm,
                                carrier, coupled);
}

static void melodic_configure_native_lfo_track(const seq_layer_t *layer, uint8_t track)
{
    melodic_native_lfo_apply(layer, track, &layer->vp[track].lfo);
}

#endif /* CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO */

/* Restore the resting coefficient for every target the LFO was driving, so
 * nothing stays modulated after it is switched off. FILTER restores the
 * track's authored cutoff; the rest push a neutral constant. */
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
     * wave. A forced onset floor or zeroed KS sustain makes those patches decay
     * to silence regardless of what the user authored. */
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

    /* Committing establishes this row's authority over the patch's own
     * envelope: later patch changes re-impose it. Each row owns its own synth,
     * so the push affects only this row. */
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

/* Push the row's stored EG1 to its own AMY synth, osc 0 - the same oscillator
 * EG0 targets. No KS/NOISE special case: EG1 has no role for those waves. */
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

/* LEGACY SNAPSHOT IMPORT ONLY. Older project files had no feedback field: KS
 * string decay was derived from filter Q. This [0.51,8.0] -> [0,1] mapping
 * exists so de_filter() can reconstruct what those files audibly had. Live
 * apply paths use seq_filter_t.feedback directly. */
float sequencer_core_ks_feedback_from_q(float q)
{
    float n = (q - 0.51f) / (8.0f - 0.51f);
    return SEQ_CLAMP_F32(n, 0.0f, 1.0f);
}

/* Apply one filter config to a row's AMY synth. Shared by the stored-state
 * configure below and the live-preview push: the caller decides whether `f` is
 * the row's committed filter or an editor's scratch copy. */
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
        /* EG1 -> cutoff depth in octaves, wired only for a non-zero amount so a
         * plain melodic filter is unchanged. Nothing routes COEF_EG1 to
         * amplitude on melodic tracks, so there is no double-use to arbitrate
         * and the amount==0 gate is the only guard needed (as in arp_core.c). */
        if (f->filter_env_amount != 0.0f) {
            e->filter_freq_coefs[COEF_EG1] = f->filter_env_amount;
        }
    } else {
        e->filter_type = FILTER_NONE;
    }
    /* KS string decay: the authored feedback, pushed directly. 0 = never
     * authored, so leave AMY's build-time 0.9 default in place. */
    if (layer->patch == SEQ_PATCH_KS && f->feedback > 0.0f) {
        e->feedback = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
    }
    amy_helpers_event_send(e);

    /* Guarantee valid EG1 breakpoints whenever the filter env is live, so
     * filter_freq_coefs[COEF_EG1] modulates a real ramp: AMY treats a
     * never-configured breakpoint set as a permanent 1.0. Uses the row's stored
     * EG1 (authored shape or the zeroed default). */
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
    dst->filter_type = (f->filter_type < SEQ_FILTER_COUNT) ? f->filter_type : FILTER_NONE;
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

/* ── Per-row melodic distortion ──────────────────────────────────────────
 * Parallel to the filter, minus the service loop: distortion has no software
 * fallback path, so a push goes straight to the row's synth and there is
 * nothing for lfo_service to arbitrate. Preview and commit therefore differ
 * only in whether the store is written. */

bool sequencer_core_get_melodic_dist(uint8_t layer_idx, uint8_t track,
                                     seq_dist_t *out)
{
    if (!out || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return false;
    *out = s_layers[layer_idx].vp[track].dist;
    return true;
}

void sequencer_core_set_melodic_dist(uint8_t layer_idx, uint8_t track,
                                     const seq_dist_t *d)
{
    if (!d || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    layer->vp[track].dist = *d;
    voice_dist_clamp(&layer->vp[track].dist);
    layer->vp[track].dist_authored = true;
    voice_apply_dist(layer->synth_id[track], &layer->vp[track].dist);
    ESP_LOGI(TAG, "dist L%u T%u -> type%u drv%u bit%u rte%u mix%u (authored)",
             layer_idx + 1u, track + 1u, layer->vp[track].dist.type,
             layer->vp[track].dist.drive, layer->vp[track].dist.bits,
             layer->vp[track].dist.rate, layer->vp[track].dist.mix);
}

void sequencer_core_preview_melodic_dist(uint8_t layer_idx, uint8_t track,
                                         const seq_dist_t *d)
{
    if (!d || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    voice_apply_dist(s_layers[layer_idx].synth_id[track], d);
}

void sequencer_core_reapply_melodic_dist(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    voice_apply_dist(s_layers[layer_idx].synth_id[track],
                     &s_layers[layer_idx].vp[track].dist);
}

/* Re-assert a row's stored distortion after anything that rebuilds its voice
 * (patch load, layer reload). Without this the stage is silently dropped, the
 * same failure the filter/LFO re-pushes exist for. */
void sequencer_configure_melodic_dist_track(uint8_t layer_idx, uint8_t track)
{
    sequencer_core_reapply_melodic_dist(layer_idx, track);
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
    /* KS string decay from the authored feedback; 0 = never authored, so keep
     * AMY's build-time 0.9 default. */
    if (is_ks && f->feedback > 0.0f) {
        e->feedback = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
    }
    amy_helpers_event_send(e);
}

/* Active editor-preview slot; contract at the "Live-preview pushes" section
 * below. WRITTEN by the editor handlers (encoder/button tasks), READ by
 * sequencer_core_lfo_service() on synth_ui_task - same core, same priority
 * tier, so a round-robin boundary can land mid-write. seq_core is lock-free
 * by discipline, so consistency is a generation counter (seqlock): writers
 * bump to odd, mutate, bump to even; the reader snapshots once per service
 * tick and falls back to the store if it caught a writer. An aligned 32-bit
 * counter is single-copy atomic on Xtensa. */
typedef struct {
    bool         active;
    uint8_t      li, tr;
    bool         filter_valid;
    seq_filter_t filter;
    bool         lfo_valid;
    seq_lfo_t    lfo;
} melodic_preview_t;
static melodic_preview_t s_preview;
static volatile uint32_t s_preview_gen;   /* odd = writer mid-update */

/* Re-point the slot at (li, tr), dropping stale scratch from another track.
 * Callers hold the write side (odd s_preview_gen). */
static void preview_slot_touch(uint8_t li, uint8_t tr)
{
    if (!s_preview.active || s_preview.li != li || s_preview.tr != tr) {
        s_preview = (melodic_preview_t){0};
        s_preview.active = true;
        s_preview.li = li;
        s_preview.tr = tr;
    }
}

/* Consistent snapshot for the service loop; returns false when no preview is
 * active or a writer was mid-update (caller then reads the store as usual -
 * one 50 ms tick of committed values, self-healing). */
static bool preview_snapshot(melodic_preview_t *out)
{
    uint32_t g0 = s_preview_gen;
    if (g0 & 1u) return false;
    *out = s_preview;
    if (s_preview_gen != g0) return false;
    return out->active;
}

/* Push an EXPLICIT lfo struct to one track's engine state (native topology or
 * software-service arming) without touching the store. Shared by the
 * committing setter (which passes the just-stored struct), the live preview
 * (editor scratch) and cancel-restore (stored struct). */
static void melodic_lfo_apply_runtime(uint8_t layer_idx, uint8_t track,
                                      const seq_lfo_t *lfo)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    /* Native topology is melodic-only: it writes voice-relative oscs 1 and 2,
     * and AMY applies the base_osc offset without a bounds check, so on a
     * 1-osc-per-voice synth (a PCM drum slot) those events would land on the
     * NEXT synth's oscillators. A drum layer's `patch` is only the stored
     * SYNTH-mode selection, so it must never enable the native path. */
    bool is_native = layer->type == SEQ_LAYER_MELODIC &&
                     sequencer_core_lfo_native_layout(layer->patch, NULL, NULL);
    if (is_native) {
        melodic_native_lfo_apply(layer, track, lfo);
        /* Restore the static target value when disabled: native clears COEF_MOD
         * but does not push the neutral coef. */
        if (!lfo->enabled || !is_native_lfo_track(lfo)) {
            lfo_restore_target_neutrals(layer, track, lfo);
        }
        /* s_lfo_hz = 0 makes the software service loop skip native tracks. */
        s_lfo_hz[layer_idx][track] = (lfo->enabled && is_native_lfo_track(lfo))
                                     ? 0.0f
                                     : (lfo->enabled ? seq_lfo_sw_hz(lfo->rate, s_bpm) : 0.0f);
        return;
    }
#endif
    /* Software path: patches without a carrier pair, or native LFO compiled out. */
    if (!lfo->enabled) {
        lfo_restore_target_neutrals(layer, track, lfo);
        s_lfo_hz[layer_idx][track] = 0.0f;
    } else {
        s_lfo_hz[layer_idx][track] = seq_lfo_sw_hz(lfo->rate, s_bpm);
    }
}

void sequencer_core_set_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    const seq_lfo_t *lfo)
{
    if (!lfo || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    layer->vp[track].lfo = *lfo;
    if (layer->vp[track].lfo.depth > 100) layer->vp[track].lfo.depth = 100;
    layer->vp[track].lfo_authored = true;

    melodic_lfo_apply_runtime(layer_idx, track, &layer->vp[track].lfo);
    ESP_LOGI(TAG, "LFO L%u T%u %s %.2f Hz d=%u tgt=0x%02x",
             layer_idx + 1u, track + 1u, lfo->enabled ? "ON" : "OFF",
             (double)s_lfo_hz[layer_idx][track], lfo->depth, lfo->targets);
}

/* Cancel-restore for the LFO live preview: re-push the stored (committed)
 * state. Safe on a never-authored row - the zeroed default is disabled with
 * an empty target set, so the restore is a no-op push. */
void sequencer_core_reapply_melodic_lfo(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    melodic_lfo_apply_runtime(layer_idx, track, &s_layers[layer_idx].vp[track].lfo);
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
    /* One consistent preview snapshot per service tick: an open editor's
     * scratch overrides the store for its one track, so relational edits are
     * audible live instead of being stomped back to committed values. */
    melodic_preview_t prev;
    bool prev_ok = preview_snapshot(&prev);
    for (int li = 0; li < s_num_layers; li++) {
        for (int tr = 0; tr < SEQ_TRACKS; tr++) {
            bool prev_here = prev_ok &&
                             prev.li == (uint8_t)li &&
                             prev.tr == (uint8_t)tr;
            bool lfo_previewed = prev_here && prev.lfo_valid;
            if (!lfo_previewed && !s_layers[li].vp[tr].lfo_authored) continue;
            const seq_lfo_t *lfo = lfo_previewed ? &prev.lfo
                                                 : &s_layers[li].vp[tr].lfo;
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
            /* Multi-target: each checked target modulates its own COEF_CONST
             * from the same LFO value. SCAN has no software analog - it needs a
             * wavetable voice, which always takes the native path. */
            if (LFO_HAS_TGT(lfo, LFO_TARGET_FILTER)) {
                const seq_filter_t *fb = (prev_here && prev.filter_valid)
                                         ? &prev.filter
                                         : &s_layers[li].vp[tr].filter;
                float base = (fb->enabled && fb->cutoff_hz > 0.0f)
                             ? fb->cutoff_hz : 1000.0f;
                e->filter_freq_coefs[COEF_CONST] =
                    base * powf(2.0f, voice_lfo_filter_octaves(lfo) * val);
            }
            if (LFO_HAS_TGT(lfo, LFO_TARGET_AMP))
                e->amp_coefs[COEF_CONST] = 1.0f - d*(0.5f - 0.5f*val);
            if (LFO_HAS_TGT(lfo, LFO_TARGET_PAN))
                e->pan_coefs[COEF_CONST] = 0.5f + d*0.5f*val;
            amy_helpers_event_send(e);

            if (LFO_HAS_TGT(lfo, LFO_TARGET_PITCH)) {
                /* freq COEF_CONST is an ABSOLUTE frequency in Hz - AMY maps it
                 * through logfreq_of_freq(x) = log2(x/440). Anchoring the swing
                 * at SEQ_LFO_PITCH_BASE_HZ makes the constant term exactly
                 * d*val octaves, matching the note-neutral reset default of 0.
                 * (A bare ratio here lands ~-8.8 octaves down and mutes the
                 * track - sub-audible playback rate on PCM oscs.)
                 *
                 * Pitch is pushed to osc 0 ONLY: a synth-wide event fans out to
                 * every osc of the voice, rewriting patch-internal modulator
                 * oscs' freq CONST - their RATE - and wrecking patch LFOs
                 * (chorus/PWM) beyond repair short of a patch reload. Stopgap:
                 * multi-carrier patches get vibrato on their first osc only;
                 * revisit with a per-voice offset or a reserved-carrier topo. */
                e = amy_helpers_event_begin();
                e->synth = syn;
                e->osc   = 0;
                e->freq_coefs[COEF_CONST] =
                    SEQ_LFO_PITCH_BASE_HZ * powf(2.0f, d * VOICE_LFO_DEPTH_PITCH * val);
                amy_helpers_event_send(e);
            }
        }
    }
}

/* ── Native LFO rebuild helpers ─────────────────────────────────────────────
 * Called after a patch/synth rebuild and after BPM changes, so native LFO
 * carrier state stays consistent with the layer's patch and tempo. */

/* Re-apply the authored native LFO for every track in a layer whose patch
 * reserves a carrier pair. No-op otherwise - the software loop handles those. */
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

/* Update the carrier frequency on all active native-LFO tracks after a BPM
 * change. Mirrors arp_core_refresh_lfo_freq(). */
void melodic_lfo_refresh_native_freq(void)
{
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    for (int li = 0; li < s_num_layers; li++) {
        const seq_layer_t *layer = &s_layers[li];
        uint8_t carrier;
        /* Melodic-only, same reason as sequencer_core_set_melodic_lfo(): the
         * carrier oscs don't exist on non-melodic (1-osc) synths. */
        if (layer->type != SEQ_LAYER_MELODIC)
            continue;
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
 * A per-track multiplier on note velocity at emit time. Default 1.0, which
 * add_layer must set explicitly since memset zeroes the struct.
 *
 * The setter re-emits the track's steps: steps are scheduled ahead of time with
 * a period and are not re-emitted per tick, so a store-only change would stay
 * silent until some unrelated re-emit. */

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
 * Editors audition scratch values while the committed store stays the source of
 * truth. Cancel re-pushes the stored state (or reloads the layer's patch for a
 * never-authored row); confirm goes through the normal setters. A preview never
 * modifies the authored flags.
 *
 * The single active-preview slot below is what makes RELATIONAL edits
 * composable: the software-LFO service recomputes swept COEF_CONST values
 * every 50 ms from the model, so without it an uncommitted cutoff or LFO
 * scratch would be stomped back toward the store on the next tick (or, for
 * the LFO, not heard at all until commit). Editors register their scratch
 * here; the service prefers it over the store for the matching track. One
 * slot suffices - only one editor is open at a time, and filter/EG1/LFO
 * previews for the SAME track compose (each keeps its own valid flag).
 * (Slot definitions live above the service loop, which reads them.) */

void sequencer_core_preview_melodic_clear(void)
{
    s_preview_gen++;
    s_preview = (melodic_preview_t){0};
    s_preview_gen++;
}

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
    /* Same clamps as the committing setter, so the audition matches what
     * confirm would store. */
    seq_filter_t tmp = *f;
    tmp.filter_type       = (f->filter_type < SEQ_FILTER_COUNT) ? f->filter_type : FILTER_NONE;
    tmp.cutoff_hz         = SEQ_CLAMP_F32(f->cutoff_hz,  65.0f, 8000.0f);
    tmp.resonance         = SEQ_CLAMP_F32(f->resonance,  0.51f, 8.0f);
    tmp.filter_env_amount = SEQ_CLAMP_F32(f->filter_env_amount, -8.0f, 8.0f);
    tmp.feedback          = SEQ_CLAMP_F32(f->feedback, 0.0f, 1.0f);
    /* Register the scratch so the software-LFO service sweeps around the
     * in-progress cutoff instead of stomping it from the store. */
    s_preview_gen++;
    preview_slot_touch(layer_idx, track);
    s_preview.filter       = tmp;
    s_preview.filter_valid = true;
    s_preview_gen++;
    melodic_filter_apply(layer_idx, track, &tmp);
}

void sequencer_core_preview_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                        const seq_lfo_t *lfo)
{
    if (!lfo || layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    /* Same clamp as the committing setter. */
    seq_lfo_t tmp = *lfo;
    if (tmp.depth > 100) tmp.depth = 100;

    s_preview_gen++;
    preview_slot_touch(layer_idx, track);
    s_preview.lfo       = tmp;
    s_preview.lfo_valid = true;
    s_preview_gen++;

    /* Native tracks hear the scratch immediately via the carrier topology;
     * software tracks are armed here (rate/phase) and the service loop reads
     * the slot for the rest. s_lfo_hz is runtime pacing, not authored state -
     * cancel restores it via sequencer_core_reapply_melodic_lfo(). */
    melodic_lfo_apply_runtime(layer_idx, track, &tmp);
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

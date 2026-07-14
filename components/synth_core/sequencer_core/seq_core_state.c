#include "sequencer_core/seq_core_internal.h"
#include "voice_config.h"
#include <assert.h>

/* ── State definitions — owns the core layer table and play state ─── */
seq_layer_t s_layers[MAX_LAYERS];
uint8_t     s_num_layers   = 0;
bool        s_playing      = true;
/* Guards sequencer_core_delete_layer()'s ~4 KB s_layers compaction against the
 * Core-0 esp_timer sequencer tick, which reads s_layers[] live. Set true around
 * the structural edit; the tick early-returns while it is set. */
volatile bool s_layers_mutating = false;

/* Single-applier enforcement for structural s_layers edits (add/delete): after
 * boot init registers the applier (synth_ui_task, which drains the deferred
 * add/delete requests), debug builds assert every structural edit runs on that
 * task. This is the durable form of the s_layers_mutating tick guard above —
 * the guard fences the one concurrent READER (the esp_timer tick); this pins
 * all WRITERS to one task by construction, the same discipline
 * s_prog_apply_pending uses for chord edits (seq_core_progression.c). */
static TaskHandle_t s_layers_applier = NULL;

void sequencer_core_set_layers_applier(TaskHandle_t applier)
{
    s_layers_applier = applier;
}

/* Skipped until a handle is registered, so single-threaded boot init passes. */
static inline void seq_assert_layers_applier(void)
{
#if !defined(NDEBUG)
    configASSERT(s_layers_applier == NULL ||
                 xTaskGetCurrentTaskHandle() == s_layers_applier);
#endif
}
/* Default seed + display fallback, NOT an authoritative global — dual-writer
 * contract documented at the declaration in seq_core_internal.h. */
uint16_t    s_melodic_patch = SEQ_MEL_PATCH;
/* Running allocator for per-row melodic synth slots. Each melodic layer claims
 * a contiguous block of SEQ_TRACKS slots starting here; reset in core_init. */
uint8_t     s_next_melodic_synth = SEQ_MEL_SYNTH_BASE;

/* Default per-track SYNTH patches by role: kick, snare/clap, hat, perc. Indices
 * into nothing — these are raw patch numbers chosen for a 4-on-floor kit. */
static const uint16_t SEQ_DRUM_DEFAULT_PATCH[SEQ_TRACKS] = {
    58,   /* track 0: kick  — Juno Drum Booms at a low pitch = thumpy body */
    245,  /* track 1: snare — DX7 B.DRM-SNAR                              */
    221,  /* track 2: hat   — DX7 BLOCK at high pitch = tight tick        */
    220,  /* track 3: perc  — DX7 COW BELL accent                        */
};

/* Role-based default pitches: pitch IS timbre for these tuned patches.
 * Low kick body, mid snare, high hat tick, mid-high perc. */
static const uint8_t SEQ_DRUM_DEFAULT_NOTE[SEQ_TRACKS] = {
    39,   /* track 0: kick  — Eb2 = 808-KIK root (natural punch)   */
    45,   /* track 1: snare — A2  = 808-SNR root (natural crack)    */
    53,   /* track 2: hat   — F3  = 808-C-HAT root (natural tick)   */
    82,   /* track 3: clap  — Bb5 = 808-DRYCLP root-12 (full snap)  */
};

void sequencer_core_init(void)
{
    amy_helpers_init();
    s_num_layers = 0;
    s_next_melodic_synth = SEQ_MEL_SYNTH_BASE;
    memset(s_layers, 0, sizeof(s_layers));
    memset(s_cached_step, 0, MAX_LAYERS * sizeof(s_cached_step[0]));
    memset(s_track_source_note, 0, MAX_LAYERS * SEQ_TRACKS * sizeof(s_track_source_note[0][0]));
    s_playing = true;
    s_bpm     = SEQ_DEFAULT_BPM;
    s_melodic_patch = SEQ_MEL_PATCH;
    s_quantizer.enabled = CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED;
    s_quantizer.root_note = CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE;
    s_quantizer.scale_index = CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE;
    if (s_quantizer.scale_index >= quantizer_scale_count()) {
        s_quantizer.scale_index = 0;
    }
    memset(s_lfo_phase, 0, MAX_LAYERS * SEQ_TRACKS * sizeof(s_lfo_phase[0][0]));
    memset(s_lfo_hz,    0, MAX_LAYERS * SEQ_TRACKS * sizeof(s_lfo_hz[0][0]));
    memset(s_lfo_rnd,   0, MAX_LAYERS * SEQ_TRACKS * sizeof(s_lfo_rnd[0][0]));
    CORE_HEAP_CHECK("core_init: before push_tempo");
    sequencer_push_tempo(s_bpm);
    CORE_HEAP_CHECK("core_init: after push_tempo");
    ESP_LOGI(TAG, "sequencer_core initialized");
}

uint8_t sequencer_core_add_layer(seq_layer_type_t type, uint8_t num_steps)
{
    seq_assert_layers_applier();
    if (s_num_layers >= MAX_LAYERS) {
        ESP_LOGW(TAG, "sequencer_core_add_layer: max layers (%d) reached", MAX_LAYERS);
        return 0xFF;
    }
    /* Claim the slot index but do NOT expose it via s_num_layers yet.
     * The tick path iterates 0..s_num_layers-1; incrementing here would let
     * the tick see a half-initialised layer.  s_num_layers++ is deferred to
     * after sequencer_configure_synth() completes below. */
    uint8_t idx = s_num_layers;
    seq_layer_t *layer = &s_layers[idx];
    memset(layer, 0, sizeof(seq_layer_t));

    /* Per-row voice params: zeroed/unauthored with amp_trim at unity — the
     * single defaults source, so no field here needs post-memset repair. */
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        voice_params_init_defaults(&layer->vp[t]);
    }

    layer->type       = type;
    layer->num_steps  = (num_steps == SEQ_MAX_STEPS) ? SEQ_MAX_STEPS : SEQ_STEPS;
    layer->num_tracks = SEQ_TRACKS;
    layer->step_page  = 0;

    if (type == SEQ_LAYER_DRUM) {
        /* Drums are now a per-track Juno-patch layer: each track gets its own
         * fixed synth slot (block 6..9) and its own patch from the curated list,
         * with note-offs honored (synth_flags = 0) so the patch's own release
         * envelope shapes the tail. */
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            layer->synth_id[t]   = (uint8_t)(SEQ_DRUM_SYNTH_BASE + t);
            /* Role-based default patch per track (kick/snare/hat/perc). */
            layer->track_patch[t] = SEQ_DRUM_DEFAULT_PATCH[t];
        }
        layer->patch       = layer->track_patch[0];  /* display fallback */
        layer->synth_flags = 0;
        layer->num_voices  = SEQ_DRUM_VOICES;
        /* Role-based pitches: pitch IS timbre for these tuned patches AND tunes
         * the 808 samples in PCM mode (render_pcm shifts by midi_note). Low kick
         * body, mid snare, high hat tick, mid-high perc; stays user-editable. */
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            s_track_source_note[idx][t] = SEQ_DRUM_DEFAULT_NOTE[t];
            layer->track_base_note[t] = SEQ_DRUM_DEFAULT_NOTE[t];
            for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = SEQ_DRUM_DEFAULT_NOTE[t];
            }
        }
    } else {
        /* Melodic: claim a contiguous block of SEQ_TRACKS synth slots from the
         * running allocator, one synth per row, so identical pitches on
         * different rows land in distinct instruments (no voice collapse).
         * Guard against exceeding AMY's synth ceiling; if we would, reuse the
         * last valid block (degrades to shared-synth rather than corruption). */
        uint8_t base = s_next_melodic_synth;
        if (base + SEQ_TRACKS - 1 > SEQ_MAX_SYNTH) {
            base = (uint8_t)(SEQ_MAX_SYNTH - (SEQ_TRACKS - 1));
            ESP_LOGW(TAG, "add_layer[L%d]: melodic synth ceiling reached, "
                          "reusing slots %u..%u", idx + 1, base, base + SEQ_TRACKS - 1);
        } else {
            s_next_melodic_synth = (uint8_t)(base + SEQ_TRACKS);
        }
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            layer->synth_id[t] = (uint8_t)(base + t);
        }
        layer->patch       = s_melodic_patch;
        layer->synth_flags = 0;
        layer->num_voices  = SEQ_MEL_VOICES;
        /* Default: Cmaj7 voicing — C4 E4 G4 B4 */
        static const uint8_t mel_notes[SEQ_TRACKS] = {60, 64, 67, 71};
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            s_track_source_note[idx][t] = mel_notes[t];
            layer->track_base_note[t] = mel_notes[t];
            for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = mel_notes[t];
            }
            layer->vp[t].env  = seq_default_melodic_env();
            layer->vp[t].env1 = seq_default_melodic_env1();
        }
    }

    /* step_prob (0% would silence every step) and step_ratchet (0 sub-hits
     * would fire nothing) need explicit non-zero defaults; step_cond_type/
     * param and the per-step transform/quant-bypass arrays default correctly
     * to their zeroed no-op (SEQ_STEP_COND_NONE / SEQ_STEP_TRANSFORM_NONE /
     * no bypass) and need no explicit init. The per-row amp trim got its
     * unity default in voice_params_init_defaults() above. */
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        for (uint8_t s = 0; s < SEQ_MAX_STEPS; s++) {
            layer->step_prob[t][s]    = 100;
            layer->step_ratchet[t][s] = 1;
        }
    }

    CORE_HEAP_CHECK("add_layer: before configure_synth");
    sequencer_configure_synth(idx);
    CORE_HEAP_CHECK("add_layer: after configure_synth");
    /* Layer is fully initialised: now expose it to the tick path. */
    s_num_layers++;
    /* Construction-time check of the permanent-drum-layer invariant: slot 0
     * is the drum layer (created first at boot) and every later add lands
     * above it. Verified once here rather than trusted throughout. */
    assert(s_layers[SEQ_DRUM_LAYER_IDX].type == SEQ_LAYER_DRUM);
    /* Runtime trig bookkeeping (loop counters / last-played / last-step-seen)
     * is indexed by layer slot, not layer identity; a wholesale reset here
     * (rather than trying to shift it in lockstep with the new layer) is
     * cheap and harmless — it only affects FILL/PREV continuity, never the
     * persisted per-step prob/ratchet/cond data carried in seq_layer_t itself. */
    sequencer_core_trig_reset_all();
    ESP_LOGI(TAG, "add_layer[L%d]: type=%d synth0=%d patch=%d steps=%d",
             idx + 1, type, layer->synth_id[0], layer->patch, layer->num_steps);
    return idx;
}

bool sequencer_core_delete_layer(uint8_t layer_idx)
{
    seq_assert_layers_applier();
    if (s_num_layers <= 1) return false;                  /* must keep at least 1 */
    if (layer_idx == SEQ_DRUM_LAYER_IDX) return false;    /* drum layer is permanent */
    if (layer_idx >= s_num_layers) return false;
    if (s_layers[layer_idx].type == SEQ_LAYER_DRUM) return false;

    /* Clear ALL layers' tags before shifting — indices above layer_idx become
     * stale after compaction and would fire as ghost notes. Ratchet one-shot
     * tags live in a separate tag space (seq_core_trig.c) and need the same
     * treatment. */
    for (uint8_t i = 0; i < s_num_layers; i++) {
        sequencer_clear_layer_tags(i);
        sequencer_core_trig_clear_all(i);
    }

    /* Release AMY oscillator slots for the deleted layer. */
    const seq_layer_t *dead = &s_layers[layer_idx];
    for (uint8_t t = 0; t < dead->num_tracks; t++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth      = dead->synth_id[t];
        e->num_voices = 0;
        amy_helpers_event_send(e);
    }

    /* Raise the mutation guard so the Core-0 sequencer tick early-returns
     * instead of reading s_layers[] mid-memmove. The tick tolerates skipped
     * ticks (trig state is reset wholesale below), so this is inaudible. */
    s_layers_mutating = true;
    /* Compact all parallel arrays by shifting survivors down by one slot. */
    uint8_t tail = (uint8_t)(s_num_layers - layer_idx - 1);
    if (tail > 0) {
        memmove(&s_layers[layer_idx],
                &s_layers[layer_idx + 1],
                tail * sizeof(s_layers[0]));
        memmove(&s_cached_step[layer_idx],
                &s_cached_step[layer_idx + 1],
                tail * sizeof(s_cached_step[0]));
        memmove(&s_track_source_note[layer_idx],
                &s_track_source_note[layer_idx + 1],
                tail * sizeof(s_track_source_note[0]));
        memmove(&s_lfo_phase[layer_idx],
                &s_lfo_phase[layer_idx + 1],
                tail * sizeof(s_lfo_phase[0]));
        memmove(&s_lfo_hz[layer_idx],
                &s_lfo_hz[layer_idx + 1],
                tail * sizeof(s_lfo_hz[0]));
        memmove(&s_lfo_rnd[layer_idx],
                &s_lfo_rnd[layer_idx + 1],
                tail * sizeof(s_lfo_rnd[0]));
    }
    s_num_layers--;
    /* Table is fully compacted and the count updated; drop the guard. */
    s_layers_mutating = false;
    sequencer_core_trig_reset_all();  /* see rationale in sequencer_core_add_layer() */

    /* Resync all surviving layers so their note tags re-register correctly. */
    if (s_playing) {
        for (uint8_t i = 0; i < s_num_layers; i++) {
            sequencer_resync_layer(i);
        }
    }

    ESP_LOGI(TAG, "delete_layer[L%u]: %u layers remain", layer_idx + 1u, s_num_layers);
    return true;
}

uint8_t sequencer_core_get_num_layers(void)
{
    return s_num_layers;
}

seq_layer_type_t sequencer_core_get_layer_type(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return SEQ_LAYER_DRUM;
    return s_layers[layer_idx].type;
}

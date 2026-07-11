#include "sequencer_core/seq_core_internal.h"
#include "voice_config.h"

/* ── State definitions — owns drum engine selector ──────────────────── */
/* Drum sound source for the whole drum layer. SYNTH = tonal AMY patches (Juno/
 * DX7) per track; PCM = built-in 808 samples per track. Switchable at runtime;
 * changing it re-configures the drum layer's synth slots in place. */
seq_drum_engine_t s_drum_engine = SEQ_DRUM_PCM;

/* Curated drum-patch cycle list for SYNTH (tonal-patch) drum mode. The per-track
 * patch-select control steps through this list (wraps); it is NOT the full
 * 0..256 range. Now spans both banks: the DX7 idiophones (BLOCK/LOG DRUM/COW
 * BELL/MARIMBA/etc.) have far sharper attack + shorter decay than the Juno
 * "drum" patches, so they read as real percussion. Pitch still drives timbre
 * (low = body/kick, high = hat/shaker) — see role-based defaults below. */
static const uint16_t SEQ_DRUM_PATCH_LIST[] = {
    245,  /* DX7 B.DRM-SNAR  — dedicated bass-drum/snare, closest to a kit voice */
    221,  /* DX7 BLOCK       — woodblock, tight click (hat/rim)                  */
    223,  /* DX7 LOG DRUM    — tuned tom/perc, musical                          */
    220,  /* DX7 COW BELL    — metallic accent                                  */
    149,  /* DX7 MARIMBA     — clean mallet (melodic perc / blips)              */
    215,  /* DX7 XYLOPHONE   — bright mallet (hat-ish at high pitch)            */
    148,  /* DX7 VIBE 1      — soft mallet (ghost notes)                        */
    219,  /* DX7 BELLS       — bell accent                                      */
    58,   /* Juno Drum Booms — boomy low (kick body at low pitch)               */
    61,   /* Juno Hand Claps — clap                                             */
    46,   /* Juno Shaker     — shaker/hat texture                               */
    70,   /* Juno Perc Pluck — plucky perc                                      */
};
#define SEQ_DRUM_PATCH_COUNT ((int)(sizeof(SEQ_DRUM_PATCH_LIST) / sizeof(SEQ_DRUM_PATCH_LIST[0])))

/* Patch-cycle domain for the curated drum list (never full-range). */
static const patch_domain_t s_drum_domain = {
    .list = SEQ_DRUM_PATCH_LIST, .count = SEQ_DRUM_PATCH_COUNT, .full_max = 0
};

/* Built-in 808 PCM sample indices used by PCM drum mode, one per track:
 * kick, snare, closed-hat, clap. The compiled-in ROM bank (and with it the
 * preset numbering) follows CONFIG_AMY_PCM_GAMMA808: the gamma808 TR-808
 * bank (amy/src/pcm_gamma808.h pcm_map[]) vs the legacy tiny set
 * (amy/src/pcm_tiny.h pcm_map[]). */
#if CONFIG_AMY_PCM_GAMMA808
static const int16_t SEQ_DRUM_PCM_PRESET[SEQ_TRACKS] = {
    2,    /* track 0: [2] TR-808 Bass Drum 3 (punchy) */
    12,   /* track 1: [12] TR-808 Snare 1     */
    9,    /* track 2: [9] TR-808 HiHat Closed */
    3,    /* track 3: [3] TR-808 Clap         */
};
#else
static const int16_t SEQ_DRUM_PCM_PRESET[SEQ_TRACKS] = {
    1,    /* track 0: [1] 808-KIK 4-D    */
    2,    /* track 1: [2] 808-SNR 4-D    */
    6,    /* track 2: [6] 808-C-HAT1-D   */
    9,    /* track 3: [9] 808-DRYCLP-D   */
};
#endif

/* Per-layer, per-track PCM preset override. Defaults (lazily) to
 * SEQ_DRUM_PCM_PRESET; sequencer_core_set_drum_pcm_preset() is the only way
 * to change an entry, letting a runtime-recorded sample (custompatches/
 * sample_rec) take over one track's slot without a shared-struct field. */
static uint16_t s_drum_pcm_preset[MAX_LAYERS][SEQ_TRACKS];
static bool     s_drum_pcm_preset_init[MAX_LAYERS];

static uint16_t drum_pcm_preset_for(uint8_t layer_idx, uint8_t track)
{
    if (!s_drum_pcm_preset_init[layer_idx]) {
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            s_drum_pcm_preset[layer_idx][t] = (uint16_t)SEQ_DRUM_PCM_PRESET[t];
        }
        s_drum_pcm_preset_init[layer_idx] = true;
    }
    return s_drum_pcm_preset[layer_idx][track];
}

/* EDM-tuned envelope parameters for PCM drum tracks (one-shot decay, sustain=0). */
static const float DRUM_PCM_ATK_MS[SEQ_TRACKS] = {2.0f,  1.0f,  1.0f,  1.0f};
static const float DRUM_PCM_DEC_MS[SEQ_TRACKS] = {600.0f, 200.0f, 100.0f, 150.0f};
static const float DRUM_PCM_REL_MS[SEQ_TRACKS] = {50.0f,  30.0f,  15.0f,  20.0f};

/* ── Private helpers ─────────────────────────────────────────────────── */

/* Apply per-track envelope shape and hat HPF after PCM wave/preset are set. */
static void sequencer_configure_drum_pcm_voice_params(uint8_t layer_idx)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth         = layer->synth_id[t];
        e->bp_is_set[0]  = 1;
        e->eg_type[0]    = ENVELOPE_LINEAR;
        e->eg0_times[0]  = DRUM_PCM_ATK_MS[t];
        e->eg0_values[0] = 1.0f;
        e->eg0_times[1]  = DRUM_PCM_DEC_MS[t];
        e->eg0_values[1] = 0.0f;   /* one-shot: no sustain, sample shapes the body */
        e->eg0_times[2]  = DRUM_PCM_REL_MS[t];
        e->eg0_values[2] = 0.0f;
        amy_helpers_event_send(e);

        if (t == 2) {   /* hat: HPF to strip low-end rumble, add crispness */
            e = amy_helpers_event_begin();
            e->synth      = layer->synth_id[t];
            e->filter_type = FILTER_HPF;
            e->filter_freq_coefs[COEF_CONST] = 3000.0f;
            e->resonance  = 0.5f;
            amy_helpers_event_send(e);
        }
    }
}

void sequencer_kill_synth_voices(uint8_t synth_id)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth    = synth_id;
    e->velocity = 0.0f;
    amy_helpers_event_send(e);
}

/* Configure a single melodic synth slot as a bare AMY oscillator.
 * Mirrors arp_configure_wave_synth() but for melodic tracks (SEQ_MEL_VOICES
 * voices per synth slot).  Envelope, filter, and LFO are applied by the caller.
 *
 * In native LFO mode oscs_per_voice=2 is always allocated (even before an LFO
 * is authored) so that toggling the LFO later never forces a pool resize.
 * Osc 1 starts dormant (amp=0); sequencer_configure_melodic_lfo() activates it
 * when an LFO is authored. */
static void sequencer_configure_melodic_wave_track(uint8_t synth_id,
                                                    uint16_t patch,
                                                    uint16_t num_voices,
                                                    bool filter_authored,
                                                    float filter_q)
{
    static const uint16_t s_wave_for_patch[] = {
        SINE, SAW_DOWN, SAW_UP, PULSE, TRIANGLE, NOISE, KS,
    };
#if CONFIG_AMY_WAVETABLE
    bool is_wavetable = (patch >= SEQ_PATCH_WAVETABLE_BASE && patch <= SEQ_PATCH_WAVETABLE_MAX);
    uint16_t wave = WAVETABLE;
    uint16_t wt_preset = pcm_wavetable_base + (uint16_t)(patch - SEQ_PATCH_WAVETABLE_BASE);
    if (!is_wavetable) {
        uint16_t widx = (uint16_t)(patch - SEQ_PATCH_WAVE_BASE);
        if (widx >= (uint16_t)(sizeof(s_wave_for_patch) / sizeof(s_wave_for_patch[0])))
            widx = 0;
        wave = s_wave_for_patch[widx];
    }
#else
    uint16_t widx = (uint16_t)(patch - SEQ_PATCH_WAVE_BASE);
    if (widx >= (uint16_t)(sizeof(s_wave_for_patch) / sizeof(s_wave_for_patch[0])))
        widx = 0;
    uint16_t wave = s_wave_for_patch[widx];
#endif

    voice_wave_cfg_t cfg = {
        .synth                = synth_id,
        .num_voices           = (uint8_t)num_voices,
#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
        .oscs_per_voice       = 2,   /* osc 1 reserved as native LFO carrier */
#else
        .oscs_per_voice       = 1,
#endif
        .wave                 = wave,
        .osc0_amp_const       = 1.0f,
        .osc0_amp_vel         = 1.0f,
        .ks_feedback_authored = filter_authored,
        .ks_feedback_q        = filter_q,
#if CONFIG_AMY_WAVETABLE
        .wt_preset            = is_wavetable ? (int16_t)wt_preset : -1,
#else
        .wt_preset            = -1,
#endif
    };
    voice_build_wave(&cfg);

#if CONFIG_SEQ_MELODIC_AMY_NATIVE_LFO
    /* osc 1: LFO carrier slot — starts dormant until an LFO is authored */
    amy_event *e = amy_helpers_event_begin();
    e->synth                 = synth_id;
    e->osc                   = 1;
    e->amp_coefs[COEF_CONST] = 0.0f;  /* dormant */
    amy_helpers_event_send(e);
#endif
}

/* Push each AUTHORED row's stored envelope to its own per-row synth.
 * Deferred authority: a row's envelope only overrides the patch's own envelope
 * once the user has committed it in the graph editor (env_authored[t]==true).
 * Unauthored rows are left alone so the freshly-loaded patch envelope plays. */
static void sequencer_configure_melodic_envelope(uint8_t layer_idx)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    /* KS has no patch envelope of its own (it's a raw-wave primitive), so an
     * unauthored row would otherwise keep whatever envelope was last on that
     * synth slot. Force the push so the KS attack/sustain override always
     * lands, regardless of authored state. */
    bool force_ks = (layer->patch == SEQ_PATCH_KS);
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (layer->vp[t].env_authored || force_ks) {
            sequencer_configure_melodic_envelope_track(layer_idx, t);
        }
    }
}

/* Push each AUTHORED row's stored second envelope (EG1) to its own synth.
 * No "force" case (unlike EG0/KS above): EG1 has no raw-wave fallback role,
 * it only matters once a row has actually been authored. */
static void sequencer_configure_melodic_envelope1(uint8_t layer_idx)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (layer->vp[t].env1_authored) {
            sequencer_configure_melodic_envelope1_track(layer_idx, t);
        }
    }
}

/* Push the filter for every authored row in a layer (called after patch reload). */
static void sequencer_configure_melodic_filter(uint8_t layer_idx)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (layer->vp[t].filter_authored) {
            sequencer_configure_melodic_filter_track(layer_idx, t);
        }
    }
}

/* The melodic envelope is stored PER ROW (per track). Each row now owns its own
 * AMY synth slot (synth_id[track]), so every row holds its own independent live
 * envelope — there is no shared synth and no "active row" to arbitrate. This
 * accessor is the single point of truth for "which env applies to (layer,track,
 * step)". For per-step support later, add a step parameter and index a wider
 * env[][] array here — callers stay unchanged. */
seq_env_t *seq_layer_env(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers) layer_idx = 0;
    if (track >= SEQ_TRACKS) track = 0;
    return &s_layers[layer_idx].vp[track].env;
}

/* Second envelope (EG1) counterpart of seq_layer_env() above — same clamping,
 * same single point of truth for "which EG1 applies to (layer,track)". */
seq_env_t *seq_layer_env1(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers) layer_idx = 0;
    if (track >= SEQ_TRACKS) track = 0;
    return &s_layers[layer_idx].vp[track].env1;
}

/* AMY events are emitted through the shared amy_helpers scratch buffer (see
 * amy_helpers.{c,h}) — one module-level event + mutex for all first-party
 * callers, all of which are FreeRTOS tasks (never ISRs). */

/* Apply one routable patch (0..SEQ_PATCH_FULL_MAX) to a synth slot,
 * dispatching per kind — the SINGLE dispatch for every consumer (melodic
 * tracks and the arp; the drone has its own excitation model):
 *   raw wave / wavetable  → direct oscillator config (no patch string)
 *   bass preset (264-266) → bass_preset_configure_track (oscs_per_voice=2)
 *   FM/ALGO (272-276)     → fm preset / live-editable custom voice (7 oscs)
 *   additive (277-279)    → additive preset / custom voice (N+1 oscs)
 *   everything else       → amy_send_patch() string loader (Juno/DX7/piano)
 * Returns true when a patch STRING was loaded: those carry global EQ/chorus
 * commands, so the caller must synth_ui_fx_reassert_global() afterwards
 * (once, even when applying to several slots). */
static bool sequencer_apply_patch_kind(uint8_t synth_id, uint16_t patch,
                                       uint16_t num_voices, uint32_t synth_flags,
                                       bool filter_authored, float filter_q)
{
    /* Virtual patch number whose feature is not in this build (browse skips
     * these, but stored/programmatic values can still arrive): snap to raw
     * SINE rather than letting a virtual number reach the string loader. */
    if (sequencer_core_patch_compiled_out(patch)) {
        sequencer_configure_melodic_wave_track(synth_id, SEQ_PATCH_SINE,
                                               num_voices, filter_authored,
                                               filter_q);
        return false;
    }
    if (sequencer_core_is_wave_patch(patch)) {
        sequencer_configure_melodic_wave_track(synth_id, patch, num_voices,
                                               filter_authored, filter_q);
        return false;
    }
    if (patch >= SEQ_PATCH_BASS_BASE && patch <= SEQ_PATCH_BASS_MAX) {
        bass_preset_configure_track(synth_id, patch, num_voices);
        return false;
    }
#if CONFIG_SYNTH_CUSTOM_FM
    if (patch >= SEQ_PATCH_FM_BASE && patch <= SEQ_PATCH_FM_MAX) {
        if (patch == SEQ_PATCH_FM_CUSTOM) {
            fm_voice_configure_track(synth_id, num_voices, &s_fm_voice);
        } else {
            fm_preset_configure_track(synth_id, patch, num_voices);
        }
        return false;
    }
#endif
#if CONFIG_SYNTH_ADDITIVE
    if (patch >= SEQ_PATCH_ADDITIVE_BASE && patch <= SEQ_PATCH_ADDITIVE_MAX) {
        if (patch == SEQ_PATCH_ADDITIVE_CUSTOM) {
            additive_voice_configure_track(synth_id, num_voices, &s_additive_voice);
        } else {
            additive_preset_configure_track(synth_id, patch, num_voices);
        }
        return false;
    }
#endif
    amy_send_patch(synth_id, patch, num_voices, synth_flags);
    return true;
}

/* Apply one patch to one slot and remember whether it needs a global-FX
 * reassert, without doing the reassert yet — batch callers apply to several
 * slots and must reassert exactly once after the loop. Returns true if a
 * later flush is owed. */
static bool seq_apply_patch(uint8_t synth_id, uint16_t patch,
                            uint16_t num_voices, uint32_t synth_flags,
                            bool filter_authored, float filter_q)
{
    return sequencer_apply_patch_kind(synth_id, patch, num_voices,
                                      synth_flags, filter_authored, filter_q);
}

/* Reassert global FX iff any patch applied since the last flush was a patch
 * STRING (Juno/DX7/piano): those carry global EQ/chorus commands that
 * overwrite the user's FX state on load. Raw-wave, bass, and FM patches carry
 * none and owe nothing. Idempotent; safe to call with owed == false. Every
 * patch-load path flushes through here so no caller can forget the reassert. */
static inline void seq_flush_patch_fx(bool owed)
{
    if (owed) synth_ui_fx_reassert_global();
}

/* (Re)configure the AMY synth(s) for layer_idx.
 * Drums use a single synth (synth_id[0]); melodic layers configure one synth
 * per row, all sharing the same patch/flags/voice-count but on distinct slots. */
void sequencer_configure_synth(uint8_t layer_idx)
{
    seq_layer_t *layer = &s_layers[layer_idx];

    if (layer->type == SEQ_LAYER_DRUM) {
        if (s_drum_engine == SEQ_DRUM_PCM) {
            /* PCM mode: each track's synth slot becomes a 1-osc PCM player loaded
             * with a built-in 808 sample. We allocate the voice with
             * oscs_per_voice=1 (no patch string), then set wave=PCM + preset on
             * osc 0. Note-on/off + velocity + pitch flow through the SAME emit
             * path as synth mode, so hits get accent/jitter dynamics and the
             * sample is tuned by midi_note (render_pcm) — not the old clinical
             * fixed-velocity drumkit path. PCM carries no global EQ/chorus, so no
             * reassert needed here. */
            for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
                /* Allocate/realloc the slot as a 1-osc voice (clears old patch). */
                amy_event *e = amy_helpers_event_begin();
                e->num_voices    = layer->num_voices;
                e->oscs_per_voice = 1;
                e->synth         = layer->synth_id[t];
                e->synth_flags   = layer->synth_flags;  /* 0 */
                amy_helpers_event_send(e);

                /* Configure osc 0 of this synth as the chosen 808 PCM sample. */
                e = amy_helpers_event_begin();
                e->synth  = layer->synth_id[t];
                e->osc    = 0;
                e->wave   = PCM;
                e->preset = drum_pcm_preset_for(layer_idx, t);
                amy_helpers_event_send(e);
            }
            sequencer_configure_drum_pcm_voice_params(layer_idx);
            return;
        }

        /* SYNTH mode — per-track: each drum row loads its OWN patch onto its OWN
         * synth slot, note-offs honored (flags = 0). Mirrors the melodic loop. */
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            sequencer_kill_synth_voices(layer->synth_id[t]);
            amy_send_patch(layer->synth_id[t], layer->track_patch[t],
                           layer->num_voices, layer->synth_flags);
        }
        /* Every drum SYNTH slot loads a patch string, so a flush is always
         * owed here (once, after the loop). */
        seq_flush_patch_fx(true);
        return;
    }

    /* Melodic: push the shared patch/flags/voices to each row's own synth.
     * Kind dispatch (raw wave / bass / FM / patch string) lives in
     * sequencer_apply_patch_kind() above, shared with the arp. */
    bool string_patch = false;
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        sequencer_kill_synth_voices(layer->synth_id[t]);
        string_patch |= seq_apply_patch(layer->synth_id[t],
                                        layer->patch,
                                        layer->num_voices,
                                        layer->synth_flags,
                                        layer->vp[t].filter_authored,
                                        layer->vp[t].filter.resonance);
    }
    seq_flush_patch_fx(string_patch);
    sequencer_configure_melodic_envelope(layer_idx);
    sequencer_configure_melodic_envelope1(layer_idx);
    sequencer_configure_melodic_filter(layer_idx);
    sequencer_configure_melodic_lfo(layer_idx);
}

/* ── Public API — melodic patch ─────────────────────────────────────── */

void sequencer_core_set_melodic_patch(uint16_t patch_number)
{
    /* 0..127: Juno, 128..255: DX7, 256: built-in piano.
     * 257..263: raw-waveform virtual patches (SEQ_PATCH_SINE..SEQ_PATCH_WAVE_MAX).
     * 264..266: multi-osc bass presets (SEQ_PATCH_BASS_BASE..SEQ_PATCH_BASS_MAX).
     * 267..271: wavetable banks (AMY_WAVETABLE only). 272..276: FM/ALGO voices.
     * 277..279: additive/partials voices
     * (SEQ_PATCH_ADDITIVE_BASE..SEQ_PATCH_ADDITIVE_MAX), always the true
     * ceiling. */
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_ROUTABLE_MAX);
    if (s_melodic_patch == patch_number) {
        return;
    }

    s_melodic_patch = patch_number;
    for (uint8_t i = 0; i < s_num_layers; i++) {
        seq_layer_t *layer = &s_layers[i];
        if (layer->type != SEQ_LAYER_MELODIC) {
            continue;
        }
        layer->patch = s_melodic_patch;
        sequencer_configure_synth(i);
    }

    ESP_LOGI(TAG, "melodic patch -> %u", (unsigned)s_melodic_patch);
}

uint16_t sequencer_core_get_melodic_patch(void)
{
    return s_melodic_patch;
}

/* Per-layer patch access — targets a single melodic layer rather than all of
 * them at once (unlike sequencer_core_set_melodic_patch).  Used by the UI
 * patch-cycle widget so each active melodic layer can step its patch
 * independently. */

uint16_t sequencer_core_get_layer_patch(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return s_melodic_patch;
    return s_layers[layer_idx].patch;
}

void sequencer_core_set_layer_patch(uint8_t layer_idx, uint16_t patch_number)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_ROUTABLE_MAX);
    if (layer->patch == patch_number) return;
    layer->patch    = patch_number;
    s_melodic_patch = patch_number;  /* side-effect write: the global accessor
                                        doubles as the display fallback, which
                                        must track the last-touched layer (see
                                        the contract in seq_core_internal.h) */
    sequencer_configure_synth(layer_idx);
    ESP_LOGI(TAG, "L%u patch -> %u", (unsigned)layer_idx, (unsigned)patch_number);
}

/* ── Live FM voice edits ──────────────────────────────────────────────────── */

#if CONFIG_SYNTH_CUSTOM_FM
void sequencer_core_fm_voice_changed(void)
{
    for (uint8_t i = 0; i < s_num_layers; i++) {
        seq_layer_t *layer = &s_layers[i];
        if (layer->type != SEQ_LAYER_MELODIC) continue;
        if (layer->patch != SEQ_PATCH_FM_CUSTOM) continue;
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            fm_voice_push_live(layer->synth_id[t], &s_fm_voice);
        }
    }
    /* The arp can play FM_CUSTOM too; it checks its own source/patch state. */
    arp_core_fm_voice_changed();
}
#endif

/* ── Live additive voice edits ────────────────────────────────────────────── */

#if CONFIG_SYNTH_ADDITIVE
void sequencer_core_additive_voice_changed(void)
{
    for (uint8_t i = 0; i < s_num_layers; i++) {
        seq_layer_t *layer = &s_layers[i];
        if (layer->type != SEQ_LAYER_MELODIC) continue;
        if (layer->patch != SEQ_PATCH_ADDITIVE_CUSTOM) continue;
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            additive_voice_push_live(layer->synth_id[t], &s_additive_voice);
        }
    }
    /* The arp can play ADDITIVE_CUSTOM too; it checks its own state. */
    arp_core_additive_voice_changed();
}
#endif

/* ── Drum per-track patch (curated Juno list) ────────────────────────────── */

/* Set one drum track's patch directly (clamped to the curated list membership;
 * a value not in the list snaps to the nearest list entry by index 0). Reloads
 * just that track's synth slot. */
void sequencer_core_set_drum_patch(uint8_t layer_idx, uint8_t track,
                                   uint16_t patch_number)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_DRUM) return;
    if (layer->track_patch[track] == patch_number) return;

    layer->track_patch[track] = patch_number;
    if (track == 0) layer->patch = patch_number;  /* keep display fallback live */

    /* In PCM mode the slot is a PCM player, not a patch — store the selection for
     * when we switch back to SYNTH mode, but don't push a patch load now (it
     * would clobber the PCM osc). */
    if (s_drum_engine == SEQ_DRUM_PCM) {
        ESP_LOGI(TAG, "drum L%u track %u patch -> %u (stored; PCM active)",
                 layer_idx, track, (unsigned)patch_number);
        return;
    }

    /* Reload only this track's synth slot with the new patch. */
    sequencer_kill_synth_voices(layer->synth_id[track]);
    amy_send_patch(layer->synth_id[track], patch_number,
                   layer->num_voices, layer->synth_flags);

    /* A drum patch is always a string, so the flush is always owed. */
    seq_flush_patch_fx(true);

    ESP_LOGI(TAG, "drum L%u track %u patch -> %u",
             layer_idx, track, (unsigned)patch_number);
}

uint16_t sequencer_core_get_drum_patch(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    return s_layers[layer_idx].track_patch[track];
}

/* Step one drum track's patch `dir` (+/-1) entries through the curated list,
 * wrapping at the ends. Returns the newly-applied patch number. */
uint16_t sequencer_core_cycle_drum_patch(uint8_t layer_idx, uint8_t track,
                                         int dir)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_DRUM) return 0;

    uint16_t next = patch_domain_step(&s_drum_domain, layer->track_patch[track], dir);
    sequencer_core_set_drum_patch(layer_idx, track, next);
    return next;
}

/* ── Drum sound source (Synth vs PCM) ───────────────────────────────────── */

void sequencer_core_set_drum_engine(seq_drum_engine_t engine)
{
    if (engine != SEQ_DRUM_SYNTH && engine != SEQ_DRUM_PCM) return;
    if (s_drum_engine == engine) return;
    s_drum_engine = engine;

    /* Re-configure every drum layer's synth slots in place for the new source.
     * The grid/velocity/pitch and scheduled note events are untouched, so the
     * pattern keeps playing — only the per-track sound source swaps. */
    for (uint8_t i = 0; i < s_num_layers; i++) {
        if (s_layers[i].type == SEQ_LAYER_DRUM) {
            sequencer_configure_synth(i);
        }
    }
    ESP_LOGI(TAG, "drum engine -> %s",
             engine == SEQ_DRUM_PCM ? "PCM" : "SYNTH");
}

seq_drum_engine_t sequencer_core_get_drum_engine(void)
{
    return s_drum_engine;
}

void sequencer_core_set_drum_pcm_preset(uint8_t layer_idx, uint8_t track,
                                        uint16_t preset_number)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    if (s_layers[layer_idx].type != SEQ_LAYER_DRUM) return;

    (void)drum_pcm_preset_for(layer_idx, track);   /* seed defaults first */
    s_drum_pcm_preset[layer_idx][track] = preset_number;

    if (s_drum_engine == SEQ_DRUM_PCM) {
        /* Live-reload just this track's osc (mirrors sequencer_core_set_drum_patch). */
        amy_event *e = amy_helpers_event_begin();
        e->synth  = s_layers[layer_idx].synth_id[track];
        e->osc    = 0;
        e->wave   = PCM;
        e->preset = preset_number;
        amy_helpers_event_send(e);
    }
    ESP_LOGI(TAG, "drum L%u track %u PCM preset -> %u",
             layer_idx, track, (unsigned)preset_number);
}

uint16_t sequencer_core_cycle_drum_pcm_preset(uint8_t layer_idx, uint8_t track,
                                              int dir)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    if (s_layers[layer_idx].type != SEQ_LAYER_DRUM) return 0;

    /* ROM drum samples only: pcm_wavetable_base is the first non-drum map
     * entry in the compiled-in bank (gamma808: 19, pcm_tiny: 11) regardless
     * of whether wavetables are compiled in. Memory presets (sample_rec
     * overrides) sit above the ROM map; stepping from one re-enters the ROM
     * bank at the near end. */
    uint16_t bound = pcm_wavetable_base;
    if (bound == 0) return 0;

    uint16_t cur = drum_pcm_preset_for(layer_idx, track);
    uint16_t next;
    if (cur >= bound) {
        next = (dir > 0) ? 0 : (uint16_t)(bound - 1);
    } else {
        next = (uint16_t)((cur + (uint16_t)bound + (dir > 0 ? 1 : -1)) % bound);
    }
    sequencer_core_set_drum_pcm_preset(layer_idx, track, next);
    return next;
}

uint16_t sequencer_core_get_drum_pcm_preset(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    return drum_pcm_preset_for(layer_idx, track);
}

/* ── Arpeggiator support ─────────────────────────────────────────────────── */

uint8_t sequencer_core_arp_synth(void)  { return SEQ_ARP_SYNTH; }
uint8_t sequencer_core_arp_voices(void) { return SEQ_ARP_VOICES; }
uint32_t sequencer_core_arp_tag_base(void) { return SEQ_ARP_TAG_BASE; }

uint8_t sequencer_core_clamp_melodic_note(int32_t midi_note)
{
    return SEQ_CLAMP_U8(midi_note, SEQ_MEL_NOTE_MIN, SEQ_MEL_NOTE_MAX);
}

void sequencer_core_push_envelope(uint8_t synth, const seq_env_t *env)
{
    if (env == NULL) return;
    float sustain = (float)env->sustain_pct / 100.0f;

    amy_event *e = amy_helpers_event_begin();
    e->synth         = synth;
    e->bp_is_set[0]  = 1;
    e->eg_type[0]    = env->eg_type;
    uint32_t attack_ms = (env->attack_ms < 2) ? 2 : env->attack_ms;  /* 2 ms floor */
    e->eg0_times[0]  = attack_ms;
    e->eg0_values[0] = 1.0f;
    e->eg0_times[1]  = env->decay_ms;
    e->eg0_values[1] = sustain;
    e->eg0_times[2]  = env->release_ms;
    e->eg0_values[2] = 0.0f;
    amy_helpers_event_send(e);
}

void sequencer_core_push_envelope_eg1(uint8_t synth, uint8_t osc, const seq_env_t *env)
{
    if (env == NULL) return;
    float sustain = (float)env->sustain_pct / 100.0f;

    amy_event *e = amy_helpers_event_begin();
    e->synth         = synth;
    e->osc           = osc;
    e->bp_is_set[1]  = 1;
    e->eg_type[1]    = env->eg_type;
    uint32_t attack_ms = (env->attack_ms < 2) ? 2 : env->attack_ms;  /* 2 ms floor */
    e->eg1_times[0]  = attack_ms;
    e->eg1_values[0] = 1.0f;
    e->eg1_times[1]  = env->decay_ms;
    e->eg1_values[1] = sustain;
    e->eg1_times[2]  = env->release_ms;
    e->eg1_values[2] = 0.0f;
    amy_helpers_event_send(e);
}

void sequencer_core_arp_configure(uint16_t patch_number, uint8_t num_voices,
                                  bool filter_authored, float filter_q)
{
    /* Full catalog, same as melodic: kind dispatch (raw wave / bass / FM /
     * patch string) is shared via sequencer_apply_patch_kind(). */
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_FULL_MAX);
    /* Osc topology can change between kinds (1-2 oscs for waves/bass, 7 for
     * FM voices, N+1 for additive); kill sounding voices before the pool is
     * rebuilt. */
    sequencer_kill_synth_voices(SEQ_ARP_SYNTH);
    bool string_patch = seq_apply_patch(SEQ_ARP_SYNTH, patch_number,
                                        num_voices, 0,
                                        filter_authored, filter_q);
    seq_flush_patch_fx(string_patch);
    ESP_LOGI(TAG, "arp synth %u patch -> %u (%u voices)",
             (unsigned)SEQ_ARP_SYNTH, (unsigned)patch_number, (unsigned)num_voices);
}

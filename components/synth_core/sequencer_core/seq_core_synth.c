#include "sequencer_core/seq_core_internal.h"

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

/* Built-in 808 PCM sample indices (from amy/src/pcm_tiny.h pcm_map[]) used by
 * PCM drum mode, one per track: kick, snare, closed-hat, clap. */
static const int16_t SEQ_DRUM_PCM_PRESET[SEQ_TRACKS] = {
    1,    /* track 0: [1] 808-KIK 4-D    */
    2,    /* track 1: [2] 808-SNR 4-D    */
    6,    /* track 2: [6] 808-C-HAT1-D   */
    9,    /* track 3: [9] 808-DRYCLP-D   */
};

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
 * voices, 1 osc each).  Envelope and filter are applied by the caller. */
static void sequencer_configure_melodic_wave_track(uint8_t synth_id,
                                                    uint16_t patch,
                                                    uint16_t num_voices)
{
    static const uint16_t s_wave_for_patch[] = {
        SINE, SAW_DOWN, SAW_UP, PULSE, TRIANGLE, NOISE, KS,
    };
    uint16_t widx = (uint16_t)(patch - SEQ_PATCH_WAVE_BASE);
    if (widx >= (uint16_t)(sizeof(s_wave_for_patch) / sizeof(s_wave_for_patch[0])))
        widx = 0;
    uint16_t wave = s_wave_for_patch[widx];

    amy_event *e = amy_helpers_event_begin();
    e->synth          = synth_id;
    e->num_voices     = num_voices;
    e->oscs_per_voice = 1;
    amy_helpers_event_send(e);

    e = amy_helpers_event_begin();
    e->synth                  = synth_id;
    e->osc                    = 0;
    e->wave                   = wave;
    if (wave == KS) e->feedback = 0.9f;
    e->freq_coefs[COEF_NOTE]  = 1.0f;
    e->amp_coefs[COEF_CONST]  = 1.0f;
    e->amp_coefs[COEF_VEL]    = 1.0f;
    e->amp_coefs[COEF_EG0]    = 1.0f;
    amy_helpers_event_send(e);
}

/* Push each AUTHORED row's stored envelope to its own per-row synth.
 * Deferred authority: a row's envelope only overrides the patch's own envelope
 * once the user has committed it in the graph editor (env_authored[t]==true).
 * Unauthored rows are left alone so the freshly-loaded patch envelope plays. */
static void sequencer_configure_melodic_envelope(uint8_t layer_idx)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (layer->env_authored[t]) {
            sequencer_configure_melodic_envelope_track(layer_idx, t);
        }
    }
}

/* Push the filter for every authored row in a layer (called after patch reload). */
static void sequencer_configure_melodic_filter(uint8_t layer_idx)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (layer->filter_authored[t]) {
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
    return &s_layers[layer_idx].env[track];
}

/* AMY events are emitted through the shared amy_helpers scratch buffer (see
 * amy_helpers.{c,h}) — one module-level event + mutex for all first-party
 * callers, all of which are FreeRTOS tasks (never ISRs). */

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
                e->preset = SEQ_DRUM_PCM_PRESET[t];
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
        /* Patch strings carry global EQ/chorus commands; keep them per-synth. */
        synth_ui_fx_reassert_global();
        return;
    }

    /* Melodic: push the shared patch/flags/voices to each row's own synth.
     * Patches >= SEQ_PATCH_WAVE_BASE are raw-waveform virtual patches; they are
     * configured directly instead of via the amy_send_patch() string loader.
     * Patches >= SEQ_PATCH_BASS_BASE are multi-osc bass presets (oscs_per_voice=2). */
    bool is_wave_patch = (layer->patch >= SEQ_PATCH_WAVE_BASE &&
                          layer->patch <= SEQ_PATCH_WAVE_MAX);
    bool is_bass_patch = (layer->patch >= SEQ_PATCH_BASS_BASE &&
                          layer->patch <= SEQ_PATCH_BASS_MAX);
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        sequencer_kill_synth_voices(layer->synth_id[t]);
        if (is_wave_patch) {
            sequencer_configure_melodic_wave_track(layer->synth_id[t],
                                                   layer->patch,
                                                   layer->num_voices);
        } else if (is_bass_patch) {
            bass_preset_configure_track(layer->synth_id[t],
                                                  layer->patch,
                                                  layer->num_voices);
        } else {
            amy_send_patch(layer->synth_id[t], layer->patch,
                           layer->num_voices, layer->synth_flags);
        }
    }
    /* Raw-wave and bass patches carry no global EQ/chorus commands; skip reassert.
     * Non-wave/non-bass patches (Juno/DX7) must reassert so that switching away
     * from one doesn't leave stale global FX active. */
    if (!is_wave_patch && !is_bass_patch) synth_ui_fx_reassert_global();
    sequencer_configure_melodic_envelope(layer_idx);
    sequencer_configure_melodic_filter(layer_idx);
}

/* ── Public API — melodic patch ─────────────────────────────────────── */

void sequencer_core_set_melodic_patch(uint16_t patch_number)
{
    /* 0..127: Juno, 128..255: DX7, 256: built-in piano.
     * 257..263: raw-waveform virtual patches (SEQ_PATCH_SINE..SEQ_PATCH_WAVE_MAX).
     * 264..266: multi-osc bass presets (SEQ_PATCH_BASS_BASE..SEQ_PATCH_BASS_MAX). */
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_BASS_MAX);
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

    /* Patch strings carry global EQ/chorus commands; keep them per-synth. */
    synth_ui_fx_reassert_global();

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

void sequencer_core_arp_configure(uint16_t patch_number, uint8_t num_voices)
{
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_WAVE_MAX);
    if (patch_number >= SEQ_PATCH_WAVE_BASE) {
        /* Wave virtual patch: configure as a raw-waveform synth (same logic as
         * melodic wave tracks) instead of loading an amy_send_patch() string. */
        sequencer_configure_melodic_wave_track(SEQ_ARP_SYNTH, patch_number, num_voices);
    } else {
        amy_send_patch(SEQ_ARP_SYNTH, patch_number, num_voices, 0);
    }
    /* Patch strings carry global EQ/chorus commands; reassert so Juno/DX7 FX
     * don't leak when switching to/from a wave patch. */
    synth_ui_fx_reassert_global();
    ESP_LOGI(TAG, "arp synth %u patch -> %u (%u voices)",
             (unsigned)SEQ_ARP_SYNTH, (unsigned)patch_number, (unsigned)num_voices);
}

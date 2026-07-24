#include "sequencer_core/seq_core_internal.h"
#include "voice_config.h"
#include "seq_clamp.h"

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
#if CONFIG_SYNTH_DRUM_SYNTH_MODE
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
#endif /* CONFIG_SYNTH_DRUM_SYNTH_MODE */

/* ── Drum sample banks (menu "Drum Bank" selector) ──
 * One row per selectable PCM bank: the compiled-in 808 ROM bank plus the
 * gamma9001 banks streamed from the 'drums' flash partition. first/count are
 * PCM preset ranges; roles[] seeds the four tracks (kick, snare, hat,
 * clap-analog) when the bank is selected. Preset numbers mirror the vendored
 * amy/src/pcm_gamma9001.h map (256=909BD .. 391=Narrow) — re-verify after any
 * AMY re-vendor. Banks are contiguous in preset space, so free per-track
 * cycling after a seed stays inside the bank until the user walks out. */
typedef struct {
    const char *name;
    uint16_t    first, count;
    uint16_t    roles[SEQ_TRACKS];
} seq_drum_bank_t;

#define SEQ_GAMMA_PCM_FIRST 256u
#define SEQ_GAMMA_PCM_COUNT 136u

static const seq_drum_bank_t s_drum_banks[] = {
    { "808",   0,   19, {   2,  12,   9,   3} },  /* ROM bank (gamma808)       */
    { "909",   256, 17, { 256, 268, 260, 258} },  /* BD, SD, HH, CLAP          */
    { "Linn",  273, 10, { 273, 280, 277, 279} },  /* Kick, Snare, HHC, RimShot */
    { "MR12",  283, 4,  { 285, 286, 283, 284} },  /* Kick, Snare, HHC, HHO     */
    { "SynFX", 287, 24, { 294, 289, 306, 302} },  /* BD04, Static, Click, Boink*/
    { "Power", 311, 20, { 311, 314, 318, 315} },  /* RealKick, Snare, HH, Clap */
    { "Perc",  331, 44, { 350, 351, 352, 335} },  /* NoiceKick, OldSnr, Shaker */
    { "Misc",  375, 17, { 388, 381, 376, 379} },  /* BassRec, Silver, Shkr, Lsr*/
};
#define SEQ_DRUM_BANK_COUNT ((uint8_t)(sizeof(s_drum_banks) / sizeof(s_drum_banks[0])))

/* Last bank applied via the selector (display state only; the per-track
 * presets are the persisted truth and cycle freely across banks). */
static uint8_t s_drum_bank = 0;

/* Gamma9001 banks are selectable only while their sample blob is mounted
 * (main.c maps the 'drums' partition and calls amy_set_gamma9001_pcm). */
static inline bool drum_gamma_available(void)
{
#ifdef GAMMA9001
    return gamma9001_pcm != NULL;
#else
    return false;
#endif
}

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

/* ── Chord-aware per-track voice budget ───────────────────────────────────
 * What each melodic row's synth was last configured with, so a chord
 * assign/remove or slot resize can detect "my voice count no longer fits"
 * without re-deriving AMY-side state. Runtime bookkeeping only; non-static so
 * delete_layer's compaction (seq_core_state.c) can shift it in lockstep with
 * the other per-layer parallel arrays. */
uint8_t s_voices_applied[MAX_LAYERS][SEQ_TRACKS];

uint8_t seq_track_num_voices(const seq_layer_t *layer, uint8_t track)
{
    uint8_t v = layer->num_voices;
    if (layer->type == SEQ_LAYER_MELODIC &&
        SEQ_NOTE_IS_CHORD(layer->track_base_note[track])) {
        uint8_t c = seq_chords_count(SEQ_CHORD_INDEX(layer->track_base_note[track]));
        if (c > v) v = c;
    }
    return v;
}

bool sequencer_layer_voices_stale(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return false;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return false;
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (seq_track_num_voices(layer, t) != s_voices_applied[layer_idx][t]) {
            return true;
        }
    }
    return false;
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
                                                    float ks_feedback)
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
        .oscs_per_voice       = 3,   /* osc1 = native LFO carrier, osc2 = its
                                        wobble modulator (chained mod_source) */
#else
        .oscs_per_voice       = 1,
#endif
        .wave                 = wave,
        .osc0_amp_const       = 1.0f,
        .osc0_amp_vel         = 1.0f,
        .ks_feedback_authored = filter_authored,
        .ks_feedback          = ks_feedback,
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
    /* Raw-wave primitives (SINE..KS, wavetables) carry no patch envelope of
     * their own. With no envelope pushed, the carrier's COEF_EG0 stays a
     * constant 1.0: AMY reads an empty breakpoint set as a permanently open
     * gate (envelope.c) and a velocity-0 note-off never zeroes it (amy.c), so
     * the oscillator rings forever — surviving both patch changes and pause,
     * which both silence voices via that same velocity-0 note-off. Force the
     * default melodic envelope onto every such unauthored row so a note-off
     * actually releases it. (KS/NOISE additionally get their sustain floor
     * applied downstream in sequencer_configure_melodic_envelope_track.) */
    bool force_wave = sequencer_core_is_wave_patch(layer->patch);
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (layer->vp[t].env_authored || force_wave) {
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

/* Push the filter for every authored row in a layer (called after patch reload).
 *
 * Unauthored rows normally keep whatever filter the just-loaded patch string
 * baked in (built-in Juno/DX7 patches carry a G/F/R filter block, so a fresh
 * voice inherits that patch's LPF). With CONFIG_SEQ_MELODIC_DISABLE_DEFAULT_LPF
 * set, those unauthored rows instead get a FILTER_NONE event so the raw patch
 * tone is heard; authored rows always take their stored filter. */
static void sequencer_configure_melodic_filter(uint8_t layer_idx)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (layer->vp[t].filter_authored) {
            sequencer_configure_melodic_filter_track(layer_idx, t);
#if CONFIG_SEQ_MELODIC_DISABLE_DEFAULT_LPF
        } else {
            /* Strip the patch-baked filter from this fresh row. */
            amy_event *e = amy_helpers_event_begin();
            e->synth       = layer->synth_id[t];
            e->filter_type = FILTER_NONE;
            amy_helpers_event_send(e);
#endif
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
                                       bool filter_authored, float ks_feedback)
{
    /* Virtual patch number whose feature is not in this build (browse skips
     * these, but stored/programmatic values can still arrive): snap to raw
     * SINE rather than letting a virtual number reach the string loader. */
    if (sequencer_core_patch_compiled_out(patch)) {
        sequencer_configure_melodic_wave_track(synth_id, SEQ_PATCH_SINE,
                                               num_voices, filter_authored,
                                               ks_feedback);
        return false;
    }
    if (sequencer_core_is_wave_patch(patch)) {
        sequencer_configure_melodic_wave_track(synth_id, patch, num_voices,
                                               filter_authored, ks_feedback);
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
                            bool filter_authored, float ks_feedback)
{
    return sequencer_apply_patch_kind(synth_id, patch, num_voices,
                                      synth_flags, filter_authored, ks_feedback);
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

    /* Melodic: push the shared patch/flags to each row's own synth. The voice
     * count is per-track: layer->num_voices, widened to the chord tone count
     * on rows carrying a chord preset (seq_track_num_voices) — voices are
     * spent only where chords play. Kind dispatch (raw wave / bass / FM /
     * patch string) lives in sequencer_apply_patch_kind() above, shared with
     * the arp. */
    bool string_patch = false;
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        uint8_t voices = seq_track_num_voices(layer, t);
        sequencer_kill_synth_voices(layer->synth_id[t]);
        string_patch |= seq_apply_patch(layer->synth_id[t],
                                        layer->patch,
                                        voices,
                                        layer->synth_flags,
                                        layer->vp[t].filter_authored,
                                        layer->vp[t].filter.feedback);
        s_voices_applied[layer_idx][t] = voices;
    }
    seq_flush_patch_fx(string_patch);
    sequencer_configure_melodic_envelope(layer_idx);
    sequencer_configure_melodic_envelope1(layer_idx);
    sequencer_configure_melodic_filter(layer_idx);
    sequencer_configure_melodic_lfo(layer_idx);
    /* Glide is an AMY per-osc setting that a voice rebuild clears, so reassert
     * it here after the pool/patch has been (re)built. */
    sequencer_core_push_melodic_portamento(layer_idx);
}

/* ── Public API — melodic patch ─────────────────────────────────────── */

/* Reconfigure a layer's synths with its scheduled events paused.
 *
 * The grid steps are repeating events inside AMY's sequencer, fired
 * autonomously from the 500 us sequencer poll task — leaving them live while
 * a patch load rebuilds the voice/osc tables lets a note-on resolve against
 * half-updated mappings and strand an osc as AUDIBLE outside any voice, where
 * no later kill can reach it (kills resolve through the CURRENT voice map).
 * That is the intermittent "ringing that survives patch changes" heard when
 * rapidly cycling patches on a playing layer. Same discipline as
 * arp_rebuild(): clear the schedule, rebuild (configure kills sounding voices
 * per track — mandatory, their note-offs were just cleared too), re-emit.
 * sequencer_emit_step() is s_playing-gated, so the resync is a no-op while
 * paused. Cost: the layer goes quiet for the events cleared mid-flight; the
 * re-emit restores the same absolute tick positions, so groove phase is kept.
 * Non-static since chord presets: also the reconfigure path for per-track
 * voice-count changes (chord assign/remove, slot resize) — see
 * seq_core_internal.h. */
void sequencer_reconfigure_layer_paused(uint8_t layer_idx)
{
    sequencer_clear_layer_tags(layer_idx);
    /* Also drop the trig engine's pending one-shots (ratchet + chord tone
     * pairs): a sub-hit scheduled moments ago must not resolve against the
     * half-rebuilt voice pool below — the same stranded-osc mechanism the
     * periodic clear above exists to prevent. */
    sequencer_core_trig_clear_all(layer_idx);
    sequencer_configure_synth(layer_idx);
    sequencer_resync_layer(layer_idx);
}

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
        sequencer_reconfigure_layer_paused(i);
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
    sequencer_reconfigure_layer_paused(layer_idx);
    ESP_LOGI(TAG, "L%u patch -> %u", (unsigned)layer_idx + 1u, (unsigned)patch_number);
}

/* Re-apply the layer's current patch and every stored/authored parameter to
 * its synth slots — the exact-state restore used when an editor's live
 * preview is cancelled on a row whose target state came from the patch itself
 * (never authored) and so cannot be re-pushed from the store. */
void sequencer_core_reload_layer_synth(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return;
    if (s_layers[layer_idx].type != SEQ_LAYER_MELODIC) return;
    sequencer_reconfigure_layer_paused(layer_idx);
    ESP_LOGI(TAG, "L%u synth reloaded (preview cancel)", (unsigned)layer_idx + 1u);
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
        ESP_LOGI(TAG, "drum L%u T%u patch -> %u (stored; PCM active)",
                 layer_idx + 1u, track + 1u, (unsigned)patch_number);
        return;
    }

    /* Reload only this track's synth slot with the new patch — with the
     * layer's schedule paused around the load (see
     * sequencer_reconfigure_layer_paused for why); resynced below. */
    sequencer_clear_layer_tags(layer_idx);
    sequencer_kill_synth_voices(layer->synth_id[track]);
    amy_send_patch(layer->synth_id[track], patch_number,
                   layer->num_voices, layer->synth_flags);
    sequencer_resync_layer(layer_idx);

    /* A drum patch is always a string, so the flush is always owed. */
    seq_flush_patch_fx(true);

    ESP_LOGI(TAG, "drum L%u T%u patch -> %u",
             layer_idx + 1u, track + 1u, (unsigned)patch_number);
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
#if CONFIG_SYNTH_DRUM_SYNTH_MODE
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_DRUM) return 0;

    uint16_t next = patch_domain_step(&s_drum_domain, layer->track_patch[track], dir);
    sequencer_core_set_drum_patch(layer_idx, track, next);
    return next;
#else
    (void)layer_idx; (void)track; (void)dir;
    return 0;
#endif
}

/* ── Drum sound source (Synth vs PCM) ───────────────────────────────────── */

void sequencer_core_set_drum_engine(seq_drum_engine_t engine)
{
    if (engine != SEQ_DRUM_SYNTH && engine != SEQ_DRUM_PCM) return;
#if !CONFIG_SYNTH_DRUM_SYNTH_MODE
    /* Synth engine compiled out: coerce so old project snapshots saved in
     * SYNTH mode still load (as PCM) instead of muting the drum layer. */
    engine = SEQ_DRUM_PCM;
#endif
    if (s_drum_engine == engine) return;
    s_drum_engine = engine;

    /* Re-configure every drum layer's synth slots in place for the new source.
     * The grid/velocity/pitch and scheduled note events are untouched, so the
     * pattern keeps playing — only the per-track sound source swaps. */
    for (uint8_t i = 0; i < s_num_layers; i++) {
        if (s_layers[i].type == SEQ_LAYER_DRUM) {
            sequencer_reconfigure_layer_paused(i);
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
    ESP_LOGI(TAG, "drum L%u T%u PCM preset -> %u",
             layer_idx + 1u, track + 1u, (unsigned)preset_number);
}

uint16_t sequencer_core_cycle_drum_pcm_preset(uint8_t layer_idx, uint8_t track,
                                              int dir)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    if (s_layers[layer_idx].type != SEQ_LAYER_DRUM) return 0;

    /* Combined drum sample domain: the ROM bank (0 .. pcm_wavetable_base-1;
     * pcm_wavetable_base is the first non-drum map entry — gamma808: 19,
     * pcm_tiny: 11) followed by the gamma9001 banks (256..391) when their
     * blob is mounted. The two ranges are walked as one wrapping list via a
     * linear index. Memory presets (sample_rec overrides, outside both
     * ranges) step back into the domain at the near end. */
    uint16_t bound = pcm_wavetable_base;
    if (bound == 0) return 0;
    uint16_t total = bound + (drum_gamma_available() ? SEQ_GAMMA_PCM_COUNT : 0);

    uint16_t cur = drum_pcm_preset_for(layer_idx, track);
    int li_cur;
    if (cur < bound) {
        li_cur = (int)cur;
    } else if (cur >= SEQ_GAMMA_PCM_FIRST &&
               cur < SEQ_GAMMA_PCM_FIRST + SEQ_GAMMA_PCM_COUNT &&
               total > bound) {
        li_cur = (int)bound + (int)(cur - SEQ_GAMMA_PCM_FIRST);
    } else {
        /* Memory preset (or unreachable gamma preset): enter at the near end. */
        li_cur = (dir > 0) ? -1 : 0;
    }

    int li_next = (li_cur + (dir > 0 ? 1 : -1) + (int)total) % (int)total;
    uint16_t next = (li_next < (int)bound)
        ? (uint16_t)li_next
        : (uint16_t)(SEQ_GAMMA_PCM_FIRST + (uint16_t)(li_next - (int)bound));
    sequencer_core_set_drum_pcm_preset(layer_idx, track, next);
    return next;
}

/* ── Drum source selector (see sequencer_core.h) ──
 * Domain layout: [Synth]* + s_drum_banks[0] (808 ROM) + gamma banks. The
 * Synth entry exists only when compiled in; the gamma entries only while the
 * sample blob is mounted. */
#if CONFIG_SYNTH_DRUM_SYNTH_MODE
#define DRUM_SOURCE_SYNTH_ENTRIES 1u
#else
#define DRUM_SOURCE_SYNTH_ENTRIES 0u
#endif

uint8_t sequencer_core_drum_source_count(void)
{
    uint8_t banks = drum_gamma_available() ? SEQ_DRUM_BANK_COUNT : 1;
    return (uint8_t)(DRUM_SOURCE_SYNTH_ENTRIES + banks);
}

const char *sequencer_core_drum_source_name(uint8_t idx)
{
#if CONFIG_SYNTH_DRUM_SYNTH_MODE
    if (idx == 0) return "Synth";
#endif
    uint8_t bank = (uint8_t)(idx - DRUM_SOURCE_SYNTH_ENTRIES);
    if (bank >= SEQ_DRUM_BANK_COUNT) return "?";
    return s_drum_banks[bank].name;
}

uint8_t sequencer_core_get_drum_source(void)
{
#if CONFIG_SYNTH_DRUM_SYNTH_MODE
    if (s_drum_engine == SEQ_DRUM_SYNTH) return 0;
#endif
    return (uint8_t)(DRUM_SOURCE_SYNTH_ENTRIES + s_drum_bank);
}

void sequencer_core_set_drum_source(uint8_t idx)
{
    if (idx >= sequencer_core_drum_source_count()) return;

#if CONFIG_SYNTH_DRUM_SYNTH_MODE
    if (idx == 0) {
        sequencer_core_set_drum_engine(SEQ_DRUM_SYNTH);
        return;
    }
#endif
    uint8_t bank = (uint8_t)(idx - DRUM_SOURCE_SYNTH_ENTRIES);
    s_drum_bank = bank;
    const seq_drum_bank_t *b = &s_drum_banks[bank];

    /* Seed every drum layer's four tracks with the bank's role defaults.
     * set_drum_pcm_preset live-reloads each track's osc when PCM is already
     * active; the engine switch below covers the Synth->PCM case (it
     * reconfigures the slots and picks up the just-seeded presets). */
    for (uint8_t i = 0; i < s_num_layers; i++) {
        if (s_layers[i].type != SEQ_LAYER_DRUM) continue;
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            sequencer_core_set_drum_pcm_preset(i, t, b->roles[t]);
        }
    }
    sequencer_core_set_drum_engine(SEQ_DRUM_PCM);
    ESP_LOGI(TAG, "drum bank -> %s (presets %u..%u)",
             b->name, (unsigned)b->first, (unsigned)(b->first + b->count - 1));
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
    uint32_t attack_ms  = SEQ_CLAMP_U32(env->attack_ms,
                                        VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    uint32_t release_ms = SEQ_CLAMP_U32(env->release_ms,
                                        VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    e->eg0_times[0]  = attack_ms;
    e->eg0_values[0] = 1.0f;
    e->eg0_times[1]  = env->decay_ms;
    e->eg0_values[1] = sustain;
    e->eg0_times[2]  = release_ms;
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
    uint32_t attack_ms  = SEQ_CLAMP_U32(env->attack_ms,
                                        VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    uint32_t release_ms = SEQ_CLAMP_U32(env->release_ms,
                                        VOICE_ENV_RELEASE_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    e->eg1_times[0]  = attack_ms;
    e->eg1_values[0] = 1.0f;
    e->eg1_times[1]  = env->decay_ms;
    e->eg1_values[1] = sustain;
    e->eg1_times[2]  = release_ms;
    e->eg1_values[2] = 0.0f;
    amy_helpers_event_send(e);
}

/* Push the layer's melodic glide time to each row's synth slot. Mirrors
 * arp_push_portamento(): a bare portamento_ms event (no velocity) fans out to
 * every voice's base osc, where AMY applies portamento_alpha as a logfreq
 * low-pass. 0 ms = off. Drum layers never call this (melodic-only config path).*/
void sequencer_core_push_melodic_portamento(uint8_t layer_idx)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth         = layer->synth_id[t];
        e->portamento_ms = layer->portamento_ms;
        amy_helpers_event_send(e);
    }
}

void sequencer_core_arp_configure(uint16_t patch_number, uint8_t num_voices,
                                  bool filter_authored, float ks_feedback)
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
                                        filter_authored, ks_feedback);
    seq_flush_patch_fx(string_patch);
    ESP_LOGI(TAG, "arp synth %u patch -> %u (%u voices)",
             (unsigned)SEQ_ARP_SYNTH, (unsigned)patch_number, (unsigned)num_voices);
}

void sequencer_core_configure_synth_slot(uint8_t synth_id, uint16_t patch_number,
                                         uint8_t num_voices)
{
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_FULL_MAX);
    /* Osc topology can change between patch kinds; kill sounding voices
     * before the pool is rebuilt (same discipline as the arp path above). */
    sequencer_kill_synth_voices(synth_id);
    bool string_patch = seq_apply_patch(synth_id, patch_number, num_voices,
                                        0, false, 0.0f);
    seq_flush_patch_fx(string_patch);
    ESP_LOGI(TAG, "synth %u patch -> %u (%u voices)",
             (unsigned)synth_id, (unsigned)patch_number, (unsigned)num_voices);
}

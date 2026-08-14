#include "sequencer_core/seq_core_internal.h"
#include "voice_config.h"
#include "seq_clamp.h"

/* ── State definitions — owns drum engine selector ──────────────────── */
/* Drum sound source for the whole drum layer. SYNTH = tonal AMY patches per
 * track, PCM = built-in 808 samples. Switching reconfigures the drum layer's
 * synth slots in place. */
seq_drum_engine_t s_drum_engine = SEQ_DRUM_PCM;

/* Curated drum-patch cycle list for SYNTH mode - the per-track patch control
 * wraps within this list, NOT the full 0..256 range. The DX7 idiophones have
 * much sharper attack and shorter decay than the Juno "drum" patches, so they
 * read as real percussion. Pitch drives timbre (low = body/kick, high =
 * hat/shaker); see the role-based defaults below. */
#if CONFIG_SYNTH_DRUM_SYNTH_MODE
static const uint16_t SEQ_DRUM_PATCH_LIST[] = {
    245,  /* DX7 B.DRM-SNAR  - bass-drum/snare, closest to a kit voice */
    221,  /* DX7 BLOCK       - woodblock, tight click (hat/rim)        */
    223,  /* DX7 LOG DRUM    - tuned tom/perc                          */
    220,  /* DX7 COW BELL    - metallic accent                         */
    149,  /* DX7 MARIMBA     - clean mallet (melodic perc / blips)     */
    215,  /* DX7 XYLOPHONE   - bright mallet (hat-ish at high pitch)   */
    148,  /* DX7 VIBE 1      - soft mallet (ghost notes)               */
    219,  /* DX7 BELLS       - bell accent                             */
    58,   /* Juno Drum Booms - kick body at low pitch                  */
    61,   /* Juno Hand Claps - clap                                    */
    46,   /* Juno Shaker     - shaker/hat texture                      */
    70,   /* Juno Perc Pluck - plucky perc                             */
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
 * PCM preset ranges; roles[] and notes[] seed the four tracks on selection -
 * a bank pick is a full kit change, sounds AND pitches. Preset numbers
 * mirror the vendored amy/src/pcm_gamma9001.h map (256=909BD .. 391=Narrow) -
 * re-verify after any AMY re-vendor. Banks are contiguous in preset space, so
 * per-track cycling after a seed stays inside the bank until the user walks
 * out (cycled presets keep the role's note).
 * notes[] values are per-bank ear-tuned defaults; until a bank has had its
 * tuning pass they carry the legacy role defaults (SEQ_DRUM_DEFAULT_NOTE,
 * tuned for the 808 ROM kit). Editing a kit = touching one row: swap a
 * role's preset number, note, and/or voice-param blocks here.
 * eg0/eg1/flt are OPTIONAL per-role voice-param defaults (point rows at
 * named static const blocks; NULL = legacy engine defaults: the EDM
 * envelope tables, no EG1, the hat HPF rule). They are the bank-level
 * analog of a patch's baked-in voice character: applied whenever the
 * track's voice params (re)configure and the row has NOT been user-
 * authored - authored vp wins, same deferred-authority rule as melodic.
 * The bank is derived from the track's CURRENT preset (not the display
 * selector), so kit defaults follow cycling within a bank and survive
 * project reload; presets outside every bank range (e.g. the runtime-
 * recorded sample slot) fall back to legacy defaults. */
typedef struct {
    const char *name;
    uint16_t    first, count;
    uint16_t    roles[SEQ_TRACKS];
    uint8_t     notes[SEQ_TRACKS];
    const seq_env_t    *eg0[SEQ_TRACKS];
    const seq_env_t    *eg1[SEQ_TRACKS];
    const seq_filter_t *flt[SEQ_TRACKS];
} seq_drum_bank_t;

#define SEQ_GAMMA_PCM_FIRST 256u
#define SEQ_GAMMA_PCM_COUNT 136u

/* Ear-tuned voice-param blocks referenced by bank rows below (a tuning
 * pass adds a named block per role that needs one; NULL slots keep the
 * legacy engine defaults). */
/* 808 kick (BD1): DX7-curve punch envelope, EG1-driven LPF24 downsweep. */
static const seq_env_t s_808_kick_eg0 =
    { .attack_ms = 2, .decay_ms = 18, .sustain_pct = 68,
      .release_ms = 168, .eg_type = ENVELOPE_DX7 };
static const seq_env_t s_808_kick_eg1 =
    { .attack_ms = 2, .decay_ms = 19, .sustain_pct = 100,
      .release_ms = 249, .eg_type = ENVELOPE_DX7 };
static const seq_filter_t s_808_kick_flt =
    { .filter_type = SEQ_FILTER_LPF24, .cutoff_hz = 279.0f,
      .resonance = 1.75f, .enabled = true,
      .filter_env_amount = -2.0f, .feedback = 0.0f };

static const seq_drum_bank_t s_drum_banks[] = {
    /* ROM bank (gamma808) - ear-tuned */
    { .name = "808",   .first = 0,   .count = 19,
      .roles = {   0,  14,  10,   3}, .notes = { 56, 31, 71, 62},
      .eg0 = { &s_808_kick_eg0 }, .eg1 = { &s_808_kick_eg1 },
      .flt = { &s_808_kick_flt } },
    /* BD1, Snare 3, HH Open, Clap */
    { .name = "909",   .first = 256, .count = 17,
      .roles = { 256, 268, 260, 258}, .notes = { 39, 45, 53, 82} },
    /* Kick, Snare, HHC, RimShot */
    { .name = "Linn",  .first = 273, .count = 10,
      .roles = { 273, 280, 277, 279}, .notes = { 39, 45, 53, 82} },
    /* Kick, Snare, HHC, HHO */
    { .name = "MR12",  .first = 283, .count = 4,
      .roles = { 285, 286, 283, 284}, .notes = { 39, 45, 53, 82} },
    /* BD04, Static, Click, Boink */
    { .name = "SynFX", .first = 287, .count = 24,
      .roles = { 294, 289, 306, 302}, .notes = { 39, 45, 53, 82} },
    /* RealKick, Snare, HH, Clap */
    { .name = "Power", .first = 311, .count = 20,
      .roles = { 311, 314, 318, 315}, .notes = { 39, 45, 53, 82} },
    /* NoiceKick, OldSnr, Shaker */
    { .name = "Perc",  .first = 331, .count = 44,
      .roles = { 350, 351, 352, 335}, .notes = { 39, 45, 53, 82} },
    /* BassRec, Silver, Shkr, Lsr */
    { .name = "Misc",  .first = 375, .count = 17,
      .roles = { 388, 381, 376, 379}, .notes = { 39, 45, 53, 82} },
};
#define SEQ_DRUM_BANK_COUNT ((uint8_t)(sizeof(s_drum_banks) / sizeof(s_drum_banks[0])))

/* Bank owning a PCM preset number, by contiguous range; NULL when no bank
 * claims it (runtime-recorded samples, out-of-range values). */
static const seq_drum_bank_t *drum_bank_for_preset(uint16_t preset)
{
    for (uint8_t b = 0; b < SEQ_DRUM_BANK_COUNT; b++) {
        const seq_drum_bank_t *bank = &s_drum_banks[b];
        if (preset >= bank->first &&
            preset < (uint16_t)(bank->first + bank->count)) {
            return bank;
        }
    }
    return NULL;
}

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

/* Built-in PCM sample indices, one per track (kick, snare, closed-hat, clap).
 * The ROM bank and its preset numbering follow CONFIG_AMY_PCM_GAMMA808:
 * amy/src/pcm_gamma808.h pcm_map[] vs the tiny set in amy/src/pcm_tiny.h. */
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

/* Per-layer, per-track PCM preset override, lazily defaulted to
 * SEQ_DRUM_PCM_PRESET. sequencer_core_set_drum_pcm_preset() is the only writer,
 * letting a runtime-recorded sample (custompatches/sample_rec) take over one
 * track's slot without a shared-struct field. */
static uint16_t s_drum_pcm_preset[MAX_LAYERS][SEQ_TRACKS];
static bool     s_drum_pcm_preset_init[MAX_LAYERS];

/* Per-track AMY PCM wave sub-mode (PCM_PLAY / PCM_LOOP / PCM_LOOP_FOREVER...).
 * 0 = engine default (one-shot); only a nonzero value is pushed to the osc,
 * so untouched tracks keep upstream's default behavior bit-for-bit. */
static uint8_t s_drum_pcm_mode[MAX_LAYERS][SEQ_TRACKS];

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

/* Apply one track's voice params after PCM wave/preset are set. Authority per
 * block, identical to the melodic reconfigure path's deferred-authority rule:
 * user-authored vp (committed in the graph editors) > the preset's bank
 * defaults (seq_drum_bank_t eg0/eg1/flt, when the row authors them) > the
 * legacy engine defaults (EDM envelope tables above, no EG1, hat HPF). */
static void sequencer_configure_drum_pcm_track_params(uint8_t layer_idx,
                                                      uint8_t track)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    const seq_drum_bank_t *bank =
        drum_bank_for_preset(drum_pcm_preset_for(layer_idx, track));

    if (layer->vp[track].env_authored) {
        sequencer_configure_melodic_envelope_track(layer_idx, track);
    } else if (bank && bank->eg0[track]) {
        sequencer_core_push_envelope(layer->synth_id[track], bank->eg0[track]);
    } else {
        amy_event *e = amy_helpers_event_begin();
        e->synth         = layer->synth_id[track];
        e->bp_is_set[0]  = 1;
        e->eg_type[0]    = ENVELOPE_LINEAR;
        e->eg0_times[0]  = DRUM_PCM_ATK_MS[track];
        e->eg0_values[0] = 1.0f;
        e->eg0_times[1]  = DRUM_PCM_DEC_MS[track];
        e->eg0_values[1] = 0.0f;   /* one-shot: no sustain, sample shapes the body */
        e->eg0_times[2]  = DRUM_PCM_REL_MS[track];
        e->eg0_values[2] = 0.0f;
        amy_helpers_event_send(e);
    }

    /* EG1 has no legacy default: without an authored or bank shape the
     * preset reload's osc reset leaves it cleared, which is the old
     * behavior. */
    if (layer->vp[track].env1_authored) {
        sequencer_configure_melodic_envelope1_track(layer_idx, track);
    } else if (bank && bank->eg1[track]) {
        sequencer_core_push_envelope_eg1(layer->synth_id[track], 0,
                                         bank->eg1[track]);
    }

    if (layer->vp[track].filter_authored) {
        sequencer_configure_melodic_filter_track(layer_idx, track);
    } else if (bank && bank->flt[track]) {
        sequencer_core_push_filter(layer->synth_id[track], bank->flt[track],
                                   false);
    } else if (track == 2) {   /* hat: HPF strips low-end rumble, adds crispness */
        amy_event *e = amy_helpers_event_begin();
        e->synth      = layer->synth_id[track];
        e->filter_type = FILTER_HPF;
        e->filter_freq_coefs[COEF_CONST] = 3000.0f;
        e->resonance  = 0.5f;
        amy_helpers_event_send(e);
    }
}

/* Apply per-track envelope shape and hat HPF after PCM wave/preset are set. */
static void sequencer_configure_drum_pcm_voice_params(uint8_t layer_idx)
{
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        sequencer_configure_drum_pcm_track_params(layer_idx, t);
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
 * The voice count each melodic row's synth was last configured with, so a
 * chord assign/remove or slot resize can detect a no-longer-fitting count
 * without re-deriving AMY-side state. Non-static so delete_layer's compaction
 * (seq_core_state.c) can shift it with the other per-layer parallel arrays. */
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

/* Configure a single melodic synth slot as a bare AMY oscillator. Mirrors
 * arp_configure_wave_synth(). Envelope, filter and LFO are the caller's job.
 *
 * In native LFO mode oscs_per_voice=3 reserves the osc1 (LFO carrier) and osc2
 * (wobble) INDEX slots so toggling the LFO never forces a pool resize; their
 * ~532 B/osc structs stay unallocated until an LFO is authored (lazy
 * materialization, see voice_config.h). */
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
    /* osc 1 (LFO carrier): only re-park it when it may already exist, i.e. it
     * survived a same-shape rebuild. On a fresh pool the event itself would
     * allocate the osc and forfeit the lazy reservation. */
    if (voice_lfo_siblings_materialized(synth_id)) {
        amy_event *e = amy_helpers_event_begin();
        e->synth                 = synth_id;
        e->osc                   = 1;
        e->amp_coefs[COEF_CONST] = 0.0f;  /* dormant */
        amy_helpers_event_send(e);
    }
#endif
}

/* Push each AUTHORED row's stored envelope to its own per-row synth.
 * Deferred authority: a row's envelope only overrides the patch's own envelope
 * once the user has committed it in the graph editor (env_authored[t]==true).
 * Unauthored rows are left alone so the freshly-loaded patch envelope plays. */
static void sequencer_configure_melodic_envelope(uint8_t layer_idx)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    /* Raw-wave primitives carry no patch envelope. With none pushed, the
     * carrier's COEF_EG0 stays 1.0: AMY reads an empty breakpoint set as a
     * permanently open gate (envelope.c) and a velocity-0 note-off never zeroes
     * it (amy.c), so the oscillator rings forever - surviving patch changes and
     * pause, which both silence voices via that same note-off. Force the
     * default envelope onto every such unauthored row. (KS/NOISE also get their
     * sustain floor in sequencer_configure_melodic_envelope_track.) */
    bool force_wave = sequencer_core_is_wave_patch(layer->patch);
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (layer->vp[t].env_authored || force_wave) {
            sequencer_configure_melodic_envelope_track(layer_idx, t);
        }
    }
}

/* Push each AUTHORED row's stored EG1 to its own synth. No "force" case unlike
 * EG0 above: EG1 has no raw-wave fallback role. */
static void sequencer_configure_melodic_envelope1(uint8_t layer_idx)
{
    const seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
        if (layer->vp[t].env1_authored) {
            sequencer_configure_melodic_envelope1_track(layer_idx, t);
        }
    }
}

/* Push the filter for every authored row in a layer (after a patch reload).
 * Unauthored rows keep whatever filter the patch string baked in (Juno/DX7
 * patches carry a G/F/R block), unless CONFIG_SEQ_MELODIC_DISABLE_DEFAULT_LPF
 * is set, which strips it so the raw patch tone is heard. */
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

/* The melodic envelope is stored PER ROW. Each row owns its own AMY synth slot
 * (synth_id[track]), so there is no shared synth and no "active row" to
 * arbitrate. Single point of truth for "which env applies to (layer,track)";
 * per-step support would add a step parameter here, callers unchanged. */
seq_env_t *seq_layer_env(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers) layer_idx = 0;
    if (track >= SEQ_TRACKS) track = 0;
    return &s_layers[layer_idx].vp[track].env;
}

/* EG1 counterpart of seq_layer_env(). */
seq_env_t *seq_layer_env1(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers) layer_idx = 0;
    if (track >= SEQ_TRACKS) track = 0;
    return &s_layers[layer_idx].vp[track].env1;
}

/* AMY events are emitted through the shared amy_helpers scratch buffer - one
 * module-level event + mutex for all first-party callers, all of which are
 * FreeRTOS tasks, never ISRs. */

/* Apply one routable patch (0..SEQ_PATCH_FULL_MAX) to a synth slot. The SINGLE
 * kind dispatch for every consumer (melodic tracks and the arp; the drone has
 * its own excitation model):
 *   raw wave / wavetable  -> direct oscillator config (no patch string)
 *   bass preset (264-266) -> bass_preset_configure_track (oscs_per_voice=2)
 *   FM/ALGO (272-276)     -> fm preset / live-editable custom voice (7 oscs)
 *   additive (277-279)    -> additive preset / custom voice (N+1 oscs)
 *   everything else       -> amy_send_patch() string loader
 * Returns true when a patch STRING was loaded: those carry global EQ/chorus
 * commands, so the caller owes one synth_ui_fx_reassert_global() afterwards,
 * even when applying to several slots. */
/* Clamp a string-patch load's polyphony so oscs_per_voice x voices fits the
 * per-track osc budget. Built-in piano is 25 oscs/voice: a layer-wide load at
 * 4 voices/track attempts 400 oscs against the 250-osc pool AND ~76 KB of
 * internal heap, exhausting ram_caps_events partway (every osc's synthinfo
 * lives there) - the 2026-08-07 incident. Degrading polyphony keeps the patch
 * usable and the heap intact. amy_patch_oscs_per_voice() returns 0 for
 * unknown/virtual numbers - no clamp, the loader rejects those itself. */
static uint16_t seq_clamp_patch_voices(uint16_t patch, uint16_t num_voices)
{
    uint16_t opv = amy_patch_oscs_per_voice(patch);
    if (opv == 0) return num_voices;
    uint16_t max_voices = SEQ_TRACK_OSC_BUDGET / opv;
    if (max_voices < 1) max_voices = 1;
    if (num_voices > max_voices) {
        ESP_LOGW(TAG, "patch %u: %u oscs/voice, clamping %u -> %u voices "
                      "(budget %u oscs/track)",
                 (unsigned)patch, (unsigned)opv, (unsigned)num_voices,
                 (unsigned)max_voices, (unsigned)SEQ_TRACK_OSC_BUDGET);
        return max_voices;
    }
    return num_voices;
}

static bool sequencer_apply_patch_kind(uint8_t synth_id, uint16_t patch,
                                       uint16_t num_voices, uint32_t synth_flags,
                                       bool filter_authored, float ks_feedback)
{
    /* Virtual patch whose feature is compiled out (browse skips these, but
     * stored/programmatic values still arrive): snap to raw SINE rather than
     * letting a virtual number reach the string loader. */
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
    /* Bass presets participate in the reserved-LFO-pair contract (they register
     * their own pool shape); every OTHER branch below configures a foreign osc
     * topology, voiding the lazy-LFO shape proof (see voice_config.h). */
    if (patch >= SEQ_PATCH_BASS_BASE && patch <= SEQ_PATCH_BASS_MAX) {
        bass_preset_configure_track(synth_id, patch, num_voices);
        return false;
    }
#if CONFIG_SYNTH_CUSTOM_FM
    if (patch >= SEQ_PATCH_FM_BASE && patch <= SEQ_PATCH_FM_MAX) {
        voice_lfo_mark_foreign(synth_id);
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
        voice_lfo_mark_foreign(synth_id);
        if (patch == SEQ_PATCH_ADDITIVE_CUSTOM) {
            additive_voice_configure_track(synth_id, num_voices, &s_additive_voice);
        } else {
            additive_preset_configure_track(synth_id, patch, num_voices);
        }
        return false;
    }
#endif
    voice_lfo_mark_foreign(synth_id);
    amy_send_patch(synth_id, patch,
                   seq_clamp_patch_voices(patch, num_voices), synth_flags);
    return true;
}

/* Apply one patch to one slot, returning whether an FX reassert is owed.
 * Batch callers apply to several slots and must flush exactly once after. */
static bool seq_apply_patch(uint8_t synth_id, uint16_t patch,
                            uint16_t num_voices, uint32_t synth_flags,
                            bool filter_authored, float ks_feedback)
{
    return sequencer_apply_patch_kind(synth_id, patch, num_voices,
                                      synth_flags, filter_authored, ks_feedback);
}

/* Reassert global FX iff a patch STRING was applied since the last flush:
 * those carry global EQ/chorus commands that overwrite the user's FX state.
 * Raw-wave, bass and FM patches owe nothing. Every patch-load path flushes
 * through here so no caller can forget the reassert. */
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
            /* PCM mode: each track's slot becomes a 1-osc PCM player - allocate
             * with oscs_per_voice=1 (no patch string), then set wave=PCM +
             * preset on osc 0. Note-on/off, velocity and pitch flow through the
             * SAME emit path as synth mode, so hits keep accent/jitter dynamics
             * and midi_note tunes the sample (render_pcm). PCM carries no
             * global EQ/chorus, so no reassert is owed. */
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
                if (s_drum_pcm_mode[layer_idx][t])
                    e->mode = s_drum_pcm_mode[layer_idx][t];
                amy_helpers_event_send(e);
            }
            sequencer_configure_drum_pcm_voice_params(layer_idx);
            return;
        }

        /* SYNTH mode: each drum row loads its OWN patch onto its OWN slot,
         * note-offs honored (flags = 0). Mirrors the melodic loop. */
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            sequencer_kill_synth_voices(layer->synth_id[t]);
            amy_send_patch(layer->synth_id[t], layer->track_patch[t],
                           layer->num_voices, layer->synth_flags);
        }
        /* Every drum SYNTH slot loads a patch string, so a flush is always
         * owed - once, after the loop. */
        seq_flush_patch_fx(true);
        return;
    }

    /* Melodic: push the shared patch/flags to each row's own synth. Voice count
     * is per-track - layer->num_voices, widened to the chord tone count on rows
     * carrying a chord preset, so voices are spent only where chords play. */
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
    /* Glide is a per-osc AMY setting that a voice rebuild clears - reassert. */
    sequencer_core_push_melodic_portamento(layer_idx);
}

/* ── Public API — melodic patch ─────────────────────────────────────── */

/* Reconfigure a layer's synths with its scheduled events paused.
 *
 * Grid steps are repeating events inside AMY's sequencer, fired autonomously.
 * Leaving them live while a patch load rebuilds the voice/osc tables lets a
 * note-on resolve against half-updated mappings and strand an osc AUDIBLE
 * outside any voice, where no later kill can reach it (kills resolve through
 * the CURRENT voice map) - the intermittent "ringing that survives patch
 * changes". Same discipline as arp_rebuild(): clear the schedule, rebuild
 * (configure MUST kill sounding voices per track, their note-offs were just
 * cleared too), re-emit. sequencer_emit_step() is s_playing-gated, so the
 * resync no-ops while paused. Cost: the layer goes quiet for the events cleared
 * mid-flight; the re-emit restores absolute tick positions, keeping groove
 * phase. Also the reconfigure path for per-track voice-count changes (chord
 * assign/remove, slot resize) - see seq_core_internal.h. */
void sequencer_reconfigure_layer_paused(uint8_t layer_idx)
{
    sequencer_clear_layer_tags(layer_idx);
    /* Drop the trig engine's pending one-shots (ratchet + chord tone pairs)
     * too: a sub-hit scheduled moments ago must not resolve against the
     * half-rebuilt pool - same stranded-osc mechanism as above. */
    sequencer_core_trig_clear_all(layer_idx);
    sequencer_configure_synth(layer_idx);
    sequencer_resync_layer(layer_idx);
}

void sequencer_core_set_melodic_patch(uint16_t patch_number)
{
    /* 0..127 Juno, 128..255 DX7, 256 piano, 257..263 raw waves, 264..266 bass
     * presets, 267..271 wavetables (AMY_WAVETABLE only), 272..276 FM/ALGO,
     * 277..279 additive - SEQ_PATCH_ADDITIVE_MAX is the true ceiling. */
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

/* Per-layer patch access: one melodic layer, unlike
 * sequencer_core_set_melodic_patch which hits them all. Lets the UI
 * patch-cycle widget step each layer independently. */

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
    s_melodic_patch = patch_number;  /* the global accessor doubles as the
                                        display fallback and must track the
                                        last-touched layer (contract in
                                        seq_core_internal.h) */
    sequencer_reconfigure_layer_paused(layer_idx);
    ESP_LOGI(TAG, "L%u patch -> %u", (unsigned)layer_idx + 1u, (unsigned)patch_number);
}

/* Re-apply the layer's patch and every authored parameter to its synth slots.
 * The exact-state restore for cancelling a live preview on a never-authored
 * row, whose state came from the patch and cannot be re-pushed from the store. */
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

/* Set one drum track's patch (expected to be a curated-list member) and reload
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

    /* In PCM mode the slot is a PCM player: store the selection for the switch
     * back to SYNTH, but pushing a patch load now would clobber the PCM osc. */
    if (s_drum_engine == SEQ_DRUM_PCM) {
        ESP_LOGI(TAG, "drum L%u T%u patch -> %u (stored; PCM active)",
                 layer_idx + 1u, track + 1u, (unsigned)patch_number);
        return;
    }

    /* Reload only this track's slot, with the layer's schedule paused around
     * the load (see sequencer_reconfigure_layer_paused for why). */
    sequencer_clear_layer_tags(layer_idx);
    sequencer_kill_synth_voices(layer->synth_id[track]);
    amy_send_patch(layer->synth_id[track], patch_number,
                   seq_clamp_patch_voices(patch_number, layer->num_voices),
                   layer->synth_flags);
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

/* Step one drum track's patch `dir` entries through the curated list, wrapping.
 * Returns the newly-applied patch number. */
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

    /* Reconfigure every drum layer's slots in place. Grid/velocity/pitch and
     * scheduled events are untouched, so the pattern keeps playing. */
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
        /* Reset the osc before reconfiguring, so any stray coefficient state
         * (a software-LFO push, filter residue) is wiped on every preset
         * change instead of surviving until the next full re-alloc. reset_osc
         * is voice-relative when synth is set. */
        amy_event *e = amy_helpers_event_begin();
        e->synth     = s_layers[layer_idx].synth_id[track];
        e->reset_osc = 0;
        amy_helpers_event_send(e);

        /* Live-reload just this track's osc. */
        e = amy_helpers_event_begin();
        e->synth  = s_layers[layer_idx].synth_id[track];
        e->osc    = 0;
        e->wave   = PCM;
        e->preset = preset_number;
        if (s_drum_pcm_mode[layer_idx][track])
            e->mode = s_drum_pcm_mode[layer_idx][track];
        amy_helpers_event_send(e);

        /* The reset also cleared the envelope and hat HPF - re-apply. */
        sequencer_configure_drum_pcm_track_params(layer_idx, track);
    }
    ESP_LOGI(TAG, "drum L%u T%u PCM preset -> %u",
             layer_idx + 1u, track + 1u, (unsigned)preset_number);
}

void sequencer_core_set_drum_pcm_mode(uint8_t layer_idx, uint8_t track,
                                      uint8_t pcm_mode)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    if (s_layers[layer_idx].type != SEQ_LAYER_DRUM) return;
    if (s_drum_pcm_mode[layer_idx][track] == pcm_mode) return;

    s_drum_pcm_mode[layer_idx][track] = pcm_mode;

    /* Mode is applied together with wave/preset on the osc (AMY assigns MODE
     * and PRESET as one block), so a live change rides the preset reload
     * path - which also re-applies envelope and hat HPF after its reset. */
    if (s_drum_engine == SEQ_DRUM_PCM) {
        sequencer_core_set_drum_pcm_preset(layer_idx, track,
                                           drum_pcm_preset_for(layer_idx, track));
    }
}

uint8_t sequencer_core_get_drum_pcm_mode(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    return s_drum_pcm_mode[layer_idx][track];
}

uint16_t sequencer_core_cycle_drum_pcm_preset(uint8_t layer_idx, uint8_t track,
                                              int dir)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    if (s_layers[layer_idx].type != SEQ_LAYER_DRUM) return 0;

    /* Combined drum sample domain: the ROM bank (0 .. pcm_wavetable_base-1,
     * that base being the first non-drum map entry) then the gamma9001 banks
     * (256..391) when their blob is mounted, walked as one wrapping list via a
     * linear index. Memory presets (sample_rec overrides, outside both ranges)
     * step back into the domain at the near end. */
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

    /* Seed every drum layer's tracks with the bank's role defaults - preset
     * AND pitch (the bank's notes[] are ear-tuned per kit; keeping the old
     * track pitch across a kit change mispitches samples whose useful range
     * differs, e.g. bass drums seeded inaudibly low). set_drum_pcm_preset
     * live-reloads each osc when PCM is already active; the engine switch
     * below covers the Synth->PCM case. set_track_midi_note is the full
     * user-edit path (clamp, step rewrite, live re-emit, preview), so a bank
     * pick also auditions the new kit at its seeded pitches. Preset first:
     * the note preview must sound the new sample. */
    for (uint8_t i = 0; i < s_num_layers; i++) {
        if (s_layers[i].type != SEQ_LAYER_DRUM) continue;
        for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
            sequencer_core_set_drum_pcm_preset(i, t, b->roles[t]);
            sequencer_core_set_track_midi_note(i, t, b->notes[t]);
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

/* Push the layer's glide time to each row's slot. Mirrors arp_push_portamento():
 * a bare portamento_ms event (no velocity) fans out to every voice's base osc,
 * where AMY applies portamento_alpha as a logfreq low-pass. 0 ms = off.
 * Melodic-only config path; drum layers never call this. */
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
    /* Full catalog, same kind dispatch as melodic. */
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_FULL_MAX);
    /* Osc topology changes between kinds, so kill sounding voices before the
     * pool is rebuilt. */
    sequencer_kill_synth_voices(SEQ_ARP_SYNTH);
    bool string_patch = seq_apply_patch(SEQ_ARP_SYNTH, patch_number,
                                        num_voices, 0,
                                        filter_authored, ks_feedback);
    seq_flush_patch_fx(string_patch);
    ESP_LOGI(TAG, "arp synth %u patch -> %u (%u voices)",
             (unsigned)SEQ_ARP_SYNTH, (unsigned)patch_number, (unsigned)num_voices);
}

/* The AMY synth slot backing a melodic row. Exposed so UI code addressing the
 * row's real voices (the live filter overlay) need not duplicate the
 * layer/track -> slot mapping, which is assigned at layer build time and is not
 * derivable from the indices. Returns 0 out of range - never a melodic slot,
 * those start at SEQ_MEL_SYNTH_BASE - so 0 means "no such row". */
uint8_t sequencer_core_get_track_synth(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= MAX_LAYERS || track >= SEQ_TRACKS) {
        return 0;
    }
    return s_layers[layer_idx].synth_id[track];
}

void sequencer_core_configure_synth_slot(uint8_t synth_id, uint16_t patch_number,
                                         uint8_t num_voices)
{
    patch_number = SEQ_CLAMP_U16(patch_number, 0, SEQ_PATCH_FULL_MAX);
    /* Kill sounding voices before the pool is rebuilt (as in the arp path). */
    sequencer_kill_synth_voices(synth_id);
    bool string_patch = seq_apply_patch(synth_id, patch_number, num_voices,
                                        0, false, 0.0f);
    seq_flush_patch_fx(string_patch);
    ESP_LOGI(TAG, "synth %u patch -> %u (%u voices)",
             (unsigned)synth_id, (unsigned)patch_number, (unsigned)num_voices);
}

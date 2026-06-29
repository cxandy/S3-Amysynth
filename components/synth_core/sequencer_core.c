#include "sequencer_core.h"
#include "custompatches/bass_presets.h"
#include "arp_core.h"
#include "amy.h"
#include "amy_helpers.h"
#include "sequencer.h"
#include "quantizer.h"
#include "seq_clamp.h"
#include "seq_defaults.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>
#include "freertos/semphr.h"

/* Defined in synth_ui.c (same component). Re-imposes the user's cached global
 * FX after a patch load so a preset's trailing global EQ/chorus commands don't
 * leak across synths. Forward-declared here to avoid pulling the u8g2/display
 * headers (synth_ui.h) into the audio core. */
void synth_ui_fx_reassert_global(void);

/* DEBUG: bisect heap corruption inside core init. Gated by
 * CONFIG_AMYSYNTH_HEAP_CHECK; compiles to nothing when off (default). */
#if CONFIG_AMYSYNTH_HEAP_CHECK
#define CORE_HEAP_CHECK(where) do { \
    if (!heap_caps_check_integrity_all(true)) { \
        ESP_LOGE(TAG, "HEAP CORRUPT detected at: %s", where); \
    } else { \
        ESP_LOGI(TAG, "HEAP OK at: %s", where); \
    } \
} while (0)
#else
#define CORE_HEAP_CHECK(where) do { (void)(where); } while (0)
#endif

#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED
#define CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED 1
#endif
#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE
#define CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE 60
#endif
#ifndef CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE
#define CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE 0
#endif
#ifndef CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
#define CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS 1
#endif
#ifndef CONFIG_SEQ_MELODIC_GATE_NUMERATOR
#define CONFIG_SEQ_MELODIC_GATE_NUMERATOR 5
#endif
#ifndef CONFIG_SEQ_MELODIC_GATE_DENOMINATOR
#define CONFIG_SEQ_MELODIC_GATE_DENOMINATOR 6
#endif
#ifndef CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED
#define CONFIG_SEQ_MELODIC_ENVELOPE_ENABLED 1
#endif
#ifndef CONFIG_SEQ_MELODIC_PATCH
#define CONFIG_SEQ_MELODIC_PATCH 138
#endif
#ifndef CONFIG_SEQ_DRUM_GATE_NUMERATOR
#define CONFIG_SEQ_DRUM_GATE_NUMERATOR 1
#endif
#ifndef CONFIG_SEQ_DRUM_GATE_DENOMINATOR
#define CONFIG_SEQ_DRUM_GATE_DENOMINATOR 2
#endif
#ifndef CONFIG_SEQ_ENV_DEBUG_DUMP
#define CONFIG_SEQ_ENV_DEBUG_DUMP 0
#endif

static const char *TAG = "seq_core";

extern uint32_t sequencer_ticks(void);

/* ── Timing ──────────────────────────────────────────────────────────── */
#define SEQ_TICKS_PER_STEP    (AMY_SEQUENCER_PPQ / 4)
/* A musical bar = 16 steps. Fixed regardless of layer length so the bar
 * counter and repeat-rate are independent of which layers are active. */
#define SEQ_TICKS_PER_BAR     (16u * SEQ_TICKS_PER_STEP)
/* Drum gate: fraction of a step the note is held before its note-off. Now that
 * drums are real Juno patches (note-offs honored), this controls choke vs. ring;
 * the patch's own release tail still plays out after note-off. Tunable via
 * Kconfig (default 1/2 step). */
#define SEQ_GATE_DRUM         ((SEQ_TICKS_PER_STEP * CONFIG_SEQ_DRUM_GATE_NUMERATOR) / CONFIG_SEQ_DRUM_GATE_DENOMINATOR)
#if CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
#define SEQ_GATE_MELODIC      ((SEQ_TICKS_PER_STEP * CONFIG_SEQ_MELODIC_GATE_NUMERATOR) / CONFIG_SEQ_MELODIC_GATE_DENOMINATOR)
#else
#define SEQ_GATE_MELODIC      ((SEQ_TICKS_PER_STEP * 2) / 3)
#endif
#define SEQ_MIN_BPM           40
#define SEQ_MAX_BPM           300
/* SEQ_DEFAULT_BPM is declared in sequencer_core.h (shared with synth_ui). */
/* ── Drum synth slots ────────────────────────────────────────────────────
 * Drums are now a per-track Juno-patch layer: each of the 4 tracks loads its
 * own AMY patch and gets its own synth slot, exactly like melodic rows. We
 * reserve a fixed block 6..9 (below the melodic base of 11) so the melodic
 * running allocator (11..62) and the arp slot (63) are untouched. */
#define SEQ_DRUM_SYNTH_BASE   6
#define SEQ_DRUM_VOICES       1  /* one voice per row; a row sounds one pitch at
                                  * a time (matches melodic). */
/* Drum tracks now play real pitches into a tonal patch, so clamp to the same
 * musical range as melodic rows rather than the old GM-drum note span. */
#define SEQ_MIDI_NOTE_MIN     24    /* C1 */
#define SEQ_MIDI_NOTE_MAX     96    /* C7 */

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

/* Built-in 808 PCM sample indices (from amy/src/pcm_tiny.h pcm_map[]) used by
 * PCM drum mode, one per track: kick, snare, closed-hat, clap. */
static const int16_t SEQ_DRUM_PCM_PRESET[SEQ_TRACKS] = {
    1,    /* track 0: [1] 808-KIK 4-D    */
    2,    /* track 1: [2] 808-SNR 4-D    */
    6,    /* track 2: [6] 808-C-HAT1-D   */
    9,    /* track 3: [9] 808-DRYCLP-D   */
};

/* ── Melodic synth defaults ──────────────────────────────────────────── */
#define SEQ_MEL_PATCH         CONFIG_SEQ_MELODIC_PATCH
/* Wave-patch ID constants (SEQ_PATCH_WAVE_BASE, SEQ_PATCH_WAVE_MAX, etc.)
 * are now in sequencer_core.h so arp_core and drone_core can use them. */
/* One AMY synth PER ROW (per track). A row only ever sounds one pitch at a
 * time, so a single voice suffices; bump to 2 to give note-off/note-on overlap
 * headroom at the boundary (2x osc cost). AMY default budget is 250 oscs. */
#define SEQ_MEL_VOICES        1
#define SEQ_MEL_SYNTH_BASE    11    /* first melodic synth slot (drum = 10) */
#define SEQ_MAX_SYNTH         62    /* melodic ceiling; slot 63 reserved for arp */
#define SEQ_MEL_NOTE_MIN      24    /* C1 */
#define SEQ_MEL_NOTE_MAX      96    /* C7 */

/* ── Arpeggiator synth slot ──────────────────────────────────────────────
 * Dedicated AMY slot for the standalone arp, reserved above the melodic
 * ceiling so it never collides with a melodic layer's per-row block. */
#define SEQ_ARP_SYNTH         63
#define SEQ_ARP_VOICES        4     /* allow note overlap at fast rates       */

/* One-shot preview fires this many ticks after an adjustment */
#define SEQ_PREVIEW_DELAY_TICKS 4

/* ── State ───────────────────────────────────────────────────────────── */
static seq_layer_t s_layers[MAX_LAYERS];
static uint8_t     s_num_layers   = 0;
static uint8_t     s_cached_step[MAX_LAYERS];
static bool        s_playing      = true;
static uint16_t    s_bpm          = SEQ_DEFAULT_BPM;
/* Drum sound source for the whole drum layer. SYNTH = tonal AMY patches (Juno/
 * DX7) per track; PCM = built-in 808 samples per track. Switchable at runtime;
 * changing it re-configures the drum layer's synth slots in place. */
static seq_drum_engine_t s_drum_engine = SEQ_DRUM_PCM;
static uint16_t    s_melodic_patch = SEQ_MEL_PATCH;
/* Running allocator for per-row melodic synth slots. Each melodic layer claims
 * a contiguous block of SEQ_TRACKS slots starting here; reset in core_init. */
static uint8_t     s_next_melodic_synth = SEQ_MEL_SYNTH_BASE;
static uint8_t     s_track_source_note[MAX_LAYERS][SEQ_TRACKS];
static quantizer_state_t s_quantizer = {
    .root_note  = CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE,
    .scale_index = CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE,
    .enabled    = CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED,
};

/* ── Bar counter ─────────────────────────────────────────────────────────
 * sequencer_ticks() is monotonic (never resets on play/stop in normal use).
 * Capture a baseline at play-start; compute bars elapsed from the delta. */
static uint32_t s_bar_baseline = 0;

static inline uint32_t sequencer_bars_elapsed(void)
{
    uint32_t t = sequencer_ticks();
    if (t < s_bar_baseline) return 0;
    return (t - s_bar_baseline) / SEQ_TICKS_PER_BAR;
}

/* ── Global chord progression ────────────────────────────────────────────── */
#define CHORD_PROG_MAX_ENTRIES 8

typedef struct {
    uint8_t      root;           /* chromatic pitch class 0-11 */
    chord_type_t chord_type;
    uint8_t      duration_bars;  /* 1 / 2 / 4 / 8 / 16 */
} chord_prog_entry_t;

typedef struct {
    chord_prog_entry_t entries[CHORD_PROG_MAX_ENTRIES];
    uint8_t            count;
    uint8_t            current;
    uint32_t           entry_start_bar; /* bars_elapsed when current entry began */
    bool               enabled;
} chord_progression_t;

static chord_progression_t s_prog = {
    .entries = {
        { .root = 0, .chord_type = CHORD_MAJ7, .duration_bars = 4 },
    },
    .count   = 1,
    .current = 0,
    .entry_start_bar = 0,
    .enabled = false,
};

/* Set by input-task entry points (encoder_task / button callback) that change
 * chord state; consumed once per tick by sequencer_core_progression_service()
 * which runs in synth_ui_task. This makes synth_ui_task the SINGLE task that
 * calls chord_progression_apply_current() -> sequencer_refresh_melodic_layers()
 * -> AMY emit, so progression/manual-chord edits never race the periodic
 * advance or each other across tasks. */
static volatile bool s_prog_apply_pending = false;

/* ── Software LFO state (phase accumulator, per-layer/track) ── */
static float    s_lfo_phase[MAX_LAYERS][SEQ_TRACKS]; /* 0..1 normalized */
static float    s_lfo_hz[MAX_LAYERS][SEQ_TRACKS];    /* Hz from rate+BPM */
static float    s_lfo_rnd[MAX_LAYERS][SEQ_TRACKS];   /* S&H held value   */
static uint32_t s_lfo_rng_state = 0xDEADBEEFu;

static void sequencer_refresh_track_note(uint8_t layer_idx, uint8_t track,
                                        bool preview);
static void sequencer_emit_step(uint8_t layer_idx, uint8_t track, uint8_t step);
static inline uint32_t seq_preview_tag(uint8_t layer, uint8_t track);
static inline uint32_t seq_preview_off_tag(uint8_t layer, uint8_t track);

static float sequencer_step_velocity(const seq_layer_t *layer,
                                     uint8_t track, uint8_t step)
{
    /* Drums now share the melodic accent+jitter curve for a less "machine-gun"
     * groove (the old fixed 1.0 made every hit identical). Falls through to the
     * same expressive path as melodic below. */
    (void)layer;

#if !CONFIG_SEQ_MELODIC_EXPRESSIVE_DEFAULTS
    (void)track;
    (void)step;
    return 1.0f;
#else

    /* With the EG0 envelope now shaping onset/tail, we can use a wider dynamic
     * range without the notes sounding dull — the accent pattern provides the
     * groove that breaks up the old "machine-gun" monotony. Base level sits
     * mid-range so accents have room to push up and ghost notes can drop down.
     * Tracks are spread slightly so stacked voices don't all hit identically. */
    float velocity = 0.62f + (0.02f * (float)track);

    /* Metric accents: strong downbeat, lighter backbeat, weak off-beats. */
    if ((step % 4) == 0) {
        velocity += 0.30f; /* downbeat of each quarter-note */
    } else if ((step % 4) == 2) {
        velocity += 0.16f; /* backbeat emphasis */
    } else {
        velocity -= 0.04f; /* the in-between 8ths sit back as ghost notes */
    }

    /* Deterministic per-step jitter so repeated bars are not bit-identical
     * (light "humanization"). Cycles every 4 steps with a small +/- swing. */
    static const float jitter[4] = { 0.015f, -0.02f, 0.01f, -0.015f };
    velocity += jitter[step & 3];

    velocity = SEQ_CLAMP_F32(velocity, 0.45f, 1.0f);
    return velocity;
#endif
}

/* AMY events are emitted through the shared amy_helpers scratch buffer (see
 * amy_helpers.{c,h}) — one module-level event + mutex for all first-party
 * callers, all of which are FreeRTOS tasks (never ISRs). */

/* The melodic envelope is stored PER ROW (per track). Each row now owns its own
 * AMY synth slot (synth_id[track]), so every row holds its own independent live
 * envelope — there is no shared synth and no "active row" to arbitrate. This
 * accessor is the single point of truth for "which env applies to (layer,track,
 * step)". For per-step support later, add a step parameter and index a wider
 * env[][] array here — callers stay unchanged. */
static seq_env_t *seq_layer_env(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers) layer_idx = 0;
    if (track >= SEQ_TRACKS) track = 0;
    return &s_layers[layer_idx].env[track];
}

/* Push the given row's stored envelope to that row's OWN AMY synth. */
static void sequencer_configure_melodic_envelope_track(uint8_t layer_idx, uint8_t track)
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

static uint8_t sequencer_clamp_layer_note(const seq_layer_t *layer, uint8_t note)
{
    if (layer->type == SEQ_LAYER_DRUM) {
        return SEQ_CLAMP_U8(note, SEQ_MIDI_NOTE_MIN, SEQ_MIDI_NOTE_MAX);
    } else {
        return SEQ_CLAMP_U8(note, SEQ_MEL_NOTE_MIN, SEQ_MEL_NOTE_MAX);
    }
}

static uint8_t sequencer_resolve_track_note(const seq_layer_t *layer,
                                            uint8_t source_note)
{
    if (layer->type != SEQ_LAYER_MELODIC) {
        return sequencer_clamp_layer_note(layer, source_note);
    }

    /* Chord mode overrides the global scale quantizer for this layer. */
    if (layer->chord_mode) {
        uint8_t snapped = quantizer_snap_to_chord(source_note,
                                                  layer->chord_root,
                                                  layer->chord_type);
        return sequencer_clamp_layer_note(layer, snapped);
    }

    if (!s_quantizer.enabled) {
        return sequencer_clamp_layer_note(layer, source_note);
    }

    const musical_scale_t *scale = quantizer_get_scale(s_quantizer.scale_index);
    uint8_t snapped = quantizer_snap_midi_note(source_note, s_quantizer.root_note, scale);
    return sequencer_clamp_layer_note(layer, snapped);
}

static void sequencer_refresh_melodic_layers(bool preview)
{
    for (uint8_t layer_idx = 0; layer_idx < s_num_layers; layer_idx++) {
        seq_layer_t *layer = &s_layers[layer_idx];
        if (layer->type != SEQ_LAYER_MELODIC) {
            continue;
        }
        for (uint8_t track = 0; track < layer->num_tracks; track++) {
            sequencer_refresh_track_note(layer_idx, track, preview);
        }
    }
}

/* Re-resolve a track's note (clamp + optional scale quantization), update every
 * step on that track to the new note, and re-emit them. When `preview` is set
 * (interactive editing) also fire a short one-shot so the user hears the note
 * immediately, even if quantization left the resolved note unchanged. */
static void sequencer_refresh_track_note(uint8_t layer_idx, uint8_t track,
                                        bool preview)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks) return;

    uint8_t source_note = s_track_source_note[layer_idx][track];
    uint8_t resolved_note = sequencer_resolve_track_note(layer, source_note);

    /* No change: skip the grid rewrite, but still preview so scrolling within
     * one scale degree remains audible. */
    if (layer->track_base_note[track] == resolved_note) {
        if (preview) {
            /* Keep the preview path active even when the snapped note does not change. */
        } else {
            return;
        }
    }

    /* Apply the resolved note to the whole track (all steps play one pitch). */
    layer->track_base_note[track] = resolved_note;
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        layer->step_note[track][s] = resolved_note;
    }

    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }

    if (!preview) {
        return;
    }

    /* One-shot preview: fires a few ticks from now using the same tag slot.
     * Rapid scrolling overwrites the slot so only the last change is heard. */
    uint32_t fire_tick = sequencer_ticks() + SEQ_PREVIEW_DELAY_TICKS;
    amy_send_note_sched(layer->synth_id[track], resolved_note, 1.0f,
                        seq_preview_tag(layer_idx, track), fire_tick, 0);
    amy_send_note_sched(layer->synth_id[track], resolved_note, 0.0f,
                        seq_preview_off_tag(layer_idx, track),
                        fire_tick + SEQ_GATE_MELODIC, 0);

    ESP_LOGI(TAG, "layer %d track %d note -> %d (preview @ tick %lu)",
             layer_idx, track, resolved_note, (unsigned long)fire_tick);
}

/* ── Tag helpers ─────────────────────────────────────────────────────── */
/*
 * Tag layout (uint32_t — tag space is effectively unlimited):
 *
 *   ON  tag = layer * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
 *             + track * SEQ_MAX_STEPS + step
 *
 *   OFF tag = ON tag + (SEQ_TRACKS * SEQ_MAX_STEPS)
 *
 *   Preview = MAX_LAYERS * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
 *             + layer * SEQ_TRACKS + track
 *
 * Per layer: 4*32*2 = 256 slots; all 4 layers occupy tags 0..1023.
 * Preview tags start at 1024.
 */
static inline uint32_t seq_tag_on(uint8_t layer, uint8_t track, uint8_t step)
{
    return (uint32_t)layer * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
         + (uint32_t)track * SEQ_MAX_STEPS
         + step;
}

static inline uint32_t seq_tag_off(uint8_t layer, uint8_t track, uint8_t step)
{
    return seq_tag_on(layer, track, step)
         + (uint32_t)(SEQ_TRACKS * SEQ_MAX_STEPS);
}

static inline uint32_t seq_preview_tag(uint8_t layer, uint8_t track)
{
    return (uint32_t)MAX_LAYERS * (SEQ_TRACKS * SEQ_MAX_STEPS * 2)
         + (uint32_t)layer * SEQ_TRACKS
         + track;
}

/* OFF tag for the preview note — occupies the block immediately after ON tags. */
static inline uint32_t seq_preview_off_tag(uint8_t layer, uint8_t track)
{
    return seq_preview_tag(layer, track) + (uint32_t)(MAX_LAYERS * SEQ_TRACKS);
}

/* ── Low-level AMY helpers ───────────────────────────────────────────── */

static void sequencer_emit_clear_tag(uint32_t tag)
{
    amy_event *e = amy_helpers_event_begin();
    e->sequence[SEQUENCE_TAG]    = tag;
    e->sequence[SEQUENCE_TICK]   = 0;
    e->sequence[SEQUENCE_PERIOD] = 0;
    amy_helpers_event_send(e);
}

static void sequencer_configure_melodic_filter(uint8_t layer_idx);  /* forward */

/* (Re)configure the AMY synth(s) for layer_idx.
 * Drums use a single synth (synth_id[0]); melodic layers configure one synth
 * per row, all sharing the same patch/flags/voice-count but on distinct slots. */
/* EDM-tuned envelope parameters for PCM drum tracks (one-shot decay, sustain=0). */
static const float DRUM_PCM_ATK_MS[SEQ_TRACKS] = {2.0f,  1.0f,  1.0f,  1.0f};
static const float DRUM_PCM_DEC_MS[SEQ_TRACKS] = {600.0f, 200.0f, 100.0f, 150.0f};
static const float DRUM_PCM_REL_MS[SEQ_TRACKS] = {50.0f,  30.0f,  15.0f,  20.0f};

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

static void sequencer_kill_synth_voices(uint8_t synth_id)
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


static void sequencer_configure_synth(uint8_t layer_idx)
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

/* Schedule (or cancel) one grid step as a pair of repeating AMY events: a
 * note-on at the step's position in the bar, and a note-off `gate` ticks later.
 * Both repeat every `bar_ticks` so the pattern loops automatically. AMY keys
 * each event by its tag, so re-emitting with the same tag updates in place. */
static void sequencer_emit_step(uint8_t layer_idx, uint8_t track, uint8_t step)
{
    seq_layer_t *layer  = &s_layers[layer_idx];
    /* Total ticks in one loop of this layer's pattern. */
    uint32_t bar_ticks  = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
    /* Repeat rate: fire every N bars. period scales bar_ticks accordingly.
     * note-off must wrap against the same period so it lands in the correct
     * half of the extended window (not just within the first bar). */
    uint32_t rr         = (layer->repeat_rate[track] >= SEQ_REPEAT_2)
                          ? (uint32_t)layer->repeat_rate[track] : 1u;
    uint32_t period     = bar_ticks * rr;
    /* How long the note is held: drums are short/percussive, melodic longer. */
    uint8_t  gate       = (layer->type == SEQ_LAYER_DRUM)
                          ? SEQ_GATE_DRUM : SEQ_GATE_MELODIC;
    /* Melodic groove: shorten the off-beat 8ths a touch so accented downbeats
     * feel longer/legato while the in-between notes are slightly detached. We
     * only ever shorten (never lengthen past SEQ_GATE_MELODIC) so the note-off
     * always lands before the next step's note-on and never cuts it off. */
    if (layer->type == SEQ_LAYER_MELODIC && (step % 2) == 1 && gate > 2) {
        gate -= 2;
    }
    uint32_t tag_on     = seq_tag_on(layer_idx, track, step);
    uint32_t tag_off    = seq_tag_off(layer_idx, track, step);
    /* +1 so tick 0 stays reserved (AMY treats tick 0 specially as "clear"). */
    uint32_t tick_on    = (uint32_t)(1 + step * SEQ_TICKS_PER_STEP);
    /* Note-off wraps within the full period (not just bar_ticks) so a note at
     * repeat_rate=2 whose gate spills past bar_ticks still fires correctly. */
    uint32_t tick_off   = (tick_on + gate) % period;
    float note_velocity = sequencer_step_velocity(layer, track, step);
    /* Apply per-track amplitude trim (default 1.0; set by graph editor amp mode). */
    note_velocity *= layer->amp_scale[track];
    if (note_velocity > 1.0f) note_velocity = 1.0f;
    if (tick_off == 0) tick_off = 1; /* avoid the reserved tick 0 */

    /* If stopped or this step is off, cancel any previously scheduled events. */
    if (!s_playing || !layer->grid[track][step]) {
        sequencer_emit_clear_tag(tag_on);
        sequencer_emit_clear_tag(tag_off);
        return;
    }

    /* Both drum and melodic layers now have one synth slot per track. */
    uint8_t synth = layer->synth_id[track];

    amy_send_note_sched(synth, layer->step_note[track][step], note_velocity,
                        tag_on, tick_on, period);
    amy_send_note_sched(synth, layer->step_note[track][step], 0.0f,
                        tag_off, tick_off, period);
}

/* Re-emit all steps for a layer (used on play-resume). */
static void sequencer_resync_layer(uint8_t layer_idx)
{
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < layer->num_tracks; t++) {
        for (uint8_t s = 0; s < layer->num_steps; s++) {
            sequencer_emit_step(layer_idx, t, s);
        }
    }
}

/* Cancel all scheduled tags for a layer (used on pause). */
static void sequencer_clear_layer_tags(uint8_t layer_idx)
{
    seq_layer_t *layer = &s_layers[layer_idx];
    for (uint8_t t = 0; t < layer->num_tracks; t++) {
        for (uint8_t s = 0; s < layer->num_steps; s++) {
            sequencer_emit_clear_tag(seq_tag_on(layer_idx, t, s));
            sequencer_emit_clear_tag(seq_tag_off(layer_idx, t, s));
        }
    }
}

/* ── BPM helpers ─────────────────────────────────────────────────────── */

static uint16_t sequencer_clamp_bpm(uint16_t b)
{
    return SEQ_CLAMP_U16(b, SEQ_MIN_BPM, SEQ_MAX_BPM);
}

static void sequencer_push_tempo(uint16_t b)
{
    amy_event *e = amy_helpers_event_begin();
    e->tempo = b;
    amy_helpers_event_send(e);
}

/* ── LFO helpers ─────────────────────────────────────────────────────── */

float lfo_rate_to_hz(lfo_rate_t rate, uint16_t bpm)
{
    float b = (float)bpm;
    switch (rate) {
        case LFO_RATE_1_8:  return b / 30.0f;    /* 1/8 note */
        case LFO_RATE_1_4:  return b / 60.0f;    /* 1/4 note */
        case LFO_RATE_1_2:  return b / 120.0f;   /* 1/2 note */
        case LFO_RATE_1BAR: return b / 240.0f;   /* 1 bar (4/4) */
        case LFO_RATE_2BAR: return b / 480.0f;
        case LFO_RATE_4BAR: return b / 960.0f;
        default:            return b / 240.0f;
    }
}

static float lfo_next_rand(void)
{
    s_lfo_rng_state ^= s_lfo_rng_state << 13;
    s_lfo_rng_state ^= s_lfo_rng_state >> 17;
    s_lfo_rng_state ^= s_lfo_rng_state << 5;
    return (float)(s_lfo_rng_state >> 17) / 32767.0f * 2.0f - 1.0f;
}

static void lfo_push_target_neutral(uint8_t synth_id, lfo_target_t target)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth = synth_id;
    switch (target) {
        case LFO_TARGET_FILTER: e->filter_freq_coefs[COEF_CONST] = 8000.0f; break;
        case LFO_TARGET_AMP:    e->amp_coefs[COEF_CONST]  = 1.0f;           break;
        case LFO_TARGET_PITCH:  e->freq_coefs[COEF_CONST] = 1.0f;           break;
        case LFO_TARGET_PAN:    e->pan_coefs[COEF_CONST]  = 0.5f;           break;
        default: break;
    }
    amy_helpers_event_send(e);
}

/* ── Public API ──────────────────────────────────────────────────────── */

void sequencer_core_init(void)
{
    amy_helpers_init();
    s_num_layers = 0;
    s_next_melodic_synth = SEQ_MEL_SYNTH_BASE;
    memset(s_layers, 0, sizeof(s_layers));
    memset(s_cached_step, 0, sizeof(s_cached_step));
    memset(s_track_source_note, 0, sizeof(s_track_source_note));
    s_playing = true;
    s_bpm     = SEQ_DEFAULT_BPM;
    s_melodic_patch = SEQ_MEL_PATCH;
    s_quantizer.enabled = CONFIG_SEQ_QUANTIZER_DEFAULT_ENABLED;
    s_quantizer.root_note = CONFIG_SEQ_QUANTIZER_DEFAULT_ROOT_NOTE;
    s_quantizer.scale_index = CONFIG_SEQ_QUANTIZER_DEFAULT_SCALE;
    if (s_quantizer.scale_index >= quantizer_scale_count()) {
        s_quantizer.scale_index = 0;
    }
    memset(s_lfo_phase, 0, sizeof(s_lfo_phase));
    memset(s_lfo_hz,    0, sizeof(s_lfo_hz));
    memset(s_lfo_rnd,   0, sizeof(s_lfo_rnd));
    CORE_HEAP_CHECK("core_init: before push_tempo");
    sequencer_push_tempo(s_bpm);
    CORE_HEAP_CHECK("core_init: after push_tempo");
    ESP_LOGI(TAG, "sequencer_core initialized");
}

uint8_t sequencer_core_add_layer(seq_layer_type_t type, uint8_t num_steps)
{
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
            ESP_LOGW(TAG, "add_layer[%d]: melodic synth ceiling reached, "
                          "reusing slots %u..%u", idx, base, base + SEQ_TRACKS - 1);
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
            layer->env[t] = seq_default_melodic_env();
        }
    }

    /* amp_scale defaults to 1.0 (unity); memset in add_layer zeroes it, so
     * explicit init here is required to avoid silencing all tracks. */
    for (uint8_t t = 0; t < SEQ_TRACKS; t++) layer->amp_scale[t] = 1.0f;

    CORE_HEAP_CHECK("add_layer: before configure_synth");
    sequencer_configure_synth(idx);
    CORE_HEAP_CHECK("add_layer: after configure_synth");
    /* Layer is fully initialised: now expose it to the tick path. */
    s_num_layers++;
    ESP_LOGI(TAG, "add_layer[%d]: type=%d synth0=%d patch=%d steps=%d",
             idx, type, layer->synth_id[0], layer->patch, layer->num_steps);
    return idx;
}

bool sequencer_core_delete_layer(uint8_t layer_idx)
{
    if (s_num_layers <= 1) return false;                  /* must keep at least 1 */
    if (layer_idx == 0)   return false;                   /* drum layer is permanent */
    if (layer_idx >= s_num_layers) return false;
    if (s_layers[layer_idx].type == SEQ_LAYER_DRUM) return false;

    /* Clear ALL layers' tags before shifting — indices above layer_idx become
     * stale after compaction and would fire as ghost notes. */
    for (uint8_t i = 0; i < s_num_layers; i++) {
        sequencer_clear_layer_tags(i);
    }

    /* Release AMY oscillator slots for the deleted layer. */
    const seq_layer_t *dead = &s_layers[layer_idx];
    for (uint8_t t = 0; t < dead->num_tracks; t++) {
        amy_event *e = amy_helpers_event_begin();
        e->synth      = dead->synth_id[t];
        e->num_voices = 0;
        amy_helpers_event_send(e);
    }

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

    /* Resync all surviving layers so their note tags re-register correctly. */
    if (s_playing) {
        for (uint8_t i = 0; i < s_num_layers; i++) {
            sequencer_resync_layer(i);
        }
    }

    ESP_LOGI(TAG, "delete_layer[%u]: %u layers remain", layer_idx, s_num_layers);
    return true;
}

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

/* Find the index of `patch` within the curated drum list, or 0 if not present. */
static int sequencer_drum_patch_index_for(uint16_t patch)
{
    for (int i = 0; i < SEQ_DRUM_PATCH_COUNT; i++) {
        if (SEQ_DRUM_PATCH_LIST[i] == patch) return i;
    }
    return 0;
}

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

    dir = (dir > 0) ? 1 : -1;
    int idx = sequencer_drum_patch_index_for(layer->track_patch[track]);
    int ni  = (idx + dir + SEQ_DRUM_PATCH_COUNT) % SEQ_DRUM_PATCH_COUNT;
    uint16_t next = SEQ_DRUM_PATCH_LIST[ni];
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

/* ── Arpeggiator support ─────────────────────────────────────────────────
 * Arp tags sit just above the sequencer's tag space. The sequencer uses:
 *   step on/off : 0 .. MAX_LAYERS*SEQ_TRACKS*SEQ_MAX_STEPS*2 - 1   (0..1023)
 *   previews    : 1024 .. 1024 + MAX_LAYERS*SEQ_TRACKS*2 - 1       (..1055)
 * so the highest sequencer tag is 1055. We base the arp at 1056 and it needs
 * ARP_MAX_SLOTS*ARP_OCT_MAX*2 = 64 tags (1056..1119).
 *
 * IMPORTANT: AMY's sequencer_add_event guards with `tag > max_sequences`
 * (NOT >=), so `sequences[]` must have at least (highest_tag + 2) entries to
 * stay clear of that off-by-one. main.c sets amy_cfg.max_sequencer_tags
 * accordingly — keep these in sync. */
#define SEQ_ARP_TAG_BASE 1056u
#define SEQ_ARP_TAG_COUNT (ARP_MAX_SLOTS * ARP_OCT_MAX * 2)  /* = 64 */
#define SEQ_ARP_TAG_MAX  (SEQ_ARP_TAG_BASE + SEQ_ARP_TAG_COUNT - 1)  /* 1119 */

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

void sequencer_core_arp_emit_note(uint32_t tag_base, uint8_t midi_note,
                                  float velocity, uint32_t tick_on,
                                  uint32_t gate_ticks, uint32_t period)
{
    /* Defensive: never let an out-of-range tag reach AMY. Its sequences[] table
     * is sized to max_sequencer_tags and add_event has a `tag > max` off-by-one,
     * so a stray tag would smash the heap. Cap to the reserved arp window. */
    if (tag_base + 1 > SEQ_ARP_TAG_MAX) {
        ESP_LOGE(TAG, "arp tag %u out of range (max %u) - dropped",
                 (unsigned)tag_base, (unsigned)SEQ_ARP_TAG_MAX);
        return;
    }

    uint32_t tick_off = (period > 0) ? ((tick_on + gate_ticks) % period)
                                     : (tick_on + gate_ticks);
    if (tick_off == 0) tick_off = 1; /* tick 0 is reserved (clear) */
    if (tick_on  == 0) tick_on  = 1;

    amy_send_note_sched(SEQ_ARP_SYNTH, midi_note, velocity,
                        tag_base, tick_on, period);
    amy_send_note_sched(SEQ_ARP_SYNTH, midi_note, 0.0f,
                        tag_base + 1, tick_off, period);
}

void sequencer_core_arp_clear_note(uint32_t tag_base)
{
    sequencer_emit_clear_tag(tag_base);
    sequencer_emit_clear_tag(tag_base + 1);
}

/* ── Per-row melodic envelope (runtime-editable) ─────────────────────────── */

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
static void sequencer_configure_melodic_filter_track(uint8_t layer_idx, uint8_t track)
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

uint8_t sequencer_core_get_num_layers(void)
{
    return s_num_layers;
}

seq_layer_type_t sequencer_core_get_layer_type(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return SEQ_LAYER_DRUM;
    return s_layers[layer_idx].type;
}

void sequencer_core_set_step(uint8_t layer_idx, uint8_t track,
                              uint8_t step, bool state)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (track >= layer->num_tracks || step >= layer->num_steps) return;
    if (layer->grid[track][step] == state) return;
    layer->grid[track][step] = state;
    sequencer_emit_step(layer_idx, track, step);
}

void sequencer_core_set_bpm(uint16_t new_bpm)
{
    s_bpm = sequencer_clamp_bpm(new_bpm);
    sequencer_push_tempo(s_bpm);
    for (int li = 0; li < s_num_layers; li++) {
        for (int tr = 0; tr < SEQ_TRACKS; tr++) {
            if (s_layers[li].lfo_authored[tr] && s_layers[li].lfo[tr].enabled)
                s_lfo_hz[li][tr] = lfo_rate_to_hz(s_layers[li].lfo[tr].rate, s_bpm);
        }
    }
    /* Sync the arp WAVE-mode LFO carrier to the new BPM (no-op when not active). */
    arp_core_refresh_lfo_freq();
}

uint16_t sequencer_core_get_bpm(void) { return s_bpm; }

/* Derive the currently-playing step from AMY's free-running tick counter:
 * position within the bar divided by ticks-per-step. When paused we return the
 * frozen value captured at pause time so the UI playhead stops in place. */
uint8_t sequencer_core_get_current_step(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return 0;
    if (!s_playing) return s_cached_step[layer_idx];
    seq_layer_t *layer = &s_layers[layer_idx];
    uint32_t bar_ticks = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
    s_cached_step[layer_idx] =
        (uint8_t)((sequencer_ticks() % bar_ticks) / SEQ_TICKS_PER_STEP);
    return s_cached_step[layer_idx];
}

/* Start/stop playback. On start, every step is re-emitted so AMY repopulates
 * its schedule; on stop, each layer's playhead is captured (for a frozen UI)
 * and all scheduled events are cancelled so nothing keeps triggering. */
void sequencer_core_set_playing(bool p)
{
    if (s_playing == p) return;
    s_playing = p;
    if (s_playing) {
        /* Anchor the bar counter so bars_elapsed is relative to this play-start. */
        s_bar_baseline = sequencer_ticks();
        s_prog.entry_start_bar = 0;
        s_prog.current = 0;
        for (uint8_t i = 0; i < s_num_layers; i++) {
            sequencer_resync_layer(i);
        }
    } else {
        /* Bug 1.1: clear arp scheduled events FIRST so repeating arp tags
         * don't keep firing while the sequencer is paused. */
        arp_core_clear_all();

        /* Freeze display positions before clearing scheduled events. */
        for (uint8_t i = 0; i < s_num_layers; i++) {
            seq_layer_t *layer = &s_layers[i];
            uint32_t bar_ticks = (uint32_t)layer->num_steps * SEQ_TICKS_PER_STEP;
            s_cached_step[i] =
                (uint8_t)((sequencer_ticks() % bar_ticks) / SEQ_TICKS_PER_STEP);
            sequencer_clear_layer_tags(i);
        }

        /* Bug 1.2: silence only the sequencer's own synth slots (melodic/drum
         * per-layer, plus arp slot 63). Drone slots 64-65 are intentionally
         * spared — they manage their own lifecycle and do not auto-resume, so
         * a global notes-off would permanently silence the drone. */
        for (uint8_t i = 0; i < s_num_layers; i++) {
            seq_layer_t *layer = &s_layers[i];
            for (uint8_t t = 0; t < layer->num_tracks; t++) {
                sequencer_kill_synth_voices(layer->synth_id[t]);
            }
        }
        sequencer_kill_synth_voices(SEQ_ARP_SYNTH);
    }
}

void sequencer_core_set_track_midi_note(uint8_t layer_idx, uint8_t track,
                                         uint8_t midi_note)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];

    midi_note = sequencer_clamp_layer_note(layer, midi_note);

    s_track_source_note[layer_idx][track] = midi_note;
    sequencer_refresh_track_note(layer_idx, track, true);
}

uint8_t sequencer_core_get_track_midi_note(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    return s_layers[layer_idx].track_base_note[track];
}

uint8_t sequencer_core_get_track_source_note(uint8_t layer_idx, uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return 0;
    return s_track_source_note[layer_idx][track];
}

void sequencer_core_set_quantizer_enabled(bool enabled)
{
    if (s_quantizer.enabled == enabled) return;
    s_quantizer.enabled = enabled;
    sequencer_refresh_melodic_layers(false);
    ESP_LOGI(TAG, "quantizer %s", enabled ? "enabled" : "disabled");
}

void sequencer_core_set_quantizer_root_note(uint8_t root_note)
{
    s_quantizer.root_note = root_note;
    sequencer_refresh_melodic_layers(false);
    ESP_LOGI(TAG, "quantizer root -> %u", root_note);
}

void sequencer_core_set_quantizer_scale(uint8_t scale_index)
{
    s_quantizer.scale_index = (scale_index >= quantizer_scale_count()) ? 0 : scale_index;
    sequencer_refresh_melodic_layers(false);
    ESP_LOGI(TAG, "quantizer scale -> %u", s_quantizer.scale_index);
}

bool sequencer_core_get_quantizer_enabled(void)
{
    return s_quantizer.enabled;
}

uint8_t sequencer_core_get_quantizer_root_note(void)
{
    return s_quantizer.root_note;
}

uint8_t sequencer_core_get_quantizer_scale(void)
{
    return s_quantizer.scale_index;
}

/* ── Global chord progression ─────────────────────────────────────────────── */

/* Map a chord type to the closest diatonic scale for arp snap quality. */
static uint8_t chord_type_to_scale_index(chord_type_t ct)
{
    /* Scale indices mirror s_scales[] in quantizer.c:
     * 0=Chromatic, 1=Major, 2=Natural Minor, 3=Dorian, 4=Phrygian,
     * 5=Lydian, 6=Mixolydian, 7=Minor Pent, 8=Major Pent */
    switch (ct) {
        case CHORD_MAJ:  return 1;
        case CHORD_MIN:  return 2;
        case CHORD_MAJ7: return 1;
        case CHORD_MIN7: return 3;  /* Dorian has the natural 6 common in min7 contexts */
        case CHORD_DOM7: return 6;  /* Mixolydian */
        case CHORD_SUS2: return 8;  /* Major Pentatonic — open, no 3rd */
        case CHORD_SUS4: return 8;
        case CHORD_DIM:  return 4;  /* Phrygian — dark/diminished flavour */
        case CHORD_AUG:  return 5;  /* Lydian — raised 4th matches augmented feel */
        case CHORD_MIN9: return 3;  /* Dorian — same family as min7 */
        case CHORD_MAJ9: return 1;  /* Major */
        default:         return 1;
    }
}

static void chord_progression_apply_current(void)
{
    if (s_prog.count == 0) return;
    const chord_prog_entry_t *e = &s_prog.entries[s_prog.current];

    /* Update every melodic layer's chord state and re-resolve all tracks. */
    for (uint8_t li = 0; li < s_num_layers; li++) {
        seq_layer_t *layer = &s_layers[li];
        if (layer->type != SEQ_LAYER_MELODIC) continue;
        layer->chord_mode  = true;
        layer->chord_root  = e->root;
        layer->chord_type  = e->chord_type;
    }
    sequencer_refresh_melodic_layers(false);

    /* Drive arp root + scale to match the new chord. */
    arp_set_root_note((uint8_t)(e->root + 60));   /* pitch class → MIDI octave 4 */
    arp_set_scale(chord_type_to_scale_index(e->chord_type));
}

/* Called from synth_ui_task at 20 Hz. This is the SINGLE task that emits chord
 * changes to AMY: it both (a) drains s_prog_apply_pending, which input-task entry
 * points (encoder_task / button callback) set after mutating chord state, and
 * (b) advances the progression when the current entry expires. Funnelling every
 * chord_progression_apply_current() / sequencer_refresh_melodic_layers() emit
 * through this one task means edits never race the periodic advance or each other
 * across tasks — no lock needed because there is only one writer of the emit. */
void sequencer_core_progression_service(void)
{
    /* Drain deferred chord applies first, regardless of playing/enabled state. */
    if (s_prog_apply_pending) {
        s_prog_apply_pending = false;
        if (s_prog.enabled && s_prog.count > 0) {
            chord_progression_apply_current();
        } else {
            /* Disabled (or empty): layers already had chord_mode cleared by the
             * caller, or a manual per-layer chord was set/cleared; re-resolve all
             * melodic layers against their own current chord/scale state. */
            sequencer_refresh_melodic_layers(false);
        }
    }

    if (!s_prog.enabled || s_prog.count == 0 || !s_playing) return;

    uint32_t bars = sequencer_bars_elapsed();
    const chord_prog_entry_t *e = &s_prog.entries[s_prog.current];

    if (bars - s_prog.entry_start_bar >= e->duration_bars) {
        uint8_t next = (uint8_t)((s_prog.current + 1) % s_prog.count);
        s_prog.current = next;
        s_prog.entry_start_bar = bars;
        chord_progression_apply_current();
        ESP_LOGI(TAG, "progression -> entry %u (root=%u type=%u)",
                 next, s_prog.entries[next].root, (unsigned)s_prog.entries[next].chord_type);
    }
}

/* ── Progression public API ─────────────────────────────────────────────── */

void sequencer_core_progression_set_enabled(bool en)
{
    s_prog.enabled = en;
    if (en && s_prog.count > 0) {
        /* Anchor entry_start_bar to now so the first entry gets its full
         * duration regardless of when in a play session this is toggled. */
        s_prog.current = 0;
        s_prog.entry_start_bar = sequencer_bars_elapsed();
        /* Defer the AMY emit to the service tick (single-applier). */
        s_prog_apply_pending = true;
    } else if (!en) {
        /* Disable chord mode on all melodic layers so they return to scale
         * quantizer; defer the re-resolve emit to the service tick. */
        for (uint8_t li = 0; li < s_num_layers; li++) {
            s_layers[li].chord_mode = false;
        }
        s_prog_apply_pending = true;
    }
}

bool sequencer_core_progression_get_enabled(void) { return s_prog.enabled; }

void sequencer_core_progression_set_entry(uint8_t idx, uint8_t root,
                                          chord_type_t chord_type,
                                          uint8_t duration_bars)
{
    if (idx >= CHORD_PROG_MAX_ENTRIES) return;
    s_prog.entries[idx].root          = root % 12;
    s_prog.entries[idx].chord_type    = (chord_type < CHORD_TYPE_COUNT)
                                        ? chord_type : CHORD_MAJ;
    s_prog.entries[idx].duration_bars = (duration_bars > 0) ? duration_bars : 4;
    if (idx >= s_prog.count) s_prog.count = (uint8_t)(idx + 1);
    /* If we edited the live (currently playing) entry, defer a re-apply so the
     * audible chord tracks the edit. */
    if (s_prog.enabled && idx == s_prog.current) s_prog_apply_pending = true;
}

void sequencer_core_progression_get_entry(uint8_t idx, uint8_t *root,
                                          chord_type_t *chord_type,
                                          uint8_t *duration_bars)
{
    if (idx >= s_prog.count || idx >= CHORD_PROG_MAX_ENTRIES) return;
    if (root)          *root          = s_prog.entries[idx].root;
    if (chord_type)    *chord_type    = s_prog.entries[idx].chord_type;
    if (duration_bars) *duration_bars = s_prog.entries[idx].duration_bars;
}

void sequencer_core_progression_set_count(uint8_t count)
{
    if (count > CHORD_PROG_MAX_ENTRIES) count = CHORD_PROG_MAX_ENTRIES;
    s_prog.count = count;
    /* If the active entry fell out of range, wrap to 0, restart its bar window,
     * and re-apply so the audible chord follows the new active entry. */
    if (s_prog.current >= s_prog.count && s_prog.count > 0) {
        s_prog.current = 0;
        s_prog.entry_start_bar = sequencer_bars_elapsed();
        if (s_prog.enabled) s_prog_apply_pending = true;
    }
}

uint8_t sequencer_core_progression_get_count(void) { return s_prog.count; }
uint8_t sequencer_core_progression_get_current(void) { return s_prog.current; }
uint8_t sequencer_core_progression_get_max(void) { return CHORD_PROG_MAX_ENTRIES; }

/* Bars elapsed within the currently-playing entry (0-based), for the UI status bar. */
uint8_t sequencer_core_progression_bars_in_current(void)
{
    if (!s_prog.enabled || s_prog.count == 0) return 0;
    uint32_t bars = sequencer_bars_elapsed();
    if (bars < s_prog.entry_start_bar) return 0;   /* baseline just moved */
    return (uint8_t)(bars - s_prog.entry_start_bar);
}

/* Append a default entry (Cmaj, 4 bars) if room remains. Returns true on success. */
bool sequencer_core_progression_add_entry(void)
{
    if (s_prog.count >= CHORD_PROG_MAX_ENTRIES) return false;
    uint8_t idx = s_prog.count;
    s_prog.entries[idx].root          = 0;          /* C */
    s_prog.entries[idx].chord_type    = CHORD_MAJ;
    s_prog.entries[idx].duration_bars = 4;
    s_prog.count = (uint8_t)(idx + 1);
    return true;
}

/* Delete entry idx, shifting later entries down. Keeps at least one entry. */
void sequencer_core_progression_delete_entry(uint8_t idx)
{
    if (idx >= s_prog.count || s_prog.count <= 1) return;
    for (uint8_t i = idx; i + 1 < s_prog.count; i++) {
        s_prog.entries[i] = s_prog.entries[i + 1];
    }
    s_prog.count--;
    /* Fix up the active index/window if it was at or past the deletion point. */
    bool active_changed = false;
    if (s_prog.current == idx) {
        if (s_prog.current >= s_prog.count) s_prog.current = 0;
        active_changed = true;
    } else if (s_prog.current > idx) {
        s_prog.current--;   /* same entry, new slot — no audible change */
    }
    if (active_changed) {
        s_prog.entry_start_bar = sequencer_bars_elapsed();
        if (s_prog.enabled) s_prog_apply_pending = true;
    }
}

void sequencer_core_progression_set_layer_chord(uint8_t layer_idx,
                                                uint8_t root,
                                                chord_type_t chord_type)
{
    if (layer_idx >= s_num_layers) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    if (layer->type != SEQ_LAYER_MELODIC) return;
    layer->chord_mode = true;
    layer->chord_root = root % 12;
    layer->chord_type = chord_type;
    /* Defer the re-resolve emit to the service tick (single-applier). */
    s_prog_apply_pending = true;
}

void sequencer_core_progression_clear_layer_chord(uint8_t layer_idx)
{
    if (layer_idx >= s_num_layers) return;
    s_layers[layer_idx].chord_mode = false;
    /* Defer the re-resolve emit to the service tick (single-applier). */
    s_prog_apply_pending = true;
}

void sequencer_core_get_layer_chord(uint8_t layer_idx, bool *chord_mode,
                                    uint8_t *root, chord_type_t *chord_type)
{
    if (layer_idx >= s_num_layers) return;
    const seq_layer_t *layer = &s_layers[layer_idx];
    if (chord_mode) *chord_mode = layer->chord_mode;
    if (root)       *root       = layer->chord_root;
    if (chord_type) *chord_type = layer->chord_type;
}

void sequencer_core_set_track_repeat_rate(uint8_t layer_idx, uint8_t track,
                                          seq_repeat_rate_t rate)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return;
    seq_layer_t *layer = &s_layers[layer_idx];
    layer->repeat_rate[track] = (uint8_t)rate;
    /* Re-emit all steps on this track so AMY picks up the new period. */
    for (uint8_t s = 0; s < layer->num_steps; s++) {
        sequencer_emit_step(layer_idx, track, s);
    }
}

seq_repeat_rate_t sequencer_core_get_track_repeat_rate(uint8_t layer_idx,
                                                        uint8_t track)
{
    if (layer_idx >= s_num_layers || track >= SEQ_TRACKS) return SEQ_REPEAT_1;
    uint8_t rr = s_layers[layer_idx].repeat_rate[track];
    switch (rr) {
        case 2: return SEQ_REPEAT_2;
        case 4: return SEQ_REPEAT_4;
        case 8: return SEQ_REPEAT_8;
        default: return SEQ_REPEAT_1;
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

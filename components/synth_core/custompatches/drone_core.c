/* drone_core.c — standalone "stutter house drone" synth.
 *
 * Translated from the AMYboard sketch. See drone_core.h for the design notes.
 *
 * IMPORTANT (heap/race safety): every AMY interaction goes through the queued
 * event API (amy_add_event), never direct synth[] access. This is consistent
 * with the amy_render lock fix documented in AMY-EDITS.md — the render path
 * walks synth[] under the queue lock, so all our config/notes must be deltas.
 *
 * amy_event is ~800 bytes; we share one module-level scratch event guarded by a
 * mutex (the established project pattern; never place amy_event on a task
 * stack). All callers here are FreeRTOS tasks, never ISRs. */

#include "custompatches/drone_core.h"
#include "synth_ui.h"      /* seq_get_bpm() (live global BPM) */
#include "amy_fx.h"        /* synth_ui_fx_reassert_global() */
#include "sequencer_core.h"    /* sequencer_core_push_envelope */
#include "seq_clamp.h"
#include "chord_types.h"   /* chord_type_t, chord_type_name() */
#include "quantizer.h"     /* quantizer_chord_intervals() */
#include "amy.h"
#include "amy_helpers.h"
#include "voice_config.h"  /* voice_env_apply_ks_noise_floor */
#include "sdkconfig.h"
#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "drone_core";

/* AMY's monotonic sequencer tick counter (48 PPQ, AMY_SEQUENCER_PPQ), advanced
 * by the audio-rate sequencer. This is the same musical-time clock the sequencer
 * and arp ride; the filter sweep derives its phase from it so the sweep is
 * frame-rate-independent and genuinely beat-locked (not tied to how often the UI
 * task happens to call drone_core_service()). */
extern uint32_t sequencer_ticks(void);

/* ── Synth slots (above the existing 0..63 map; needs amy_cfg.max_synths>=66) ── */
#define DRONE_SYNTH_MAIN   64
#define DRONE_SYNTH_SUB    65
#define DRONE_OSCS_PER_VC  2     /* osc0 = carrier, osc1 = stutter LFO */
#define DRONE_MAIN_VOICES  DRONE_CHORD_MAX_NOTES  /* one voice per chord note */
#define DRONE_SUB_VOICES   1     /* sub is always a single tone (root)        */

#define DRONE_GATE_VEL     1.0f

#define DRONE_ROOT_MIN     24    /* C1 — lowest selectable drone root */
#define DRONE_ROOT_MAX     72    /* C5 — highest selectable drone root */
#define DRONE_ROOT_DEFAULT 45    /* A2 — matches the former Am7 voicing */

/* Limits */
#define DRONE_RES_MIN      0.1f
#define DRONE_RES_MAX      3.0f    /* AMY itself imposes no upper Q cap (floor 0.51);
                                    * 8 gives a strong acid peak with headroom before
                                    * self-oscillation transients spike the clip LUT. */
#define DRONE_SWEEP_MIN    100.0f
#define DRONE_SWEEP_MAX    8000.0f
#define DRONE_BARS_MIN     1
#define DRONE_BARS_MAX     16
#define DRONE_PATCH_MIN    0
/* Drone PATCH-mode ceiling: wave patches 257-261 (SINE..TRIANGLE) are supported,
 * plus the wavetable bank patches (267-271, AMY_WAVETABLE only). NOISE (262),
 * KS (263), and the bass presets (264-266) are intentionally excluded from the
 * drone — their excitation/multi-osc model misbehaves with the drone's
 * one-shot trigger style; drone_set_patch() snaps values in that gap back down
 * to TRIANGLE rather than clamping the whole range there. */
#if CONFIG_AMY_WAVETABLE
#define DRONE_PATCH_MAX    SEQ_PATCH_WAVETABLE_MAX
#else
#define DRONE_PATCH_MAX    SEQ_PATCH_TRIANGLE
#endif
#define DRONE_GATE_MIN     0.05f   /* osc1 PULSE duty: tighter chop as it shrinks */
#define DRONE_GATE_MAX     0.95f
#define DRONE_SWING_MAX    66      /* percent of one subdivision, applied to odd steps */
#define DRONE_BLIP_MAX     1.0f    /* per-step downward filter-zap depth (0 = off)     */
#define DRONE_PAT_STEPS    8       /* steps per bar in a pattern mask                  */

/* Chord intervals come from the shared quantizer_chord_intervals(chord_type_t)
 * table (components/synth_core/include/quantizer.h).  The private
 * s_chord_formulas / s_chord_names tables and the drone_chord_t enum have
 * been removed in favour of the shared chord_type_t system. */

/* Stutter rate -> multiplier on the beat rate (beats/sec * mult = LFO Hz).
 * 1/4 = 1x beat, 1/8 = 2x, 1/16 = 4x, 1/32 = 8x. */
static const float s_rate_mult[DRONE_RATE_COUNT] = {
    [DRONE_RATE_1_4]  = 1.0f,
    [DRONE_RATE_1_8]  = 2.0f,
    [DRONE_RATE_1_16] = 4.0f,
    [DRONE_RATE_1_32] = 8.0f,
};

static const char *s_rate_names[DRONE_RATE_COUNT] = {
    [DRONE_RATE_1_4]  = "1/4",
    [DRONE_RATE_1_8]  = "1/8",
    [DRONE_RATE_1_16] = "1/16",
    [DRONE_RATE_1_32] = "1/32",
};

/* ── Step patterns ── 8-bit per-bar masks (bit0 = step0, LSB-first). A 0 bit
 * means that stutter subdivision is skipped (filter closed). FULL = legacy. */
static const uint8_t s_pattern_mask[DRONE_PAT_COUNT] = {
    [DRONE_PAT_FULL]    = 0xFF,   /* 1 1 1 1 1 1 1 1 */
    [DRONE_PAT_FOUR]    = 0x55,   /* 1 0 1 0 1 0 1 0 */
    [DRONE_PAT_OFFBEAT] = 0xAA,   /* 0 1 0 1 0 1 0 1 */
    [DRONE_PAT_GALLOP]  = 0x5B,   /* 1 1 0 1 1 0 1 0 */
    [DRONE_PAT_DUB]     = 0x51,   /* 1 0 0 0 1 0 1 0 */
};

static const char *s_pattern_names[DRONE_PAT_COUNT] = {
    [DRONE_PAT_FULL]    = "FULL",
    [DRONE_PAT_FOUR]    = "FOUR",
    [DRONE_PAT_OFFBEAT] = "OFFBT",
    [DRONE_PAT_GALLOP]  = "GALOP",
    [DRONE_PAT_DUB]     = "DUB",
};

/* ── State ── */
typedef struct {
    bool           enabled;
    drone_source_t source;
    uint16_t       wave;        /* AMY wave constant for the carrier */
    chord_type_t   chord;       /* chord preset (shared chord_type_t) */
    uint8_t        root_note;   /* drone-local root (DRONE_ROOT_MIN..MAX) */
    float          resonance;
    float          amp_peak;    /* 0..1 on-beat level knob (linear: peak_lin = amp_peak) */
    float          amp_duck;    /* 0..1 duck depth knob  (duck_db = amp_duck * 40 dB)   */
    drone_rate_t   rate;
    uint16_t       patch;
    bool           sub_enabled;
    int8_t         sub_interval;/* semitones below the main          */
    float          sweep_lo;
    float          sweep_hi;
    uint8_t        sweep_bars;
    /* stutter-house controls */
    float          gate_len;    /* osc1 PULSE duty: 0.05..0.95 (chop length)  */
    uint8_t        swing_pct;   /* 0..66 swing on the filter/pattern grid     */
    float          blip_depth;  /* 0..1 per-step downward filter-zap depth    */
    drone_pattern_t pattern;    /* per-bar step on/off mask                   */
    float          last_blip_cutoff; /* to avoid redundant cutoff re-sends    */
    /* sweep phase, advanced each service tick (0..2pi) */
    float          last_lfo_hz; /* to avoid redundant LFO re-sends   */
    voice_params_t vp;          /* shared voice params. The drone uses env (graph
                                   editor ADSR), env1 (EG1 timing storage), their
                                   authored flags, and amp_trim (multiplied into
                                   amp_peak in s_amp_peak_lin() for the effective
                                   on-beat level; graph editor amp mode,
                                   MY_BUTTON_2). vp.filter/vp.lfo are unused: the
                                   drone's sweep filter and stutter gate are its
                                   own concepts. Defaults via
                                   voice_params_init_defaults in drone_core_init. */
} drone_state_t;

static drone_state_t s_d;

/* Schedule-affecting setters mark the drone dirty instead of rebuilding
 * inline; drone_core_service() (once per UI frame) does a single rebuild when
 * dirty, so an encoder spin through waves/params collapses to one rebuild +
 * one burst of AMY event-mutex traffic per frame instead of one per detent.
 * Mirrors the arp's s_arp_dirty discipline. Setters run on Core-0 input tasks
 * and the drain runs on the Core-0 synth_ui_task — single-core, one-way flag,
 * so the volatile is for compiler ordering, not cross-core synchronization.
 * Enable, chord, and root-note changes stay synchronous: enabling must sound
 * NOW, and chord/root must note-off the OLD voicing before state changes or
 * voices stick. */
static volatile bool s_d_dirty = false;
static inline void drone_mark_dirty(void) { s_d_dirty = true; }

/* ── Peak/Duck dB amp helpers ──────────────────────────────────────────────
 * These are the SINGLE source of truth for the engine math.  Both
 * drone_configure_wave_synth() and drone_get_amp_levels_norm() call these
 * helpers — never inline the formula elsewhere.
 *
 * Knob→dB/linear mapping:
 *   peak_lin = amp_peak           (linear 0..1, identity mapping)
 *   duck_db  = amp_duck * 40.0f  (0 dB at knob=0, -40 dB floor at knob=1)
 *
 * AMY engine coefs (from the two-equation solve):
 *   m          = duck_db / 120
 *   const_sent = peak_lin * 10^(-duck_db / 40)
 *
 * Result:
 *   ON-beat  (LFO=+1): const_sent * 10^(3*m) = peak_lin
 *   OFF-beat (LFO=-1): const_sent * 10^(-3*m) = peak_lin * 10^(-duck_db/20)
 */
static inline float s_amp_peak_lin(void)
{
    /* amp_trim is a per-target volume trim (0..1, default 1.0) set by the graph
     * editor amp mode. Fold it into amp_peak here so all callers automatically
     * pick up the trimmed level (single source of truth rule applies). */
    float v = s_d.amp_peak * s_d.vp.amp_trim;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

static inline float s_amp_duck_db(void)
{
    return s_d.amp_duck * 40.0f;
}

static inline float s_amp_m(void)
{
    return s_amp_duck_db() / 120.0f;
}

static inline float s_amp_const_sent(void)
{
    return s_amp_peak_lin() * powf(10.0f, -s_amp_duck_db() / 40.0f);
}

/* AMY events are emitted through the shared amy_helpers scratch buffer. */

/* ── Tempo helpers ── */

/* Beats per second from the live global BPM. */
static inline float drone_bps(void)
{
    uint16_t bpm = seq_get_bpm();
    if (bpm < 1) bpm = 1;
    return (float)bpm / 60.0f;
}

/* Stutter LFO frequency for the current rate + tempo. */
static inline float drone_lfo_hz(void)
{
    return drone_bps() * s_rate_mult[s_d.rate];
}

/* Count the notes in a chord formula (up to the first -1 sentinel, max DRONE_CHORD_MAX_NOTES).
 * Uses the shared quantizer_chord_intervals() table; returns 0 if chord is out of range. */
static uint8_t drone_chord_note_count(chord_type_t chord)
{
    const int8_t *row = quantizer_chord_intervals(chord);
    if (!row) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < DRONE_CHORD_MAX_NOTES; i++) {
        if (row[i] < 0) break;
        n++;
    }
    return n;
}

/* ── Synth configuration (WAVE mode) ──
 * Build a `voices`-voice synth, each voice = osc0 carrier (NOTE-following, so
 * its pitch comes from the voice's note-on) amplitude-gated by osc1 PULSE LFO.
 * A multi-voice main synth therefore sounds a chord when fed multiple notes. */
/* `wave` is the AMY waveform to use for the carrier (osc0).  Callers pass
 * s_d.wave for WAVE-mode, or the patch-derived wave for PATCH-mode wave patches.
 * `wt_preset` selects which built-in wavetable bank to play when wave==WAVETABLE
 * (index into pcm_wavetable_base..+pcm_wavetable_samples-1); pass -1 for any
 * non-wavetable wave, where it is a no-op. */
static void drone_configure_wave_synth(uint8_t synth, uint8_t voices, uint16_t wave,
                                       int16_t wt_preset)
{
    float lfo_hz    = drone_lfo_hz();
    /* AMY 1.2.12 (#720) dB/exp amp model: amp = const_sent * 10^(3*m*LFO).
     * Peak/Duck mapping (see s_amp_* helpers for full derivation):
     *   ON-beat (LFO=+1) = peak_lin = amp_peak
     *   OFF-beat(LFO=-1) = peak_lin * 10^(-duck_db/20), duck_db = amp_duck*40. */
    float m          = s_amp_m();
    float const_sent = s_amp_const_sent();

    /* Shared skeleton: pool (build-your-own synth: N voices, 2 oscs/voice, no
     * patch) + osc0 NOTE-following carrier (COEF_NOTE=1) so each voice plays
     * its own chord note. Velocity does not scale amp (osc0_amp_vel=0); EG0
     * multiplies the whole amp, so the ADSR envelope shapes the drone
     * swell/fade *around* the LFO stutter. */
    voice_wave_cfg_t cfg = {
        .synth                = synth,
        .num_voices           = voices,
        .oscs_per_voice       = DRONE_OSCS_PER_VC,
        .wave                 = wave,
        .osc0_amp_const       = const_sent,
        .osc0_amp_vel         = 0.0f,
        .ks_feedback_authored = false,   /* fixed 0.9 KS default */
        .ks_feedback_q        = 0.0f,
        .wt_preset            = wt_preset,
    };
    voice_build_wave(&cfg);

    /* osc1 = PULSE LFO. Absolute Hz (note-follow off), full const amp.
     * The PULSE duty IS the gate length: 0.5 = 50/50 square (legacy), lower =
     * a shorter "on" fraction per subdivision = a tighter percussive chop. */
    amy_event *e = amy_helpers_event_begin();
    e->synth                 = synth;
    e->osc                   = 1;
    e->wave                  = PULSE;
    e->duty_coefs[COEF_CONST]= s_d.gate_len;
    e->freq_coefs[COEF_CONST]= lfo_hz;
    e->freq_coefs[COEF_NOTE] = 0.0f;     /* absolute Hz, ignore note */
    e->amp_coefs[COEF_CONST] = 1.0f;
    e->amp_coefs[COEF_VEL]   = 0.0f;     /* turn off the default vel/eg amp */
    e->amp_coefs[COEF_EG0]   = 0.0f;
    amy_helpers_event_send(e);

    /* osc0 drone specializations: amp = const + mod(=osc1) stutter gate, and
     * the LPF24 sweep filter. Sent after osc1 so mod_source always points at
     * a configured gate oscillator. */
    e = amy_helpers_event_begin();
    e->synth                  = synth;
    e->osc                    = 0;
    e->amp_coefs[COEF_MOD]    = m;
    e->mod_source             = 1;       /* osc1 of this voice (base-osc rel) */
    e->filter_type            = FILTER_LPF24;
    e->filter_freq_coefs[COEF_CONST] = s_d.sweep_hi;
    e->resonance              = s_d.resonance;
    amy_helpers_event_send(e);
}

/* ── Synth configuration (PATCH mode) ──
 * Load an AMY patch onto the carrier synth; apply filter + resonance on top. */
static void drone_configure_patch_synth(uint8_t synth, uint8_t voices)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth        = synth;
    e->num_voices   = voices;
    e->patch_number = s_d.patch;
    amy_helpers_event_send(e);

    /* Patch strings carry global EQ/chorus commands; keep them per-synth. */
    synth_ui_fx_reassert_global();

    /* Apply filter sweep + resonance on osc0 of the patch (best-effort). */
    e = amy_helpers_event_begin();
    e->synth                  = synth;
    e->osc                    = 0;
    e->filter_type            = FILTER_LPF24;
    e->filter_freq_coefs[COEF_CONST] = s_d.sweep_hi;
    e->resonance              = s_d.resonance;
    amy_helpers_event_send(e);
}

/* (Re)build both carrier synths for the current source/params. Main is sized to
 * the chord's note count; the sub is always a single voice. */
static void drone_rebuild(void)
{
    uint8_t chord_n = drone_chord_note_count(s_d.chord);
    if (chord_n < 1) chord_n = 1;

    if (s_d.source == DRONE_SRC_WAVE) {
        drone_configure_wave_synth(DRONE_SYNTH_MAIN, chord_n, s_d.wave, -1);
        if (s_d.sub_enabled) {
            drone_configure_wave_synth(DRONE_SYNTH_SUB, DRONE_SUB_VOICES, s_d.wave, -1);
        }
    } else if (s_d.source == DRONE_SRC_PATCH && s_d.patch >= SEQ_PATCH_WAVE_BASE) {
        /* PATCH-mode wave virtual patches (257-261, and — AMY_WAVETABLE —
         * 267-271): configure as a wave synth using the full drone
         * stutter/filter/mod setup, waveform derived from the patch number.
         * Sends these ranges as AMY patch numbers would be silent (they are
         * not real AMY patches), so we intercept here like the melodic
         * sequencer does in sequencer_configure_synth(). */
        static const uint16_t s_wave_for_patch[] = {
            SINE, SAW_DOWN, SAW_UP, PULSE, TRIANGLE,
        };
        uint16_t wave = SAW_DOWN;
        int16_t  wt_preset = -1;
#if CONFIG_AMY_WAVETABLE
        if (s_d.patch >= SEQ_PATCH_WAVETABLE_BASE && s_d.patch <= SEQ_PATCH_WAVETABLE_MAX) {
            wave = WAVETABLE;
            wt_preset = (int16_t)(pcm_wavetable_base + (s_d.patch - SEQ_PATCH_WAVETABLE_BASE));
        } else
#endif
        {
            uint16_t widx = s_d.patch - SEQ_PATCH_WAVE_BASE;
            wave = (widx < 5) ? s_wave_for_patch[widx] : SAW_DOWN;
        }
        drone_configure_wave_synth(DRONE_SYNTH_MAIN, chord_n, wave, wt_preset);
        if (s_d.sub_enabled) {
            drone_configure_wave_synth(DRONE_SYNTH_SUB, DRONE_SUB_VOICES, wave, wt_preset);
        }
    } else {
        drone_configure_patch_synth(DRONE_SYNTH_MAIN, chord_n);
        if (s_d.sub_enabled) {
            drone_configure_patch_synth(DRONE_SYNTH_SUB, DRONE_SUB_VOICES);
        }
    }
    /* Re-impose the user's custom ADSR if authored (rebuild/patch resets the
     * synth oscs). Deferred authority, matching the melodic + arp behaviour. */
    if (s_d.vp.env_authored) {
        seq_env_t env_to_push = s_d.vp.env;
        /* Attack-floor only (is_ks=false): the drone never zeroes KS sustain,
         * and KS/NOISE are excluded from its wave cycle anyway — this guards
         * stale persisted values without changing the envelope shape. */
        voice_env_apply_ks_noise_floor(&env_to_push, false,
                                       s_d.wave == KS || s_d.wave == NOISE);
        sequencer_core_push_envelope(DRONE_SYNTH_MAIN, &env_to_push);
        if (s_d.sub_enabled) {
            sequencer_core_push_envelope(DRONE_SYNTH_SUB, &env_to_push);
        }
    }
    /* Second envelope (EG1): only pushed when authored — nothing in the
     * drone's own WAVE/PATCH setup above routes a coef to COEF_EG1, so this
     * is a no-op unless the loaded PATCH-mode instrument already does. */
    if (s_d.vp.env1_authored) {
        sequencer_core_push_envelope_eg1(DRONE_SYNTH_MAIN, 0, &s_d.vp.env1);
        if (s_d.sub_enabled) {
            sequencer_core_push_envelope_eg1(DRONE_SYNTH_SUB, 0, &s_d.vp.env1);
        }
    }
    s_d.last_lfo_hz = drone_lfo_hz();
}

/* Fire one note-on/off on a synth (the instrument allocator picks a voice). */
static void drone_note(uint8_t synth, bool on, float midi_note)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth     = synth;
    e->midi_note = midi_note;
    e->velocity  = on ? DRONE_GATE_VEL : 0.0f;
    amy_helpers_event_send(e);
}

/* Start/stop the sustained drone voices. The main synth gets a note-on per
 * chord note (one voice each); the sub gets a single note at the chord ROOT
 * shifted by sub_interval. On disable we release the same notes so the ADSR
 * release fades them out. */
static void drone_apply_enabled(void)
{
    const int8_t *formula = quantizer_chord_intervals(s_d.chord);
    uint8_t chord_n = drone_chord_note_count(s_d.chord);
    if (chord_n < 1) {
        /* NULL or empty chord: play root note only. */
        drone_note(DRONE_SYNTH_MAIN, s_d.enabled, (float)s_d.root_note);
    } else {
        for (uint8_t i = 0; i < chord_n; i++) {
            if (!formula || formula[i] < 0) break;
            int midi = SEQ_CLAMP_INT((int)s_d.root_note + (int)formula[i], 0, 127);
            drone_note(DRONE_SYNTH_MAIN, s_d.enabled, (float)midi);
        }
    }

    if (s_d.sub_enabled) {
        int sub = SEQ_CLAMP_INT((int)s_d.root_note + (int)s_d.sub_interval, 12, 108);
        drone_note(DRONE_SYNTH_SUB, s_d.enabled, (float)sub);
    }
}

/* Push the current filter cutoff to a synth's osc0. */
static void drone_push_cutoff(uint8_t synth, float cutoff)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth = synth;
    e->osc   = 0;
    e->filter_freq_coefs[COEF_CONST] = cutoff;
    amy_helpers_event_send(e);
}

/* ── Public API ── */

void drone_core_init(void)
{
    amy_helpers_init();

    memset(&s_d, 0, sizeof(s_d));
    voice_params_init_defaults(&s_d.vp);   /* unauthored, amp trim at unity */
    s_d.enabled      = false;
    s_d.source       = DRONE_SRC_WAVE;
    s_d.wave         = SAW_DOWN;
    s_d.chord        = CHORD_MIN7;
    s_d.root_note    = DRONE_ROOT_DEFAULT;
    s_d.resonance    = 1.5f;
    s_d.amp_peak     = 0.5f;     /* on-beat level (linear; 0.5 = -6 dB)   */
    s_d.amp_duck     = 0.5f;     /* duck depth knob (0.5 -> 20 dB duck)   */
    s_d.rate         = DRONE_RATE_1_16;
    s_d.patch        = 25;
    s_d.sub_enabled  = true;
    s_d.sub_interval = -12;      /* one octave below */
    s_d.sweep_lo     = 600.0f;
    s_d.sweep_hi     = 2000.0f;
    s_d.sweep_bars   = 4;
    s_d.gate_len     = 0.5f;     /* 50/50 square = legacy gate            */
    s_d.swing_pct    = 0;        /* straight grid                        */
    s_d.blip_depth   = 0.0f;     /* filter-zap off by default            */
    s_d.pattern      = DRONE_PAT_FULL;
    s_d.last_blip_cutoff = -1.0f;
    /* Default ADSR: slow swell suited to a drone. Not authored until the user
     * commits in the graph editor (the raw wave plays at full sustain otherwise,
     * since an unauthored env is not pushed and EG0 holds at 1.0 on a held note). */
    s_d.vp.env.attack_ms   = 200;
    s_d.vp.env.decay_ms    = 300;
    s_d.vp.env.sustain_pct = 100;
    s_d.vp.env.release_ms  = 600;
    s_d.vp.env.eg_type     = 0;   /* ENVELOPE_NORMAL */
    /* Second envelope (EG1): not routed to anything by the drone's own WAVE
     * patches (its filter movement already comes from the independent
     * sweep_lo/sweep_hi + BPM-synced service tick below, layering a second
     * envelope-driven cutoff shift on top would double-modulate the same
     * parameter). Kept purely as timing storage for a PATCH-mode drone whose
     * loaded AMY patch already routes its own bp1. */
    s_d.vp.env1.attack_ms   = 15;
    s_d.vp.env1.decay_ms    = 400;
    s_d.vp.env1.sustain_pct = 25;
    s_d.vp.env1.release_ms  = 400;
    s_d.vp.env1.eg_type     = 0;   /* ENVELOPE_NORMAL */

    drone_rebuild();
    ESP_LOGI(TAG, "drone_core initialized (synths %u/%u)",
             DRONE_SYNTH_MAIN, DRONE_SYNTH_SUB);
}

void drone_core_service(void)
{
    /* Drain the coalesced rebuild BEFORE the enabled gate: a param changed
     * while the drone is off must still rebuild (cheap synth reconfig only;
     * drone_apply_enabled() is what actually sounds notes) so the change is
     * live on the next enable. Drained before the LFO-hz tracking below so a
     * rebuild and the tempo push land in the right order within the frame. */
    if (s_d_dirty) {
        s_d_dirty = false;
        drone_rebuild();
        if (s_d.enabled) drone_apply_enabled();
    }

    if (!s_d.enabled) return;

    /* Keep the LFO locked to tempo: re-send osc1 freq if the BPM changed the
     * stutter rate by more than a small epsilon. */
    float lfo_hz = drone_lfo_hz();
    if (fabsf(lfo_hz - s_d.last_lfo_hz) > 0.01f) {
        amy_event *e = amy_helpers_event_begin();
        e->synth                  = DRONE_SYNTH_MAIN;
        e->osc                    = 1;
        e->freq_coefs[COEF_CONST] = lfo_hz;
        e->freq_coefs[COEF_NOTE]  = 0.0f;
        amy_helpers_event_send(e);
        if (s_d.sub_enabled) {
            e = amy_helpers_event_begin();
            e->synth                  = DRONE_SYNTH_SUB;
            e->osc                    = 1;
            e->freq_coefs[COEF_CONST] = lfo_hz;
            e->freq_coefs[COEF_NOTE]  = 0.0f;
            amy_helpers_event_send(e);
        }
        s_d.last_lfo_hz = lfo_hz;
    }

    /* Everything below is a pure function of the global musical clock, NOT of how
     * often this service runs. At 48 PPQ one bar (4 beats) = 192 ticks. This keeps
     * the sweep, the stutter subdivision grid, swing, the step pattern and the
     * per-step filter blip all frame-rate-independent and beat-locked: they
     * advance with tempo automatically and stay coherent with the sequencer/arp
     * bar grid across BPM changes. */
    const uint32_t bar_ticks = (uint32_t)(AMY_SEQUENCER_PPQ * 4);   /* 192 */
    uint32_t now = sequencer_ticks();

    /* (a) Slow bar-length sweep = the BASE cutoff. */
    uint32_t period_ticks = (uint32_t)s_d.sweep_bars * bar_ticks;
    if (period_ticks < 1) period_ticks = 1;
    uint32_t pos = now % period_ticks;
    float phase = (2.0f * (float)M_PI) * ((float)pos / (float)period_ticks);
    float mid  = 0.5f * (s_d.sweep_lo + s_d.sweep_hi);
    float half = 0.5f * (s_d.sweep_hi - s_d.sweep_lo);
    float base = mid + half * sinf(phase);

    /* (b) Stutter subdivision grid. ticks_per_sub = 192 / (4 * rate_mult), e.g.
     * 1/16 -> rate_mult 4 -> 12 ticks/sub. Guard against 0. The pattern mask is
     * an 8-step per-bar grid, so the step index wraps mod DRONE_PAT_STEPS. */
    float subs_per_bar = 4.0f * s_rate_mult[s_d.rate];
    uint32_t ticks_per_sub = (uint32_t)((float)bar_ticks / subs_per_bar + 0.5f);
    if (ticks_per_sub < 1) ticks_per_sub = 1;

    /* (c) Swing: push ODD subdivisions later by swing_pct% of one subdivision.
     * Applied by offsetting the clock used for the sub-phase calc. */
    uint32_t pos_in_bar = now % bar_ticks;
    uint32_t sub_index_raw = pos_in_bar / ticks_per_sub;
    uint32_t swing_off = 0;
    if ((sub_index_raw & 1u) && s_d.swing_pct > 0) {
        swing_off = (uint32_t)((float)ticks_per_sub * (float)s_d.swing_pct / 100.0f);
    }
    /* effective position within the current subdivision (0..1), swing-shifted */
    uint32_t swung = (now + bar_ticks - swing_off) % bar_ticks; /* avoid underflow */
    uint32_t sub_index = (swung / ticks_per_sub) % DRONE_PAT_STEPS;
    float frac_in_sub = (float)(swung % ticks_per_sub) / (float)ticks_per_sub;

    /* (d) Pattern mask: a 0 bit closes the filter for that step (skips it). */
    bool step_on = (s_pattern_mask[s_d.pattern] >> sub_index) & 1u;

    /* (e) Per-step filter blip: a downward zap at the start of each open step,
     * decaying as we move through the subdivision. cutoff = base * (1 - depth *
     * env), env = exp(-k * frac). AMY's logfreq downward slew-limit smooths the
     * attack edge into something musical. */
    float cutoff;
    if (!step_on) {
        cutoff = DRONE_SWEEP_MIN;            /* closed: skip this subdivision */
    } else if (s_d.blip_depth > 0.0001f) {
        float env = expf(-4.0f * frac_in_sub);          /* 1 -> ~0 across the step */
        cutoff = base * (1.0f - s_d.blip_depth * env);
        if (cutoff < DRONE_SWEEP_MIN) cutoff = DRONE_SWEEP_MIN;
    } else {
        cutoff = base;                       /* blip off = plain sweep (legacy) */
    }

    /* Skip redundant pushes (cutoff barely changed) to keep the event queue light
     * when the drone is idling on a sustained step. */
    if (fabsf(cutoff - s_d.last_blip_cutoff) > 1.0f) {
        drone_push_cutoff(DRONE_SYNTH_MAIN, cutoff);
        if (s_d.sub_enabled) {
            drone_push_cutoff(DRONE_SYNTH_SUB, cutoff * 0.5f);  /* sub sits lower */
        }
        s_d.last_blip_cutoff = cutoff;
    }
}

void drone_set_enabled(bool on)
{
    if (s_d.enabled == on) return;
    s_d.enabled = on;
    if (on) {
        /* Rebuild so a fresh enable always reflects the current params. The
         * sweep phase is derived from the global tick clock (not reset here), so
         * the drone sweep stays phase-locked to the transport bar grid. */
        drone_rebuild();
    }
    drone_apply_enabled();
    ESP_LOGI(TAG, "drone %s", on ? "ON" : "OFF");
}

void drone_set_source(drone_source_t src)
{
    if (src != DRONE_SRC_WAVE && src != DRONE_SRC_PATCH) return;
    if (s_d.source == src) return;
    s_d.source = src;
    drone_mark_dirty();
}

void drone_set_wave(uint16_t amy_wave)
{
    /* NOISE and KS removed from the drone wave cycle (excitation model misbehaves
     * with drone's one-shot trigger style).  Coerce any persisted/stale value. */
    if (amy_wave == NOISE || amy_wave == KS) amy_wave = SAW_DOWN;
    if (s_d.wave == amy_wave) return;
    s_d.wave = amy_wave;
    if (s_d.source == DRONE_SRC_WAVE) drone_mark_dirty();
}

void drone_set_chord(chord_type_t chord)
{
    if (chord >= CHORD_TYPE_COUNT) return;
    if (s_d.chord == chord) return;
    /* Releasing the old chord's held notes before the rebuild avoids leaving
     * voices stuck on when the new chord has fewer notes. */
    bool was_enabled = s_d.enabled;
    if (was_enabled) {
        s_d.enabled = false;
        drone_apply_enabled();   /* note-off the current chord */
    }
    s_d.chord = chord;
    drone_rebuild();             /* resize main synth to the new note count */
    if (was_enabled) {
        s_d.enabled = true;
        drone_apply_enabled();   /* note-on the new chord */
    }
}

void drone_set_root_note(uint8_t note)
{
    uint8_t clamped = (uint8_t)SEQ_CLAMP_INT((int)note, DRONE_ROOT_MIN, DRONE_ROOT_MAX);
    if (s_d.root_note == clamped) return;
    bool was_enabled = s_d.enabled;
    if (was_enabled) {
        s_d.enabled = false;
        drone_apply_enabled();   /* note-off current notes (old root) */
    }
    s_d.root_note = clamped;
    if (was_enabled) {
        s_d.enabled = true;
        drone_apply_enabled();   /* note-on with new root */
    }
    ESP_LOGI(TAG, "drone root -> %u", (unsigned)clamped);
}

void drone_set_resonance(float r)
{
    r = SEQ_CLAMP_F32(r, DRONE_RES_MIN, DRONE_RES_MAX);
    if (fabsf(s_d.resonance - r) < 0.001f) return;
    s_d.resonance = r;
    /* Resonance is an osc0 param; push without full rebuild. */
    amy_event *e = amy_helpers_event_begin();
    e->synth     = DRONE_SYNTH_MAIN;
    e->osc       = 0;
    e->resonance = r;
    amy_helpers_event_send(e);
    if (s_d.sub_enabled) {
        e = amy_helpers_event_begin();
        e->synth     = DRONE_SYNTH_SUB;
        e->osc       = 0;
        e->resonance = r;
        amy_helpers_event_send(e);
    }
}

void drone_set_amp_peak(float c)
{
    c = SEQ_CLAMP_F32(c, 0.0f, 1.0f);
    if (fabsf(s_d.amp_peak - c) < 0.001f) return;
    s_d.amp_peak = c;
    if (s_d.source == DRONE_SRC_WAVE) drone_mark_dirty();
}

void drone_set_amp_duck(float m)
{
    /* Full range 0..1: duck_knob=1 -> -40 dB floor (not silence), no 0.95 cap needed. */
    m = SEQ_CLAMP_F32(m, 0.0f, 1.0f);
    if (fabsf(s_d.amp_duck - m) < 0.001f) return;
    s_d.amp_duck = m;
    if (s_d.source == DRONE_SRC_WAVE) drone_mark_dirty();
}

void drone_set_rate(drone_rate_t rate)
{
    if (rate >= DRONE_RATE_COUNT) return;
    if (s_d.rate == rate) return;
    s_d.rate = rate;
    /* service() picks up the new LFO Hz on the next frame. Nudge immediately. */
    s_d.last_lfo_hz = -1.0f;
}

bool drone_patch_excluded(uint16_t patch)
{
    /* Compile-awareness is shared with every other consumer: a range gated
     * off by Kconfig is excluded here too, on top of the drone's own rules. */
    if (sequencer_core_patch_compiled_out(patch)) return true;
    if (patch > DRONE_PATCH_MAX) return true;   /* incl. bass/FM/additive w/o wavetable */
#if CONFIG_AMY_WAVETABLE
    /* NOISE/KS/bass (262-266) sit below the wavetable range that IS
     * supported, so they need excluding individually. */
    if (patch > SEQ_PATCH_TRIANGLE && patch < SEQ_PATCH_WAVETABLE_BASE) {
        return true;
    }
#endif
    return false;
}

void drone_set_patch(uint16_t patch)
{
    /* Cycling never offers an excluded patch (the drone domain's predicate
     * skips them); this snap only defends direct/programmatic callers. */
    if (drone_patch_excluded(patch)) patch = SEQ_PATCH_TRIANGLE;
    if (s_d.patch == patch) return;
    s_d.patch = patch;
    if (s_d.source == DRONE_SRC_PATCH) drone_mark_dirty();
}

void drone_set_sub_enabled(bool on)
{
    if (s_d.sub_enabled == on) return;
    s_d.sub_enabled = on;
    if (on) {
        drone_mark_dirty();
    } else {
        /* Note-off stays synchronous: the sounding sub voice must release now,
         * not a frame later (same rule as the chord/root note-offs). */
        int sub_midi = SEQ_CLAMP_INT((int)s_d.root_note + (int)s_d.sub_interval, 12, 108);
        drone_note(DRONE_SYNTH_SUB, false, (float)sub_midi);
    }
}

void drone_set_sub_interval(int8_t st)
{
    int v = SEQ_CLAMP_INT((int)st, -36, 0);
    if (s_d.sub_interval == (int8_t)v) return;
    s_d.sub_interval = (int8_t)v;
    if (s_d.sub_enabled) drone_mark_dirty();
}

void drone_set_sweep_lo(float hz)
{
    hz = SEQ_CLAMP_F32(hz, DRONE_SWEEP_MIN, DRONE_SWEEP_MAX);
    if (hz > s_d.sweep_hi) hz = s_d.sweep_hi;
    s_d.sweep_lo = hz;
}

void drone_set_sweep_hi(float hz)
{
    hz = SEQ_CLAMP_F32(hz, DRONE_SWEEP_MIN, DRONE_SWEEP_MAX);
    if (hz < s_d.sweep_lo) hz = s_d.sweep_lo;
    s_d.sweep_hi = hz;
}

void drone_set_sweep_bars(uint8_t bars)
{
    s_d.sweep_bars = SEQ_CLAMP_U8((int)bars, DRONE_BARS_MIN, DRONE_BARS_MAX);
}

void drone_set_gate_len(float frac)
{
    frac = SEQ_CLAMP_F32(frac, DRONE_GATE_MIN, DRONE_GATE_MAX);
    if (fabsf(s_d.gate_len - frac) < 0.001f) return;
    s_d.gate_len = frac;
    /* Gate length is osc1's PULSE duty; push it without a full rebuild. WAVE
     * mode only (PATCH carriers have no osc1 LFO). */
    if (s_d.source != DRONE_SRC_WAVE) return;
    amy_event *e = amy_helpers_event_begin();
    e->synth                  = DRONE_SYNTH_MAIN;
    e->osc                    = 1;
    e->duty_coefs[COEF_CONST] = frac;
    amy_helpers_event_send(e);
    if (s_d.sub_enabled) {
        e = amy_helpers_event_begin();
        e->synth                  = DRONE_SYNTH_SUB;
        e->osc                    = 1;
        e->duty_coefs[COEF_CONST] = frac;
        amy_helpers_event_send(e);
    }
}

void drone_set_swing(uint8_t pct)
{
    s_d.swing_pct = SEQ_CLAMP_U8((int)pct, 0, DRONE_SWING_MAX);
    /* Consumed by service(); force a cutoff re-push next frame. */
    s_d.last_blip_cutoff = -1.0f;
}

void drone_set_blip(float depth)
{
    depth = SEQ_CLAMP_F32(depth, 0.0f, DRONE_BLIP_MAX);
    s_d.blip_depth = depth;
    s_d.last_blip_cutoff = -1.0f;
}

void drone_set_pattern(drone_pattern_t p)
{
    if (p >= DRONE_PAT_COUNT) return;
    s_d.pattern = p;
    s_d.last_blip_cutoff = -1.0f;
}

void drone_get_envelope(seq_env_t *out)
{
    if (out) *out = s_d.vp.env;
}

void drone_set_envelope(const seq_env_t *env)
{
    if (!env) return;
    s_d.vp.env = *env;
    if (s_d.vp.env.attack_ms < 2) s_d.vp.env.attack_ms = 2;  /* 2 ms floor */
    s_d.vp.env_authored = true;
    seq_env_t env_to_push = s_d.vp.env;
    /* Attack-floor only, matching drone_rebuild(): no KS sustain zeroing. */
    voice_env_apply_ks_noise_floor(&env_to_push, false,
                                   s_d.wave == KS || s_d.wave == NOISE);
    sequencer_core_push_envelope(DRONE_SYNTH_MAIN, &env_to_push);
    if (s_d.sub_enabled) {
        sequencer_core_push_envelope(DRONE_SYNTH_SUB, &env_to_push);
    }
    ESP_LOGI(TAG, "drone env -> A%u D%u S%u%% R%u",
             (unsigned)s_d.vp.env.attack_ms, (unsigned)s_d.vp.env.decay_ms,
             (unsigned)s_d.vp.env.sustain_pct, (unsigned)s_d.vp.env.release_ms);
}

void drone_get_envelope2(seq_env_t *out)
{
    if (out) *out = s_d.vp.env1;
}

void drone_set_envelope2(const seq_env_t *env)
{
    if (!env) return;
    s_d.vp.env1 = *env;
    if (s_d.vp.env1.attack_ms < 2) s_d.vp.env1.attack_ms = 2;  /* 2 ms floor */
    s_d.vp.env1_authored = true;
    sequencer_core_push_envelope_eg1(DRONE_SYNTH_MAIN, 0, &s_d.vp.env1);
    if (s_d.sub_enabled) {
        sequencer_core_push_envelope_eg1(DRONE_SYNTH_SUB, 0, &s_d.vp.env1);
    }
    ESP_LOGI(TAG, "drone env1 -> A%u D%u S%u%% R%u",
             (unsigned)s_d.vp.env1.attack_ms, (unsigned)s_d.vp.env1.decay_ms,
             (unsigned)s_d.vp.env1.sustain_pct, (unsigned)s_d.vp.env1.release_ms);
}

/* ── Getters ── */
bool           drone_get_enabled(void)      { return s_d.enabled; }
drone_source_t drone_get_source(void)       { return s_d.source; }
uint16_t       drone_get_wave(void)         { return s_d.wave; }
chord_type_t   drone_get_chord(void)        { return s_d.chord; }
uint8_t        drone_get_root_note(void)    { return s_d.root_note; }
float          drone_get_resonance(void)    { return s_d.resonance; }
float          drone_get_amp_peak(void)     { return s_d.amp_peak; }
float          drone_get_amp_duck(void)     { return s_d.amp_duck; }
drone_rate_t   drone_get_rate(void)         { return s_d.rate; }
uint16_t       drone_get_patch(void)        { return s_d.patch; }
bool           drone_get_sub_enabled(void)  { return s_d.sub_enabled; }
int8_t         drone_get_sub_interval(void) { return s_d.sub_interval; }
float          drone_get_sweep_lo(void)     { return s_d.sweep_lo; }
float          drone_get_sweep_hi(void)     { return s_d.sweep_hi; }
uint8_t        drone_get_sweep_bars(void)   { return s_d.sweep_bars; }
float          drone_get_gate_len(void)     { return s_d.gate_len; }
uint8_t        drone_get_swing(void)        { return s_d.swing_pct; }
float          drone_get_blip(void)         { return s_d.blip_depth; }
drone_pattern_t drone_get_pattern(void)     { return s_d.pattern; }

const char *drone_rate_name(drone_rate_t rate)
{
    if (rate >= DRONE_RATE_COUNT) return "?";
    return s_rate_names[rate];
}

const char *drone_wave_name(uint16_t amy_wave)
{
    switch (amy_wave) {
        case SINE:     return "SINE";
        case PULSE:    return "PULSE";
        case SAW_DOWN: return "SAW";
        case SAW_UP:   return "SAWUP";
        case TRIANGLE: return "TRI";
        case NOISE:    return "NOISE";
        case KS:       return "KS";
        default:       return "?";
    }
}

const char *drone_chord_name(chord_type_t chord)
{
    return chord_type_name(chord);
}

void drone_get_amp_levels_norm(float *floor_norm, float *ceil_norm)
{
    /* Compute ON-beat and OFF-beat linear amplitudes from the SAME dB math
     * as drone_configure_wave_synth() — single source of truth via s_amp_*. */
    float peak    = s_amp_peak_lin();
    float duck_db = s_amp_duck_db();
    if (ceil_norm)  *ceil_norm  = peak;
    if (floor_norm) *floor_norm = peak * powf(10.0f, -duck_db / 20.0f);
}

const char *drone_pattern_name(drone_pattern_t p)
{
    if (p >= DRONE_PAT_COUNT) return "?";
    return s_pattern_names[p];
}

/* ── Per-target amplitude trim (graph editor amp mode) ──────────────────── */

void drone_set_amp_trim(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (fabsf(s_d.vp.amp_trim - v) < 0.001f) return;
    s_d.vp.amp_trim = v;
    /* s_amp_peak_lin() is called by drone_configure_wave_synth() via drone_rebuild(),
     * so the coalesced rebuild picks up the new effective peak. */
    if (s_d.source == DRONE_SRC_WAVE) drone_mark_dirty();
}

float drone_get_amp_trim(void) { return s_d.vp.amp_trim; }

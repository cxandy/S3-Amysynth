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
#include "sequencer_core.h"    /* sequencer_core_push_envelope */
#include "seq_clamp.h"
#include "amy.h"
#include "amy_helpers.h"
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
#define DRONE_PATCH_MAX    256
#define DRONE_GATE_MIN     0.05f   /* osc1 PULSE duty: tighter chop as it shrinks */
#define DRONE_GATE_MAX     0.95f
#define DRONE_SWING_MAX    66      /* percent of one subdivision, applied to odd steps */
#define DRONE_BLIP_MAX     1.0f    /* per-step downward filter-zap depth (0 = off)     */
#define DRONE_PAT_STEPS    8       /* steps per bar in a pattern mask                  */

/* ── Chord formulas (semitone intervals from root, -1 = unused) ──
 * Root-relative so the same shape plays in any key via drone_set_root_note(). */
static const int8_t s_chord_formulas[DRONE_CHORD_COUNT][DRONE_CHORD_MAX_NOTES] = {
    /* Minor 7th */ [DRONE_CHORD_MIN7] = {  0,  3,  7, 10, -1 },
    /* Major 7th */ [DRONE_CHORD_MAJ7] = {  0,  4,  7, 11, -1 },
    /* Minor 9th */ [DRONE_CHORD_MIN9] = {  0,  3,  7, 10, 14 },
    /* Major 9th */ [DRONE_CHORD_MAJ9] = {  0,  4,  7, 11, 14 },
    /* Sus4      */ [DRONE_CHORD_SUS4] = {  0,  5,  7, 12, -1 },
};

static const char *s_chord_names[DRONE_CHORD_COUNT] = {
    [DRONE_CHORD_MIN7] = "Min7",
    [DRONE_CHORD_MAJ7] = "Maj7",
    [DRONE_CHORD_MIN9] = "Min9",
    [DRONE_CHORD_MAJ9] = "Maj9",
    [DRONE_CHORD_SUS4] = "Sus4",
};

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
    drone_chord_t  chord;       /* chord preset the carrier plays    */
    uint8_t        root_note;   /* drone-local root (DRONE_ROOT_MIN..MAX) */
    float          resonance;
    float          amp_const;   /* 0..1 always-on carrier level      */
    float          amp_mod;     /* 0..1 stutter depth (LFO modulation)*/
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
    seq_env_t      env;         /* runtime-editable ADSR (graph editor) */
    bool           env_authored;/* true once the user commits a custom env */
} drone_state_t;

static drone_state_t s_d;

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

/* Count the notes in a chord formula (up to the first -1 sentinel). */
static uint8_t drone_chord_note_count(drone_chord_t chord)
{
    if (chord >= DRONE_CHORD_COUNT) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < DRONE_CHORD_MAX_NOTES; i++) {
        if (s_chord_formulas[chord][i] < 0) break;
        n++;
    }
    return n;
}

/* ── Synth configuration (WAVE mode) ──
 * Build a `voices`-voice synth, each voice = osc0 carrier (NOTE-following, so
 * its pitch comes from the voice's note-on) amplitude-gated by osc1 PULSE LFO.
 * A multi-voice main synth therefore sounds a chord when fed multiple notes. */
static void drone_configure_wave_synth(uint8_t synth, uint8_t voices)
{
    float lfo_hz = drone_lfo_hz();
    /* Carrier amplitude follows AMY's combine_controls_mult exactly:
     *   amp = const * (1 + mod * LFO),  LFO bipolar (-1..+1) from the PULSE osc.
     * const = always-on level, mod = stutter depth (mod=1 gates to silence on
     * the LFO-low half). Set directly (0..1), matching amp={'const':x,'mod':y}. */
    float amp_const = s_d.amp_const;
    float amp_mod   = s_d.amp_mod;

    /* Build-your-own synth: N voices, 2 oscs/voice, no patch. */
    amy_event *e = amy_helpers_event_begin();
    e->synth          = synth;
    e->num_voices     = voices;
    e->oscs_per_voice = DRONE_OSCS_PER_VC;
    amy_helpers_event_send(e);

    /* osc1 = PULSE LFO. Absolute Hz (note-follow off), full const amp.
     * The PULSE duty IS the gate length: 0.5 = 50/50 square (legacy), lower =
     * a shorter "on" fraction per subdivision = a tighter percussive chop. */
    e = amy_helpers_event_begin();
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

    /* osc0 = carrier. NOTE-following pitch (COEF_NOTE=1) so each voice plays its
     * own chord note; amp = const + mod(=osc1) gated, EG0 envelope; LPF24. */
    e = amy_helpers_event_begin();
    e->synth                  = synth;
    e->osc                    = 0;
    e->wave                   = s_d.wave;
    e->freq_coefs[COEF_NOTE]  = 1.0f;    /* follow the voice's note pitch */
    e->amp_coefs[COEF_CONST]  = amp_const;
    e->amp_coefs[COEF_MOD]    = amp_mod;
    e->amp_coefs[COEF_VEL]    = 0.0f;    /* velocity does not scale amp      */
    /* EG0 multiplies the whole amp (combine_controls_mult), so the ADSR
     * envelope shapes the drone swell/fade *around* the LFO stutter. */
    e->amp_coefs[COEF_EG0]    = 1.0f;
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
        drone_configure_wave_synth(DRONE_SYNTH_MAIN, chord_n);
        if (s_d.sub_enabled) {
            drone_configure_wave_synth(DRONE_SYNTH_SUB, DRONE_SUB_VOICES);
        }
    } else {
        drone_configure_patch_synth(DRONE_SYNTH_MAIN, chord_n);
        if (s_d.sub_enabled) {
            drone_configure_patch_synth(DRONE_SYNTH_SUB, DRONE_SUB_VOICES);
        }
    }
    /* Re-impose the user's custom ADSR if authored (rebuild/patch resets the
     * synth oscs). Deferred authority, matching the melodic + arp behaviour. */
    if (s_d.env_authored) {
        sequencer_core_push_envelope(DRONE_SYNTH_MAIN, &s_d.env);
        if (s_d.sub_enabled) {
            sequencer_core_push_envelope(DRONE_SYNTH_SUB, &s_d.env);
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
    const int8_t *formula = s_chord_formulas[s_d.chord];
    uint8_t chord_n = drone_chord_note_count(s_d.chord);
    if (chord_n < 1) chord_n = 1;

    for (uint8_t i = 0; i < chord_n; i++) {
        if (formula[i] < 0) break;
        int midi = SEQ_CLAMP_INT((int)s_d.root_note + (int)formula[i], 0, 127);
        drone_note(DRONE_SYNTH_MAIN, s_d.enabled, (float)midi);
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
    s_d.enabled      = false;
    s_d.source       = DRONE_SRC_WAVE;
    s_d.wave         = SAW_DOWN;
    s_d.chord        = DRONE_CHORD_MIN7;
    s_d.root_note    = DRONE_ROOT_DEFAULT;
    s_d.resonance    = 1.5f;
    s_d.amp_const    = 0.5f;     /* always-on level                  */
    s_d.amp_mod      = 0.5f;     /* moderate stutter depth (LFO mod) */
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
    s_d.env.attack_ms   = 200;
    s_d.env.decay_ms    = 300;
    s_d.env.sustain_pct = 100;
    s_d.env.release_ms  = 600;
    s_d.env.eg_type     = 0;   /* ENVELOPE_NORMAL */
    s_d.env_authored    = false;

    drone_rebuild();
    ESP_LOGI(TAG, "drone_core initialized (synths %u/%u)",
             DRONE_SYNTH_MAIN, DRONE_SYNTH_SUB);
}

void drone_core_service(void)
{
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
    drone_rebuild();
    if (s_d.enabled) drone_apply_enabled();
}

void drone_set_wave(uint16_t amy_wave)
{
    if (s_d.wave == amy_wave) return;
    s_d.wave = amy_wave;
    if (s_d.source == DRONE_SRC_WAVE) {
        drone_rebuild();
        if (s_d.enabled) drone_apply_enabled();
    }
}

void drone_set_chord(drone_chord_t chord)
{
    if (chord >= DRONE_CHORD_COUNT) return;
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

void drone_set_amp_const(float c)
{
    c = SEQ_CLAMP_F32(c, 0.0f, 1.0f);
    if (fabsf(s_d.amp_const - c) < 0.001f) return;
    s_d.amp_const = c;
    if (s_d.source == DRONE_SRC_WAVE) {
        drone_rebuild();
        if (s_d.enabled) drone_apply_enabled();
    }
}

void drone_set_amp_mod(float m)
{
    m = SEQ_CLAMP_F32(m, 0.0f, 0.95f); /* cap at 0.95: mod=1 silences floor completely */
    if (fabsf(s_d.amp_mod - m) < 0.001f) return;
    s_d.amp_mod = m;
    if (s_d.source == DRONE_SRC_WAVE) {
        drone_rebuild();
        if (s_d.enabled) drone_apply_enabled();
    }
}

void drone_set_rate(drone_rate_t rate)
{
    if (rate >= DRONE_RATE_COUNT) return;
    if (s_d.rate == rate) return;
    s_d.rate = rate;
    /* service() picks up the new LFO Hz on the next frame. Nudge immediately. */
    s_d.last_lfo_hz = -1.0f;
}

void drone_set_patch(uint16_t patch)
{
    patch = SEQ_CLAMP_U16((int)patch, DRONE_PATCH_MIN, DRONE_PATCH_MAX);
    if (s_d.patch == patch) return;
    s_d.patch = patch;
    if (s_d.source == DRONE_SRC_PATCH) {
        drone_rebuild();
        if (s_d.enabled) drone_apply_enabled();
    }
}

void drone_set_sub_enabled(bool on)
{
    if (s_d.sub_enabled == on) return;
    s_d.sub_enabled = on;
    if (on) {
        drone_rebuild();
        if (s_d.enabled) drone_apply_enabled();
    } else {
        int sub_midi = SEQ_CLAMP_INT((int)s_d.root_note + (int)s_d.sub_interval, 12, 108);
        drone_note(DRONE_SYNTH_SUB, false, (float)sub_midi);
    }
}

void drone_set_sub_interval(int8_t st)
{
    int v = SEQ_CLAMP_INT((int)st, -36, 0);
    if (s_d.sub_interval == (int8_t)v) return;
    s_d.sub_interval = (int8_t)v;
    if (s_d.sub_enabled) {
        drone_rebuild();
        if (s_d.enabled) drone_apply_enabled();
    }
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
    if (out) *out = s_d.env;
}

void drone_set_envelope(const seq_env_t *env)
{
    if (!env) return;
    s_d.env = *env;
    s_d.env_authored = true;
    sequencer_core_push_envelope(DRONE_SYNTH_MAIN, &s_d.env);
    if (s_d.sub_enabled) {
        sequencer_core_push_envelope(DRONE_SYNTH_SUB, &s_d.env);
    }
    ESP_LOGI(TAG, "drone env -> A%u D%u S%u%% R%u",
             (unsigned)s_d.env.attack_ms, (unsigned)s_d.env.decay_ms,
             (unsigned)s_d.env.sustain_pct, (unsigned)s_d.env.release_ms);
}

/* ── Getters ── */
bool           drone_get_enabled(void)      { return s_d.enabled; }
drone_source_t drone_get_source(void)       { return s_d.source; }
uint16_t       drone_get_wave(void)         { return s_d.wave; }
drone_chord_t  drone_get_chord(void)        { return s_d.chord; }
uint8_t        drone_get_root_note(void)    { return s_d.root_note; }
float          drone_get_resonance(void)    { return s_d.resonance; }
float          drone_get_amp_const(void)    { return s_d.amp_const; }
float          drone_get_amp_mod(void)      { return s_d.amp_mod; }
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
        default:       return "?";
    }
}

const char *drone_chord_name(drone_chord_t chord)
{
    if (chord >= DRONE_CHORD_COUNT) return "?";
    return s_chord_names[chord];
}

const char *drone_pattern_name(drone_pattern_t p)
{
    if (p >= DRONE_PAT_COUNT) return "?";
    return s_pattern_names[p];
}

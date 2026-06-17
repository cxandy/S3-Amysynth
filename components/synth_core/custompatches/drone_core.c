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
#include "synth_ui.h"      /* seq_state (live BPM) */
#include "sequencer_core.h"    /* sequencer_core_push_envelope */
#include "seq_clamp.h"
#include "amy.h"
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

/* Sub drone gate note (root). Its pitch follows the chord root via COEF_NOTE. */
#define DRONE_SUB_NOTE     33    /* A1-ish base; sub_interval applied on top   */

/* Limits */
#define DRONE_RES_MIN      0.1f
#define DRONE_RES_MAX      2.0f
#define DRONE_SWEEP_MIN    100.0f
#define DRONE_SWEEP_MAX    8000.0f
#define DRONE_BARS_MIN     1
#define DRONE_BARS_MAX     16
#define DRONE_PATCH_MIN    0
#define DRONE_PATCH_MAX    256

/* ── Chord matrix (MIDI notes, fixed width, -1 = unused) ──
 * Voiced in a comfortable mid register so the LPF24 + sweep keep them present.
 * The sub plays each chord's ROOT (notes[0]) an octave-ish below. */
static const int8_t s_chord_notes[DRONE_CHORD_COUNT][DRONE_CHORD_MAX_NOTES] = {
    /* Am7  : A2 C3 E3 G3        */ [DRONE_CHORD_AM7]   = { 45, 48, 52, 55, -1 },
    /* Fmaj7: F2 A2 C3 E3        */ [DRONE_CHORD_FMAJ7] = { 41, 45, 48, 52, -1 },
    /* Dm9  : D2 F2 A2 C3 E3     */ [DRONE_CHORD_DM9]   = { 38, 41, 45, 48, 52 },
    /* Cmaj9: C3 E3 G3 B3 D4     */ [DRONE_CHORD_CMAJ9] = { 48, 52, 55, 59, 62 },
    /* Gsus4: G2 C3 D3 G3        */ [DRONE_CHORD_GSUS4] = { 43, 48, 50, 55, -1 },
};

static const char *s_chord_names[DRONE_CHORD_COUNT] = {
    [DRONE_CHORD_AM7]   = "Am7",
    [DRONE_CHORD_FMAJ7] = "Fmaj7",
    [DRONE_CHORD_DM9]   = "Dm9",
    [DRONE_CHORD_CMAJ9] = "Cmaj9",
    [DRONE_CHORD_GSUS4] = "Gsus4",
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

/* ── State ── */
typedef struct {
    bool           enabled;
    drone_source_t source;
    uint16_t       wave;        /* AMY wave constant for the carrier */
    drone_chord_t  chord;       /* chord preset the carrier plays    */
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
    /* sweep phase, advanced each service tick (0..2pi) */
    float          last_lfo_hz; /* to avoid redundant LFO re-sends   */
    seq_env_t      env;         /* runtime-editable ADSR (graph editor) */
    bool           env_authored;/* true once the user commits a custom env */
} drone_state_t;

static drone_state_t s_d;

/* ── Scratch event ── */
static amy_event         s_ev;
static SemaphoreHandle_t s_ev_mutex = NULL;

static inline void d_ev_begin(void)
{
    xSemaphoreTake(s_ev_mutex, portMAX_DELAY);
    s_ev = amy_default_event();
}

static inline void d_ev_send(void)
{
    amy_add_event(&s_ev);
    xSemaphoreGive(s_ev_mutex);
}

/* ── Tempo helpers ── */

/* Beats per second from the live global BPM. */
static inline float drone_bps(void)
{
    uint16_t bpm = seq_state.bpm;
    if (bpm < 1) bpm = 1;
    return (float)bpm / 60.0f;
}

/* Stutter LFO frequency for the current rate + tempo. */
static inline float drone_lfo_hz(void)
{
    return drone_bps() * s_rate_mult[s_d.rate];
}

/* Count the notes in a chord preset (up to the first -1 sentinel). */
static uint8_t drone_chord_note_count(drone_chord_t chord)
{
    if (chord >= DRONE_CHORD_COUNT) return 0;
    uint8_t n = 0;
    for (uint8_t i = 0; i < DRONE_CHORD_MAX_NOTES; i++) {
        if (s_chord_notes[chord][i] < 0) break;
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
    d_ev_begin();
    s_ev.synth          = synth;
    s_ev.num_voices     = voices;
    s_ev.oscs_per_voice = DRONE_OSCS_PER_VC;
    d_ev_send();

    /* osc1 = PULSE LFO. Absolute Hz (note-follow off), full const amp. */
    d_ev_begin();
    s_ev.synth                 = synth;
    s_ev.osc                   = 1;
    s_ev.wave                  = PULSE;
    s_ev.duty_coefs[COEF_CONST]= 0.5f;
    s_ev.freq_coefs[COEF_CONST]= lfo_hz;
    s_ev.freq_coefs[COEF_NOTE] = 0.0f;     /* absolute Hz, ignore note */
    s_ev.amp_coefs[COEF_CONST] = 1.0f;
    s_ev.amp_coefs[COEF_VEL]   = 0.0f;     /* turn off the default vel/eg amp */
    s_ev.amp_coefs[COEF_EG0]   = 0.0f;
    d_ev_send();

    /* osc0 = carrier. NOTE-following pitch (COEF_NOTE=1) so each voice plays its
     * own chord note; amp = const + mod(=osc1) gated, EG0 envelope; LPF24. */
    d_ev_begin();
    s_ev.synth                  = synth;
    s_ev.osc                    = 0;
    s_ev.wave                   = s_d.wave;
    s_ev.freq_coefs[COEF_NOTE]  = 1.0f;    /* follow the voice's note pitch */
    s_ev.amp_coefs[COEF_CONST]  = amp_const;
    s_ev.amp_coefs[COEF_MOD]    = amp_mod;
    s_ev.amp_coefs[COEF_VEL]    = 0.0f;    /* velocity does not scale amp      */
    /* EG0 multiplies the whole amp (combine_controls_mult), so the ADSR
     * envelope shapes the drone swell/fade *around* the LFO stutter. */
    s_ev.amp_coefs[COEF_EG0]    = 1.0f;
    s_ev.mod_source             = 1;       /* osc1 of this voice (base-osc rel) */
    s_ev.filter_type            = FILTER_LPF24;
    s_ev.filter_freq_coefs[COEF_CONST] = s_d.sweep_hi;
    s_ev.resonance              = s_d.resonance;
    d_ev_send();
}

/* ── Synth configuration (PATCH mode) ──
 * Load an AMY patch onto the carrier synth; apply filter + resonance on top. */
static void drone_configure_patch_synth(uint8_t synth, uint8_t voices)
{
    d_ev_begin();
    s_ev.synth        = synth;
    s_ev.num_voices   = voices;
    s_ev.patch_number = s_d.patch;
    d_ev_send();

    /* Apply filter sweep + resonance on osc0 of the patch (best-effort). */
    d_ev_begin();
    s_ev.synth                  = synth;
    s_ev.osc                    = 0;
    s_ev.filter_type            = FILTER_LPF24;
    s_ev.filter_freq_coefs[COEF_CONST] = s_d.sweep_hi;
    s_ev.resonance              = s_d.resonance;
    d_ev_send();
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
    d_ev_begin();
    s_ev.synth     = synth;
    s_ev.midi_note = midi_note;
    s_ev.velocity  = on ? DRONE_GATE_VEL : 0.0f;
    d_ev_send();
}

/* Start/stop the sustained drone voices. The main synth gets a note-on per
 * chord note (one voice each); the sub gets a single note at the chord ROOT
 * shifted by sub_interval. On disable we release the same notes so the ADSR
 * release fades them out. */
static void drone_apply_enabled(void)
{
    const int8_t *notes = s_chord_notes[s_d.chord];
    uint8_t chord_n = drone_chord_note_count(s_d.chord);
    if (chord_n < 1) chord_n = 1;

    for (uint8_t i = 0; i < chord_n; i++) {
        if (notes[i] < 0) break;
        drone_note(DRONE_SYNTH_MAIN, s_d.enabled, (float)notes[i]);
    }

    if (s_d.sub_enabled) {
        int root = (notes[0] >= 0) ? notes[0] : DRONE_SUB_NOTE;
        int sub  = SEQ_CLAMP_INT(root + (int)s_d.sub_interval, 12, 108);
        drone_note(DRONE_SYNTH_SUB, s_d.enabled, (float)sub);
    }
}

/* Push the current filter cutoff to a synth's osc0. */
static void drone_push_cutoff(uint8_t synth, float cutoff)
{
    d_ev_begin();
    s_ev.synth = synth;
    s_ev.osc   = 0;
    s_ev.filter_freq_coefs[COEF_CONST] = cutoff;
    d_ev_send();
}

/* ── Public API ── */

void drone_core_init(void)
{
    if (s_ev_mutex == NULL) {
        s_ev_mutex = xSemaphoreCreateMutex();
    }

    memset(&s_d, 0, sizeof(s_d));
    s_d.enabled      = false;
    s_d.source       = DRONE_SRC_WAVE;
    s_d.wave         = SAW_DOWN;
    s_d.chord        = DRONE_CHORD_AM7;
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
        d_ev_begin();
        s_ev.synth                  = DRONE_SYNTH_MAIN;
        s_ev.osc                    = 1;
        s_ev.freq_coefs[COEF_CONST] = lfo_hz;
        s_ev.freq_coefs[COEF_NOTE]  = 0.0f;
        d_ev_send();
        if (s_d.sub_enabled) {
            d_ev_begin();
            s_ev.synth                  = DRONE_SYNTH_SUB;
            s_ev.osc                    = 1;
            s_ev.freq_coefs[COEF_CONST] = lfo_hz;
            s_ev.freq_coefs[COEF_NOTE]  = 0.0f;
            d_ev_send();
        }
        s_d.last_lfo_hz = lfo_hz;
    }

    /* Filter sweep phase is a pure function of the global musical clock, NOT of
     * how often this service runs. At 48 PPQ one bar (4 beats) = 192 ticks, so
     * the sweep period is sweep_bars*192 ticks. Phase = 2pi * (tick % period) /
     * period. This is frame-rate-independent (UI jitter / skipped frames cannot
     * drift it) and beat-locked: it advances with tempo automatically and stays
     * coherent with the sequencer/arp bar grid across BPM changes. */
    uint32_t period_ticks = (uint32_t)s_d.sweep_bars * (uint32_t)(AMY_SEQUENCER_PPQ * 4);
    if (period_ticks < 1) period_ticks = 1;
    uint32_t pos = sequencer_ticks() % period_ticks;
    float phase = (2.0f * (float)M_PI) * ((float)pos / (float)period_ticks);

    float mid  = 0.5f * (s_d.sweep_lo + s_d.sweep_hi);
    float half = 0.5f * (s_d.sweep_hi - s_d.sweep_lo);
    float cutoff = mid + half * sinf(phase);

    drone_push_cutoff(DRONE_SYNTH_MAIN, cutoff);
    if (s_d.sub_enabled) {
        drone_push_cutoff(DRONE_SYNTH_SUB, cutoff * 0.5f);  /* sub sits lower */
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

void drone_set_resonance(float r)
{
    r = SEQ_CLAMP_F32(r, DRONE_RES_MIN, DRONE_RES_MAX);
    if (fabsf(s_d.resonance - r) < 0.001f) return;
    s_d.resonance = r;
    /* Resonance is an osc0 param; push without full rebuild. */
    d_ev_begin();
    s_ev.synth     = DRONE_SYNTH_MAIN;
    s_ev.osc       = 0;
    s_ev.resonance = r;
    d_ev_send();
    if (s_d.sub_enabled) {
        d_ev_begin();
        s_ev.synth     = DRONE_SYNTH_SUB;
        s_ev.osc       = 0;
        s_ev.resonance = r;
        d_ev_send();
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
    m = SEQ_CLAMP_F32(m, 0.0f, 1.0f);
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
        drone_note(DRONE_SYNTH_SUB, false, (float)DRONE_SUB_NOTE);
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

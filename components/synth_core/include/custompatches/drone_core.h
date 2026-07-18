#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"     /* seq_env_t */
#include "chord_types.h"   /* chord_type_t, chord_type_name() */
#include "quantizer.h"     /* quantizer_chord_intervals() */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Stutter house drone ──────────────────────────────────────────────────
 * A standalone, customizable drone synth translated from the AMYboard
 * "stutter house drone" sketch. It is fully independent of the sequencer
 * layers and the arp: it owns its own AMY synth slots (above the existing
 * 0..63 map), its own state, and its own screen.
 *
 * Sound design (WAVE mode):
 *   osc0 = carrier (SAW/SQUARE/TRI/SINE), NOTE-following so each voice plays a
 *          chord note; amplitude gated by a continuously-running square LFO via
 *          mod_source.  AMY 1.2.12 dB/exp amp model:
 *            amp = const_sent * 10^(3*m*LFO),  LFO in {-1,+1}
 *          ON-beat (LFO=+1) = peak_lin; OFF-beat (LFO=-1) = peak_lin * 10^(-duck_db/20).
 *          See drone_set_amp_peak / drone_set_amp_duck for the knob mapping.
 *   osc1 = PULSE LFO at a tempo-locked note division ("stutter rate").
 *   LPF24 filter whose cutoff is swept slowly (tempo-locked, in bars).
 *   The carrier plays a CHORD (one voice per note); a single "sub" voice
 *   tracks the chord ROOT a fixed interval below.
 *
 * PATCH mode: the carrier synth loads an AMY patch preset instead of the raw
 * 2-osc voice; the patch defines its own oscillators and amplitude. The
 * square-LFO stutter and the const/mod amp controls are WAVE-mode only; PATCH
 * mode keeps the chord voicing, filter sweep and resonance.
 *
 * Enable follows the global transport tempo (the LFO frequency and sweep
 * period are derived from seq_state.bpm). */

typedef enum {
    DRONE_SRC_WAVE  = 0,
    DRONE_SRC_PATCH = 1,
} drone_source_t;

/* Stutter LFO note divisions (tempo-locked). */
typedef enum {
    DRONE_RATE_1_4  = 0,
    DRONE_RATE_1_8  = 1,
    DRONE_RATE_1_16 = 2,
    DRONE_RATE_1_32 = 3,
    DRONE_RATE_COUNT
} drone_rate_t;

/* Chord presets.  The carrier plays the chord (one voice per note); the sub
 * tracks the chord ROOT only.  Chord shapes come from the shared
 * quantizer_chord_intervals(chord_type_t) table.  The drone caps voice count
 * at DRONE_CHORD_MAX_NOTES regardless of how many intervals the shared row
 * contains. */
#define DRONE_CHORD_MAX_NOTES 5

/* Stutter step patterns. Each is an 8-step per-bar on/off mask consulted by the
 * filter-blip service path: steps whose bit is 0 are skipped (filter closed) so
 * the even stutter grid becomes a rhythm. FULL (all steps on) = legacy behavior. */
typedef enum {
    DRONE_PAT_FULL   = 0,   /* 1 1 1 1 1 1 1 1  — every subdivision fires */
    DRONE_PAT_FOUR   = 1,   /* 1 0 1 0 1 0 1 0  — four-on-floor / downbeats */
    DRONE_PAT_OFFBEAT= 2,   /* 0 1 0 1 0 1 0 1  — offbeat skip             */
    DRONE_PAT_GALLOP = 3,   /* 1 1 0 1 1 0 1 0  — galloping push           */
    DRONE_PAT_DUB    = 4,   /* 1 0 0 0 1 0 1 0  — sparse dub stab          */
    DRONE_PAT_COUNT
} drone_pattern_t;

/* ── Lifecycle ── */
void drone_core_init(void);

/* Per-UI-frame service: advances the tempo-locked filter sweep and keeps the
 * LFO frequency in sync with the current BPM. Cheap no-op while disabled.
 * Call once per UI frame, like arp_core_service(). */
void drone_core_service(void);

/* ── Parameter setters ── */
void drone_set_enabled(bool on);          /* sustained note-on/off of the voices */
void drone_set_source(drone_source_t src);/* WAVE <-> PATCH (rebuilds the synths) */
void drone_set_wave(uint16_t amy_wave);   /* SAW_DOWN/SAW_UP/PULSE/TRIANGLE/SINE  */
void drone_set_chord(chord_type_t chord); /* chord preset the carrier plays       */
void drone_set_root_note(uint8_t midi_note); /* drone-local root (24..72); independent of global quantizer root */
void drone_set_resonance(float r);        /* filter resonance                     */
/* Carrier amplitude — Peak/Duck dB model (WAVE mode only).
 *
 * PEAK knob 0..1 (linear):  peak_lin = peak_knob.
 *   At 1.0 the on-beat (LFO=+1) carrier plays at full-scale; at 0.0 silent.
 *   Default 0.5.
 *
 * DUCK knob 0..1 (linear):  duck_db = duck_knob * 40.0.
 *   How many dB the off-beat (LFO=-1) is attenuated below the on-beat.
 *   0 = no ducking; 1 = -40 dB floor.  Default 0.5 (20 dB duck).
 *
 * AMY engine coefs (derived internally — do not use these directly):
 *   m          = duck_db / 120
 *   const_sent = peak_lin * 10^(-duck_db / 40)
 * so that: ON-beat = peak_lin, OFF-beat = peak_lin * 10^(-duck_db / 20). */
void drone_set_amp_peak(float c);         /* 0.0..1.0 on-beat level (linear)      */
void drone_set_amp_duck(float m);         /* 0.0..1.0 duck depth (0=flat,1=-40dB) */
void drone_set_rate(drone_rate_t rate);   /* stutter LFO note division            */
void drone_set_patch(uint16_t patch);     /* PATCH-mode preset                    */

/* The drone's explicit opt-out from the shared patch catalog: true for the
 * ranges its excitation model can't play (NOISE, KS, bass presets, FM voices
 * — anything past its wavetable/raw-wave ceiling). Feeds the drone cycling
 * domain's `excluded` predicate (ui_patch_cycle.c) and drone_set_patch()'s
 * snap-back, so the exclusion list lives in exactly one place. */
bool drone_patch_excluded(uint16_t patch);
void drone_set_sub_enabled(bool on);      /* sub drone on/off                     */
void drone_set_sub_interval(int8_t st);   /* sub interval, semitones (e.g. -12)   */
void drone_set_sweep_lo(float hz);        /* filter sweep low cutoff              */
void drone_set_sweep_hi(float hz);        /* filter sweep high cutoff             */
void drone_set_sweep_bars(uint8_t bars);  /* sweep period in bars (tempo-locked)  */
void drone_set_gate_len(float frac);      /* 0.05..0.95 chop length (osc1 duty)   */
void drone_set_swing(uint8_t pct);        /* 0..66 swing on the stutter grid      */
void drone_set_blip(float depth);         /* 0..1 per-step filter-zap depth       */
void drone_set_pattern(drone_pattern_t p);/* per-bar step on/off mask             */

/* ── Runtime-editable ADSR envelope (shared graph editor) ── */
void drone_get_envelope(seq_env_t *out);
void drone_set_envelope(const seq_env_t *env);

/* ── Second envelope (EG1, shared graph editor) ──
 * Independent breakpoint generator, timing-only (no coef wiring is added to
 * the drone's own WAVE-mode patches by this feature — see AMY-EDITS.md /
 * docs/handoff/a1-second-envelope.md for why). Still useful for a PATCH-mode
 * drone whose loaded AMY patch already routes its own bp1 internally. */
void drone_get_envelope2(seq_env_t *out);
void drone_set_envelope2(const seq_env_t *env);

/* ── Editor live-preview (AMY only; drone store + authored flags untouched) ──
 * Audition scratch editor values on the drone's main (and, when enabled, sub)
 * synth. Cancel restores by calling these again with the stored values. */
void drone_preview_envelope(const seq_env_t *env);
void drone_preview_envelope2(const seq_env_t *env);

/* ── Getters (for the list UI) ── */
bool           drone_get_enabled(void);
drone_source_t drone_get_source(void);
uint16_t       drone_get_wave(void);
chord_type_t   drone_get_chord(void);
uint8_t        drone_get_root_note(void);
float          drone_get_resonance(void);
float          drone_get_amp_peak(void);   /* 0..1 on-beat level knob value        */
float          drone_get_amp_duck(void);   /* 0..1 duck depth knob value           */
void           drone_set_amp_trim(float v);/* 0..1 per-target trim (default 1.0)   */
float          drone_get_amp_trim(void);   /* 0..1 per-target trim value           */

/* Visualiser helper: returns ON-beat (ceil) and OFF-beat (floor) amplitudes
 * as 0..1 normalised linear values, computed from the same dB math the AMY
 * engine uses.  Both pointers may be NULL if not needed. */
void           drone_get_amp_levels_norm(float *floor_norm, float *ceil_norm);
drone_rate_t   drone_get_rate(void);
uint16_t       drone_get_patch(void);
bool           drone_get_sub_enabled(void);
int8_t         drone_get_sub_interval(void);
float          drone_get_sweep_lo(void);
float          drone_get_sweep_hi(void);
uint8_t        drone_get_sweep_bars(void);
float          drone_get_gate_len(void);
uint8_t        drone_get_swing(void);
float          drone_get_blip(void);
drone_pattern_t drone_get_pattern(void);

/* Human-readable names for the enum values (for display). */
const char *drone_rate_name(drone_rate_t rate);
const char *drone_wave_name(uint16_t amy_wave);
const char *drone_chord_name(chord_type_t chord);   /* wraps chord_type_name() */
const char *drone_pattern_name(drone_pattern_t p);

#ifdef __cplusplus
}
#endif

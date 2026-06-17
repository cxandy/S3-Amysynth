#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "display_seq.h"   /* seq_env_t */

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
 *          mod_source. amp = const * (1 + mod*LFO) (combine_controls_mult).
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

/* Chord presets. The carrier plays the chord (one voice per note); the sub
 * tracks the chord ROOT only. Each preset is up to DRONE_CHORD_MAX_NOTES MIDI
 * notes; the matrix is fixed-width and padded with -1 sentinels. */
#define DRONE_CHORD_MAX_NOTES 5

typedef enum {
    DRONE_CHORD_AM7   = 0,
    DRONE_CHORD_FMAJ7 = 1,
    DRONE_CHORD_DM9   = 2,
    DRONE_CHORD_CMAJ9 = 3,
    DRONE_CHORD_GSUS4 = 4,
    DRONE_CHORD_COUNT
} drone_chord_t;

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
void drone_set_chord(drone_chord_t chord);/* chord preset the carrier plays       */
void drone_set_resonance(float r);        /* filter resonance                     */
/* Carrier amplitude coefs, matching AMY's amp={'const':x,'mod':y} (0.0..1.0).
 * amp = const * (1 + mod * LFO), LFO bipolar (-1..+1): const sets the always-on
 * level, mod the stutter depth (mod=1 reaches silence on the LFO-low half). */
void drone_set_amp_const(float c);        /* 0.0..1.0 always-on carrier level     */
void drone_set_amp_mod(float m);          /* 0.0..1.0 stutter depth               */
void drone_set_rate(drone_rate_t rate);   /* stutter LFO note division            */
void drone_set_patch(uint16_t patch);     /* PATCH-mode preset                    */
void drone_set_sub_enabled(bool on);      /* sub drone on/off                     */
void drone_set_sub_interval(int8_t st);   /* sub interval, semitones (e.g. -12)   */
void drone_set_sweep_lo(float hz);        /* filter sweep low cutoff              */
void drone_set_sweep_hi(float hz);        /* filter sweep high cutoff             */
void drone_set_sweep_bars(uint8_t bars);  /* sweep period in bars (tempo-locked)  */

/* ── Runtime-editable ADSR envelope (shared graph editor) ── */
void drone_get_envelope(seq_env_t *out);
void drone_set_envelope(const seq_env_t *env);

/* ── Getters (for the list UI) ── */
bool           drone_get_enabled(void);
drone_source_t drone_get_source(void);
uint16_t       drone_get_wave(void);
drone_chord_t  drone_get_chord(void);
float          drone_get_resonance(void);
float          drone_get_amp_const(void);
float          drone_get_amp_mod(void);
drone_rate_t   drone_get_rate(void);
uint16_t       drone_get_patch(void);
bool           drone_get_sub_enabled(void);
int8_t         drone_get_sub_interval(void);
float          drone_get_sweep_lo(void);
float          drone_get_sweep_hi(void);
uint8_t        drone_get_sweep_bars(void);

/* Human-readable names for the enum values (for display). */
const char *drone_rate_name(drone_rate_t rate);
const char *drone_wave_name(uint16_t amy_wave);
const char *drone_chord_name(drone_chord_t chord);

#ifdef __cplusplus
}
#endif

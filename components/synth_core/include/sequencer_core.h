#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "display_seq.h"   /* seq_layer_type_t, seq_layer_t, SEQ_* defines */
#include "chord_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Virtual wave-patch IDs ─────────────────────────────────────────────────
 * Patch numbers beyond the 0..256 built-in (Juno/DX7/piano) range.
 * Intercepted before amy_send_patch() so they never collide with real patches.
 * Melodic tracks, arp, and drone all use these constants for wave-patch routing.
 * Drone: only SEQ_PATCH_WAVE_BASE..SEQ_PATCH_TRIANGLE are valid (NOISE/KS excluded).
 * Arp:   full range SEQ_PATCH_WAVE_BASE..SEQ_PATCH_WAVE_MAX. */
#define SEQ_PATCH_WAVE_BASE   257
#define SEQ_PATCH_SINE        257   /* AMY SINE     */
#define SEQ_PATCH_SAW_DOWN    258   /* AMY SAW_DOWN */
#define SEQ_PATCH_SAW_UP      259   /* AMY SAW_UP   */
#define SEQ_PATCH_PULSE       260   /* AMY PULSE    */
#define SEQ_PATCH_TRIANGLE    261   /* AMY TRIANGLE */
#define SEQ_PATCH_NOISE       262   /* AMY NOISE    — arp only; drone excludes */
#define SEQ_PATCH_KS          263   /* AMY KS       — arp only; drone excludes */
#define SEQ_PATCH_WAVE_MAX    263

/* ── Multi-osc bass presets (melodic only; oscs_per_voice=2) ────────────
 * Intercepted before amy_send_patch() — never routed to the Juno/DX7 loader.
 * Osc 0 is the primary audio carrier; osc 1 is the secondary/sub layer.
 * Osc 0 envelope is overwritten by sequencer_configure_melodic_envelope()
 * after configure; characteristic sound comes from osc structure + filter. */
#define SEQ_PATCH_BASS_BASE   264
#define SEQ_PATCH_BASS_1      264   /* Classic Sub-Heavy Detune (PULSE + detuned SAW, LPF24) */
#define SEQ_PATCH_BASS_2      265   /* Solid Sine-Reinforced Acid/Pluck (SINE + SAW, LPF24) */
#define SEQ_PATCH_BASS_3      266   /* FM DX7-Style (SINE carrier + sub-octave SINE, DX7 env) */
#define SEQ_PATCH_BASS_MAX    266

/* ── BPM range & default (shared with synth_ui for boot initialisation) ── */
#define SEQ_DEFAULT_BPM  108

/* ── Shared LFO helper ────────────────────────────────────────────────────
 * Convert a tempo-synced lfo_rate_t to a frequency in Hz for the given BPM.
 * Defined in sequencer_core.c; also used by arp_core.c (avoids duplication). */
float lfo_rate_to_hz(lfo_rate_t rate, uint16_t bpm);

/* ── Core lifecycle ── */
void sequencer_core_init(void);
void sequencer_core_set_playing(bool playing);
void sequencer_core_set_bpm(uint16_t bpm);
uint16_t sequencer_core_get_bpm(void);
void sequencer_core_set_quantizer_enabled(bool enabled);
void sequencer_core_set_quantizer_root_note(uint8_t root_note);
void sequencer_core_set_quantizer_scale(uint8_t scale_index);
bool sequencer_core_get_quantizer_enabled(void);
uint8_t sequencer_core_get_quantizer_root_note(void);
uint8_t sequencer_core_get_quantizer_scale(void);
void sequencer_core_set_melodic_patch(uint16_t patch_number);
uint16_t sequencer_core_get_melodic_patch(void);

/* ── Drum per-track patch (curated Juno list) ──
 * Drum layers are per-track Juno-patch layers: each track owns its own patch.
 * set/get operate on (layer_idx, track); cycle steps the curated drum list by
 * `dir` (+/-1) and returns the newly-applied patch. No-ops for non-drum/out-of-
 * range layers. */
void     sequencer_core_set_drum_patch(uint8_t layer_idx, uint8_t track,
                                       uint16_t patch_number);
uint16_t sequencer_core_get_drum_patch(uint8_t layer_idx, uint8_t track);
uint16_t sequencer_core_cycle_drum_patch(uint8_t layer_idx, uint8_t track,
                                         int dir);

/* ── Drum sound source (whole-layer Synth vs PCM) ──
 * SEQ_DRUM_SYNTH = tonal AMY patches (Juno/DX7) per track, shaped by the
 * accent/jitter velocity + per-track pitch + EG0 path. SEQ_DRUM_PCM = the
 * built-in Roland-808 samples per track, routed through the SAME velocity +
 * pitch path (render_pcm tunes the sample by midi_note), so PCM hits are
 * humanized rather than the old fixed-velocity "machine-gun" feel. The toggle
 * applies to the entire drum layer; set re-configures the drum synth slots in
 * place (safe while playing). */
typedef enum {
    SEQ_DRUM_SYNTH = 0,
    SEQ_DRUM_PCM   = 1,
} seq_drum_engine_t;

void              sequencer_core_set_drum_engine(seq_drum_engine_t engine);
seq_drum_engine_t sequencer_core_get_drum_engine(void);

/* ── Per-row melodic ADSR envelope (runtime-editable) ──
 * Scoped per row (per track); each row has its own AMY synth, so its envelope
 * is fully independent. See seq_env_t in display_seq.h for the extension path
 * to per-step. get returns false for non-melodic/out-of-range. set clamps,
 * stores, and pushes the envelope to that row's own synth. */
bool sequencer_core_get_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         seq_env_t *out);
void sequencer_core_set_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         const seq_env_t *env);

/* ── Per-track amplitude trim (graph editor amp mode) ──
 * 0..1 multiplier on note velocity. Default 1.0 (unity; set init in add_layer).
 * get returns 1.0 for invalid layer/track. set is a store-only op; the new
 * value is applied on the next sequencer_emit_step() call for that track. */
float sequencer_core_get_melodic_amp_scale(uint8_t layer_idx, uint8_t track);
void  sequencer_core_set_melodic_amp_scale(uint8_t layer_idx, uint8_t track,
                                           float v);

/* ── Per-row melodic filter (runtime-editable) ──
 * Parallel to the envelope system. Default: enabled=false (bypass).
 * set stores + pushes to that row's synth immediately.
 * get returns false for non-melodic/out-of-range layers. */
bool sequencer_core_get_melodic_filter(uint8_t layer_idx, uint8_t track,
                                       seq_filter_t *out);
void sequencer_core_set_melodic_filter(uint8_t layer_idx, uint8_t track,
                                       const seq_filter_t *f);

/* Push a filter directly to an arbitrary AMY synth slot (shared by arp/drone). */
void sequencer_core_push_filter(uint8_t synth, const seq_filter_t *f);

/* ── Per-track melodic LFO (tempo-synced software modulator) ─────────────
 * Modulates filter cutoff, amp, pitch, or pan at a rate derived from BPM.
 * sequencer_core_lfo_service() must be called periodically (~20 Hz). */
void sequencer_core_set_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    const seq_lfo_t *lfo);
bool sequencer_core_get_melodic_lfo(uint8_t layer_idx, uint8_t track,
                                    seq_lfo_t *out);
void sequencer_core_lfo_service(void);

/* Returns the current playhead step for the given layer (0..num_steps-1).
 * When paused the last computed step is returned (display freezes). */
uint8_t sequencer_core_get_current_step(uint8_t layer_idx);

/* ── Layer management ── */

/* Add a new layer. Returns the new layer index (0..MAX_LAYERS-1), or
 * 0xFF if the layer table is full. Configures the AMY synth immediately. */
uint8_t          sequencer_core_add_layer(seq_layer_type_t type, uint8_t num_steps);
/* Delete a melodic layer by index. Returns false if the layer is the drum
 * layer (idx 0), the only remaining layer, or out of range. Cancels all
 * scheduled tags for all layers, frees the deleted layer's AMY oscillator
 * slots, compacts the layer array, and resyncs surviving layers if playing. */
bool             sequencer_core_delete_layer(uint8_t layer_idx);
uint8_t          sequencer_core_get_num_layers(void);
seq_layer_type_t sequencer_core_get_layer_type(uint8_t layer_idx);

/* ── Per-layer step / note control ── */
void    sequencer_core_set_step(uint8_t layer_idx, uint8_t track,
                                uint8_t step, bool state);
void    sequencer_core_set_track_midi_note(uint8_t layer_idx, uint8_t track,
                                           uint8_t midi_note);
uint8_t sequencer_core_get_track_midi_note(uint8_t layer_idx, uint8_t track);
uint8_t sequencer_core_get_track_source_note(uint8_t layer_idx, uint8_t track);

/* ── Generic envelope push ────────────────────────────────────────────────
 * Push an ADSR (EG0 breakpoint set) to an arbitrary AMY synth slot's voices.
 * Shared by the arp and the standalone drone so they reuse the exact same EG0
 * delta path as the melodic layers. The synth's osc0 must have its amp EG0 coef
 * enabled (patch-loaded synths do by default; the drone enables it explicitly).
 * Does nothing for an out-of-range eg_type. */
void sequencer_core_push_envelope(uint8_t synth, const seq_env_t *env);

/* ── Arpeggiator support ──────────────────────────────────────────────────
 * The arp lives in its own module (arp_core) but routes all AMY traffic
 * through these helpers so it reuses the one shared event buffer + mutex and
 * never races the sequencer. The arp owns a dedicated synth slot. */

/* Dedicated AMY synth slot reserved for the arp. */
uint8_t sequencer_core_arp_synth(void);

/* Default voice count for the arp synth. */
uint8_t sequencer_core_arp_voices(void);

/* Clamp a MIDI note to the melodic playable range (C1..C7). */
uint8_t sequencer_core_clamp_melodic_note(int32_t midi_note);

/* (Re)configure the arp synth with a patch + voice count (flags = 0). */
void sequencer_core_arp_configure(uint16_t patch_number, uint8_t num_voices);

/* Schedule a repeating note-on + note-off pair on the arp synth.
 *  tag_base  : unique tag for this arp step (off uses tag_base+1)
 *  midi_note : already-snapped/clamped pitch to play
 *  velocity  : 0..1 note-on velocity
 *  tick_on   : sequence tick for the note-on (must be >=1)
 *  gate_ticks: ticks the note is held before the note-off
 *  period    : repeat period in ticks (full arp cycle length) */
void sequencer_core_arp_emit_note(uint32_t tag_base, uint8_t midi_note,
                                  float velocity, uint32_t tick_on,
                                  uint32_t gate_ticks, uint32_t period);

/* Cancel a previously-scheduled arp note (clears tag_base and tag_base+1). */
void sequencer_core_arp_clear_note(uint32_t tag_base);

/* Base tag for arp events — well above the sequencer's tag space. */
uint32_t sequencer_core_arp_tag_base(void);

/* ── Per-track repeat rate ────────────────────────────────────────────────
 * A track with repeat_rate=N fires every N bars instead of every bar.
 * Re-emits all steps immediately so AMY picks up the new period. */
void              sequencer_core_set_track_repeat_rate(uint8_t layer_idx,
                                                       uint8_t track,
                                                       seq_repeat_rate_t rate);
seq_repeat_rate_t sequencer_core_get_track_repeat_rate(uint8_t layer_idx,
                                                       uint8_t track);

/* ── Global chord progression ─────────────────────────────────────────────
 * A list of (root, chord_type, duration_bars) entries that auto-advances.
 * When enabled, all melodic layer quantizers and the arp follow the active
 * chord. progression_service() must be called at ~20 Hz from the UI task. */
void    sequencer_core_progression_set_enabled(bool en);
bool    sequencer_core_progression_get_enabled(void);
void    sequencer_core_progression_set_entry(uint8_t idx, uint8_t root,
                                             chord_type_t chord_type,
                                             uint8_t duration_bars);
void    sequencer_core_progression_get_entry(uint8_t idx, uint8_t *root,
                                             chord_type_t *chord_type,
                                             uint8_t *duration_bars);
void    sequencer_core_progression_set_count(uint8_t count);
uint8_t sequencer_core_progression_get_count(void);
uint8_t sequencer_core_progression_get_current(void);
uint8_t sequencer_core_progression_get_max(void);
uint8_t sequencer_core_progression_bars_in_current(void);
bool    sequencer_core_progression_add_entry(void);
void    sequencer_core_progression_delete_entry(uint8_t idx);
void    sequencer_core_progression_service(void);

/* Manual per-layer chord override (used when global progression is off). */
void sequencer_core_progression_set_layer_chord(uint8_t layer_idx,
                                                uint8_t root,
                                                chord_type_t chord_type);
void sequencer_core_progression_clear_layer_chord(uint8_t layer_idx);
void sequencer_core_get_layer_chord(uint8_t layer_idx, bool *chord_mode,
                                    uint8_t *root, chord_type_t *chord_type);

#ifdef __cplusplus
}
#endif

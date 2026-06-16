#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "priv_u8g2_seq.h"   /* seq_layer_type_t, seq_layer_t, SEQ_* defines */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Core lifecycle ── */
void sequencer_core_init(void);
void sequencer_core_set_playing(bool playing);
void sequencer_core_set_bpm(uint16_t bpm);
void sequencer_core_set_quantizer_enabled(bool enabled);
void sequencer_core_set_quantizer_root_note(uint8_t root_note);
void sequencer_core_set_quantizer_scale(uint8_t scale_index);
bool sequencer_core_get_quantizer_enabled(void);
uint8_t sequencer_core_get_quantizer_root_note(void);
uint8_t sequencer_core_get_quantizer_scale(void);
void sequencer_core_set_melodic_patch(uint16_t patch_number);
uint16_t sequencer_core_get_melodic_patch(void);

/* ── Per-row melodic ADSR envelope (runtime-editable) ──
 * Scoped per row (per track); each row has its own AMY synth, so its envelope
 * is fully independent. See seq_env_t in priv_u8g2_seq.h for the extension path
 * to per-step. get returns false for non-melodic/out-of-range. set clamps,
 * stores, and pushes the envelope to that row's own synth. */
bool sequencer_core_get_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         seq_env_t *out);
void sequencer_core_set_melodic_envelope(uint8_t layer_idx, uint8_t track,
                                         const seq_env_t *env);

/* Returns the current playhead step for the given layer (0..num_steps-1).
 * When paused the last computed step is returned (display freezes). */
uint8_t sequencer_core_get_current_step(uint8_t layer_idx);

/* ── Layer management ── */

/* Add a new layer. Returns the new layer index (0..MAX_LAYERS-1), or
 * 0xFF if the layer table is full. Configures the AMY synth immediately. */
uint8_t          sequencer_core_add_layer(seq_layer_type_t type, uint8_t num_steps);
uint8_t          sequencer_core_get_num_layers(void);
seq_layer_type_t sequencer_core_get_layer_type(uint8_t layer_idx);

/* ── Per-layer step / note control ── */
void    sequencer_core_set_step(uint8_t layer_idx, uint8_t track,
                                uint8_t step, bool state);
void    sequencer_core_set_track_midi_note(uint8_t layer_idx, uint8_t track,
                                           uint8_t midi_note);
uint8_t sequencer_core_get_track_midi_note(uint8_t layer_idx, uint8_t track);
uint8_t sequencer_core_get_track_source_note(uint8_t layer_idx, uint8_t track);

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

#ifdef __cplusplus
}
#endif

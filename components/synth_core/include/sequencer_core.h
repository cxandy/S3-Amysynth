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
 * Drone: SEQ_PATCH_WAVE_BASE..SEQ_PATCH_TRIANGLE plus (AMY_WAVETABLE)
 *        SEQ_PATCH_WAVETABLE_BASE..MAX; NOISE/KS/bass are excluded (see
 *        DRONE_PATCH_MAX in drone_core.c).
 * Arp:   full range SEQ_PATCH_WAVE_BASE..SEQ_PATCH_WAVE_MAX, plus
 *        (AMY_WAVETABLE) SEQ_PATCH_WAVETABLE_BASE..MAX. */
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

/* ── Wavetable virtual patches (melodic, arp, drone; AMY_WAVETABLE only) ──
 * One virtual patch per built-in wavetable bank (see pcm_tiny.h /
 * pcm_wavetable_base). wave=WAVETABLE, preset=pcm_wavetable_base+index.
 * Intercepted the same way as SEQ_PATCH_WAVE_BASE — never sent to
 * amy_send_patch(). Range kept separate from SEQ_PATCH_WAVE_BASE so it can be
 * added without renumbering the bass presets above. */
#if CONFIG_AMY_WAVETABLE
#define SEQ_PATCH_WAVETABLE_BASE  267
#define SEQ_PATCH_WAVETABLE_0     267   /* 111.WAV      */
#define SEQ_PATCH_WAVETABLE_1     268   /* BRAIDS01.WAV */
#define SEQ_PATCH_WAVETABLE_2     269   /* PPG_WA00.WAV */
#define SEQ_PATCH_WAVETABLE_3     270   /* SINE2SAW.WAV */
#define SEQ_PATCH_WAVETABLE_4     271   /* VIRAL.WAV    */
#define SEQ_PATCH_WAVETABLE_MAX   271
#endif

/* True for any raw-waveform virtual patch (SINE..KS, and — when AMY_WAVETABLE
 * is compiled in — the wavetable bank patches). Shared by the melodic
 * synth/LFO configurators so both ranges route the same way (direct oscillator
 * config, no amy_send_patch(), native-LFO eligible). */
static inline bool sequencer_core_is_wave_patch(uint16_t patch)
{
    if (patch >= SEQ_PATCH_WAVE_BASE && patch <= SEQ_PATCH_WAVE_MAX) return true;
#if CONFIG_AMY_WAVETABLE
    if (patch >= SEQ_PATCH_WAVETABLE_BASE && patch <= SEQ_PATCH_WAVETABLE_MAX) return true;
#endif
    return false;
}

/* ── DX7-style 6-operator FM/ALGO voices (melodic only; oscs_per_voice=7) ──
 * Osc 0 is the AMY ALGO control osc (algorithm + algo_source[0..5] wired to
 * relative oscs 1..6); oscs 1..6 are SINE operators. Intercepted before
 * amy_send_patch() exactly like the bass presets above.
 * FM_BASS/EPIANO/BELL/LEAD are fixed starter presets (see fm_presets.c).
 * FM_CUSTOM is the single live-editable voice driven by the FM UI screen
 * (see custompatches/fm_voice.h); its parameters are global, not per-layer —
 * every melodic row currently on this patch shares the same edited voice.
 * Numbered to start unconditionally after the wavetable range (267-271, only
 * ever compiled in under CONFIG_AMY_WAVETABLE) rather than reusing it, so FM
 * patch numbers never shift under a build-flag change and the two virtual
 * patch features can never collide. */
#define SEQ_PATCH_FM_BASE     272
#define SEQ_PATCH_FM_BASS     272   /* FM Bass (2-op chain, algorithm 0) */
#define SEQ_PATCH_FM_EPIANO   273   /* FM E.Piano (2 carriers, algorithm 0) */
#define SEQ_PATCH_FM_BELL     274   /* FM Bell (inharmonic ratios, algorithm 0) */
#define SEQ_PATCH_FM_LEAD     275   /* FM Lead (brighter 2-op chain, algorithm 0) */
#define SEQ_PATCH_FM_CUSTOM   276   /* Live-editable voice — opens the FM screen */
#define SEQ_PATCH_FM_MAX      276
#define SEQ_PATCH_ROUTABLE_MAX SEQ_PATCH_FM_MAX

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
void     sequencer_core_set_layer_patch(uint8_t layer_idx, uint16_t patch_number);
uint16_t sequencer_core_get_layer_patch(uint8_t layer_idx);

/* Re-push the live custom FM voice (custompatches/fm_voice.h's s_fm_voice) to
 * every melodic row currently on SEQ_PATCH_FM_CUSTOM. Called by the FM UI
 * screen after any encoder edit to algorithm/ratio/level/feedback. No-op if
 * no row is on that patch. */
void sequencer_core_fm_voice_changed(void);

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

/* ── Per-row second envelope (EG1, runtime-editable) ──
 * Independent AMY breakpoint generator, parallel to the EG0 accessors above.
 * Whether it is audible depends entirely on the loaded patch/preset: it does
 * nothing unless some coef (typically filter_freq_coefs) targets COEF_EG1 —
 * either baked into a patch string, or into one of our own custom presets
 * (see AMY-EDITS.md / bass_presets.c). Same deferred-authority model as EG0. */
bool sequencer_core_get_melodic_envelope2(uint8_t layer_idx, uint8_t track,
                                          seq_env_t *out);
void sequencer_core_set_melodic_envelope2(uint8_t layer_idx, uint8_t track,
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

/* Push a filter directly to an arbitrary AMY synth slot (shared by arp/drone).
 * is_ks: when true, also pushes f->resonance through sequencer_core_ks_feedback_from_q()
 * into the synth's KS feedback field (independent of f->enabled — KS feedback is
 * intrinsic to the oscillator, not the optional post-render filter). */
void sequencer_core_push_filter(uint8_t synth, const seq_filter_t *f, bool is_ks);

/* Map a Q value (same [0.51, 8.0] range enforced by sequencer_core_set_melodic_filter)
 * linearly onto AMY's KS oscillator feedback range [0.0, 1.0]. Q=8.0 -> feedback=1.0
 * is the verified-safe ceiling (lossless two-tap averaging filter, the classic
 * "infinite sustain" Karplus-Strong case); above 1.0 the KS buffer would diverge. */
float sequencer_core_ks_feedback_from_q(float q);

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

/* ── Per-step probability / ratchet / conditional trig ────────────────────
 * A step with prob==100 && ratchet==1 && cond==NONE is "plain" and costs
 * nothing extra (same always-on periodic AMY tag as before this feature).
 * Any other combination routes that one step through
 * sequencer_core_service_tick()'s one-shot per-loop scheduling instead — see
 * seq_core_trig.c for the engine. All four setters clamp their input and
 * immediately re-emit the step so the change is audible on its next
 * occurrence; all four getters return the "plain" default for an
 * out-of-range layer/track/step. */
void    sequencer_core_set_step_prob(uint8_t layer_idx, uint8_t track,
                                     uint8_t step, uint8_t prob_pct);
uint8_t sequencer_core_get_step_prob(uint8_t layer_idx, uint8_t track,
                                     uint8_t step);
void    sequencer_core_set_step_ratchet(uint8_t layer_idx, uint8_t track,
                                        uint8_t step, uint8_t count);
uint8_t sequencer_core_get_step_ratchet(uint8_t layer_idx, uint8_t track,
                                        uint8_t step);
void    sequencer_core_set_step_cond(uint8_t layer_idx, uint8_t track,
                                     uint8_t step, seq_step_cond_type_t type,
                                     uint8_t param);
void    sequencer_core_get_step_cond(uint8_t layer_idx, uint8_t track,
                                     uint8_t step, seq_step_cond_type_t *type,
                                     uint8_t *param);

/* Evaluate one AMY sequencer tick's worth of decorated-step bookkeeping:
 * detects step-boundary crossings per layer and, for any step that is
 * "decorated" (see above), rolls its conditional trig + probability and
 * one-shot schedules its ratchet sub-hits. Must be called once per AMY
 * sequencer tick — main.c wires this into amy_cfg.amy_external_sequencer_hook,
 * which already runs at that cadence (~2 kHz worst case, esp_timer task,
 * Core 0). No-op while paused. */
void sequencer_core_service_tick(void);

/* ── Generic envelope push ────────────────────────────────────────────────
 * Push an ADSR (EG0 breakpoint set) to an arbitrary AMY synth slot's voices.
 * Shared by the arp and the standalone drone so they reuse the exact same EG0
 * delta path as the melodic layers. The synth's osc0 must have its amp EG0 coef
 * enabled (patch-loaded synths do by default; the drone enables it explicitly).
 * Does nothing for an out-of-range eg_type. */
void sequencer_core_push_envelope(uint8_t synth, const seq_env_t *env);

/* Push env into the given synth/osc's EG1 breakpoint set (bp_is_set[1]).
 * `osc` lets a caller target a non-zero oscillator (e.g. a bass preset whose
 * filter lives on osc 1); melodic rows, the arp, and the drone all target
 * osc 0. Whichever coef is wired to COEF_EG1 (filter_freq_coefs in every
 * current use) is what actually moves — this call only supplies the timing. */
void sequencer_core_push_envelope_eg1(uint8_t synth, uint8_t osc, const seq_env_t *env);

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
void sequencer_core_arp_configure(uint16_t patch_number, uint8_t num_voices,
                                  bool filter_authored, float filter_q);

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

/* ── Per-track mute / solo ────────────────────────────────────────────────
 * Scoped per layer: solo only compares against the other tracks of the SAME
 * layer, not across layers. Standard mixing-desk semantics — if any track in
 * the layer is soloed, only soloed tracks are audible; solo overrides mute,
 * including on a track that is itself both muted and soloed. Gated in the
 * tick/emit path (sequencer_emit_step): an inaudible track's grid steps are
 * cancelled instead of scheduled, so it never produces a note-on. Both setters
 * hard-kill the affected synth slot's live voices so the change is heard
 * immediately, not just on the next scheduled note. */
void sequencer_core_set_track_mute(uint8_t layer_idx, uint8_t track, bool mute);
bool sequencer_core_get_track_mute(uint8_t layer_idx, uint8_t track);
void sequencer_core_set_track_solo(uint8_t layer_idx, uint8_t track, bool solo);
bool sequencer_core_get_track_solo(uint8_t layer_idx, uint8_t track);

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

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "seq_model.h"     /* seq_env_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ── Standalone arpeggiator ──────────────────────────────────────────────
 * Independent of the sequencer's layers: it owns up to ARP_MAX_SLOTS note
 * inputs, its own scale/root, its own AMY synth, and runs in sync with the
 * global tempo via repeating AMY SEQUENCE events.
 *
 * Slots store the RAW chromatic MIDI value. Special sentinels:
 *   -1 (empty)   : slot unused; not part of the playback cycle.
 *   ARP_REST (-2): deliberate silent step (SLOT mode only); occupies time but
 *                  plays nothing. Meaningless in UP/DOWN — store only -1 there.
 * The arp snaps to its own scale only at playback (and for display), so the
 * stored value never absorbs its own quantized output — no compounding drift. */

#define ARP_MAX_SLOTS 8
#define ARP_REST ((int16_t)-2)   /* deliberate silent step in ARP_SLOT mode */

typedef enum {
    ARP_UP   = 0,
    ARP_DOWN = 1,
    ARP_SLOT = 2,   /* slot-order: position preserved, ARP_REST honoured */
    ARP_DIR_COUNT
} arp_dir_t;

/* Sound source toggle — mirrors drone_source_t in drone_core.h.
 * ARP_SRC_PATCH: load a DX7/Juno patch (existing behaviour, default).
 * ARP_SRC_WAVE : configure a bare AMY oscillator with the user-chosen waveform;
 *                the arp sequence still drives pitch via normal note events. */
typedef enum {
    ARP_SRC_WAVE  = 0,   /* raw AMY waveform oscillator — no patch */
    ARP_SRC_PATCH = 1,   /* DX7/Juno patch (existing behaviour)    */
} arp_source_t;

/* RATE table: musical subdivision -> ticks per arp note.
 * AMY_SEQUENCER_PPQ = 48, so a 1/16 = 12 ticks (matches SEQ_TICKS_PER_STEP). */
typedef enum {
    ARP_RATE_1_1  = 0,
    ARP_RATE_1_4  = 1,   /* quarter note   */
    ARP_RATE_1_8  = 2,   /* eighth note    */
    ARP_RATE_1_16 = 3,  /* sixteenth note */
    ARP_RATE_1_32 = 4, /* thirty-second  */
    ARP_RATE_1_4T = 5,
    ARP_RATE_1_8T = 6,
    ARP_RATE_1_16T = 7,
    ARP_RATE_1_32T = 8,  
    ARP_RATE_COUNT
} arp_rate_t;

/* ── Lifecycle ── */
void arp_core_init(void);

/* Recompute and (re)emit the whole arp sequence immediately. Exposed so callers
 * can force a refresh (e.g. after tempo changes). Setters no longer call this
 * inline — they mark the arp dirty and arp_core_service() coalesces the re-emit. */
void arp_core_refresh(void);

/* Cancel all scheduled arp AMY events without re-emitting. Use on sequencer
 * pause to silence the arp without affecting the arp's enabled/configuration
 * state. Does NOT kill any voice currently mid-gate — follow with
 * sequencer_kill_synth_voices(SEQ_ARP_SYNTH) to silence any live note. */
void arp_core_clear_all(void);

/* Perform a pending (coalesced) re-emit if a setter marked the arp dirty since
 * the last call; cheap no-op otherwise. Call once per UI frame so a fast encoder
 * spin collapses into a single re-emit instead of one per detent. */
void arp_core_service(void);

/* ── Parameter setters (each re-emits the sequence) ── */
void arp_set_enabled(bool enabled);
void arp_set_direction(arp_dir_t dir);
void arp_set_octaves(uint8_t octaves);        /* clamped 1..ARP_OCT_MAX */
void arp_set_rate(arp_rate_t rate);
void arp_set_gate_pct(uint8_t gate_pct);      /* clamped 10..100        */
void arp_set_scale(uint8_t scale_index);
void arp_set_root_note(uint8_t root_note);
void arp_set_chord(uint8_t root_midi, uint8_t scale_index);
void arp_set_patch(uint16_t patch_number);
/* Live FM voice edits (FM screen): re-push the shared custom voice
 * (s_fm_voice) to the arp synth when the arp is currently playing it
 * (source == ARP_SRC_PATCH && patch == SEQ_PATCH_FM_CUSTOM); no-op
 * otherwise. Called from sequencer_core_fm_voice_changed() alongside the
 * melodic-layer fanout. */
void arp_core_fm_voice_changed(void);
/* Live additive voice edits: same contract as arp_core_fm_voice_changed()
 * but for s_additive_voice / SEQ_PATCH_ADDITIVE_CUSTOM. Called from
 * sequencer_core_additive_voice_changed(). */
void arp_core_additive_voice_changed(void);
/* Sound source: switch between a DX7/Juno patch and a raw AMY waveform.
 * On change, rebuilds the synth slot and re-applies any authored ADSR/filter.
 * In WAVE mode, arp_set_patch() stores the patch but does not reconfigure the
 * slot (takes effect on the next switch back to PATCH). */
void arp_set_source(arp_source_t src);
/* Select the AMY waveform used in WAVE mode (e.g. SAW_DOWN, SINE, PULSE …).
 * If WAVE mode is currently active the slot is reconfigured immediately. */
void arp_set_wave(uint16_t amy_wave);
/* Set slot value to a chromatic MIDI note, -1 to clear, or ARP_REST for a
 * deliberate silent step (meaningful in ARP_SLOT mode only). */
void arp_set_slot(uint8_t idx, int16_t chromatic_note);

/* ── Runtime-editable ADSR envelope (shared graph editor) ──
 * The arp owns one envelope applied to its synth's voices. get always returns
 * the stored env; set stores + pushes it to the arp synth. Re-applied after a
 * patch change so the custom envelope survives patch reconfig. */
void arp_get_envelope(seq_env_t *out);
void arp_set_envelope(const seq_env_t *env);

/* ── Second envelope (EG1, shared graph editor) ──
 * Independent breakpoint generator; audible only once something is wired to
 * COEF_EG1 — WAVE mode routes its filter_freq_coefs through it whenever the
 * arp's filter is authored+enabled, and a PATCH-mode instrument may already
 * route its own bp1 internally (many Juno/DX7 patches do). */
void arp_get_envelope2(seq_env_t *out);
void arp_set_envelope2(const seq_env_t *env);

/* ── Runtime-editable filter (shared filter editor) ──
 * Parallel to the envelope: default enabled=false (bypass). set stores + pushes
 * to the arp synth. Re-applied after a patch change when filter_authored. */
void arp_get_filter(seq_filter_t *out);
void arp_set_filter(const seq_filter_t *f);

/* ── Native AMY LFO (WAVE mode only) ──
 * Uses a voice-local osc 1 as the LFO carrier (mod_source=1 on osc 0).
 * PATCH mode: setter is a no-op on AMY (LFO state stored for persistence). */
void arp_get_lfo(seq_lfo_t *out);
void arp_set_lfo(const seq_lfo_t *lfo);

/* Recompute and push the LFO carrier frequency at the current BPM.
 * Called by sequencer_core_set_bpm() after s_bpm is updated.
 * No-op when source != ARP_SRC_WAVE or lfo_authored is false. */
void arp_core_refresh_lfo_freq(void);

/* ── Getters (for UI display) ── */
bool      arp_get_enabled(void);
arp_dir_t arp_get_direction(void);
uint8_t   arp_get_octaves(void);
arp_rate_t arp_get_rate(void);
const char *arp_rate_name(arp_rate_t rate);
uint8_t   arp_get_gate_pct(void);
uint8_t   arp_get_scale(void);
uint8_t   arp_get_root_note(void);
uint16_t     arp_get_patch(void);
arp_source_t arp_get_source(void);
uint16_t     arp_get_wave(void);
int16_t   arp_get_slot(uint8_t idx);          /* raw chromatic, -1=empty, ARP_REST=-2 */
/* Snapped pitch the slot will actually play (for display), or -1 if empty/rest. */
int16_t   arp_get_slot_snapped(uint8_t idx);
uint8_t   arp_active_slot_count(void);        /* notes up to first -1 (UP/DOWN) */
uint8_t   arp_active_step_count(void);        /* notes + rests across all slots (SLOT mode) */

/* ── Per-target amplitude trim (graph editor amp mode) ──
 * 0..1 multiplier on note velocity at emit time. Default 1.0 (unity).
 * set stores + marks dirty → coalesced re-emit on next arp_core_service(). */
void  arp_set_amp_scale(float v);
float arp_get_amp_scale(void);

/* ── Portamento / glide (AMY-native, PORTAMENTO_MS delta) ──
 * Milliseconds of exponential glide between consecutive note pitches, applied
 * by AMY internally (portamento_alpha low-pass on logfreq) — no scheduling or
 * render-path change needed here. 0 = off (default, matches AMY's own reset
 * value). Unlike the scheduling setters above, this does not mark the arp
 * dirty: it is pushed straight to the synth and survives independently of
 * note re-emits, but IS wiped by a patch/synth reconfigure (AMY resets
 * portamento_alpha to 0 on osc reset), so arp_rebuild() re-pushes it after
 * every source/patch/wave change. */
void     arp_set_portamento_ms(uint16_t ms);
uint16_t arp_get_portamento_ms(void);
#define ARP_PORTAMENTO_MAX_MS 2000u   /* generous glide ceiling for a lead line */

#define ARP_OCT_MAX 4

#ifdef __cplusplus
}
#endif

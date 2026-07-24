#pragma once

/* ── Chord presets ─────────────────────────────────────────────────────────
 * A small global table of user-authored chords (absolute MIDI pitches) that
 * melodic tracks can play instead of a single note. Assignment rides the
 * existing per-track source-note path: values SEQ_CHORD_BASE..+SLOTS-1 in
 * s_track_source_note / track_base_note / step_note[] are sentinels meaning
 * "play chord slot N", passing through sequencer_resolve_track_note()
 * untouched. Expansion to real pitches happens at emit time (the decorated
 * one-shot path in seq_core_trig.c), where the global chord progression's
 * transpose offset is applied per fire — intervals are preserved, chord tones
 * are never individually re-quantized.
 *
 * The sentinel range starts at 200 so 97..199 stays free for future sentinel
 * families and chord values read distinctly in dumps. step_note is uint8_t;
 * the playable range tops out at SEQ_MEL_NOTE_MAX (96). */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEQ_CHORD_SLOTS      8
#define SEQ_CHORD_MAX_NOTES  4      /* == Kconfig ceiling for SEQ_MEL_VOICES */
#define SEQ_CHORD_BASE       200u

#define SEQ_NOTE_IS_CHORD(n) ((uint8_t)(n) >= SEQ_CHORD_BASE && \
                              (uint8_t)(n) <  SEQ_CHORD_BASE + SEQ_CHORD_SLOTS)
#define SEQ_CHORD_INDEX(n)   ((uint8_t)((n) - SEQ_CHORD_BASE))
#define SEQ_CHORD_NOTE(idx)  ((uint8_t)(SEQ_CHORD_BASE + (idx)))

typedef struct {
    uint8_t notes[SEQ_CHORD_MAX_NOTES];  /* absolute MIDI, ascending; 0 = empty */
    uint8_t count;                       /* 0 = preset undefined */
} seq_chord_t;

/* Table accessors. set() normalizes (drops zero/empty positions, sorts
 * ascending, clamps to the melodic range) and then notifies the engine via
 * sequencer_core_chord_slot_changed() so tracks referencing the slot re-emit
 * and, when the tone count grew past the layer's voice budget, reconfigure
 * their synth's voice count — callers never do that bookkeeping themselves. */
bool    seq_chords_get(uint8_t idx, seq_chord_t *out);
void    seq_chords_set(uint8_t idx, const seq_chord_t *chord);
void    seq_chords_clear(uint8_t idx);
uint8_t seq_chords_count(uint8_t idx);
bool    seq_chords_is_defined(uint8_t idx);
uint8_t seq_chords_defined_count(void);
void    seq_chords_reset_all(void);

/* Bulk table replace for the project loader: normalizes and copies all
 * SEQ_CHORD_SLOTS entries (NULL = clear the table) WITHOUT the per-slot
 * engine sweep — the loader re-imports every layer right afterwards, which
 * rebuilds voices/emits from scratch anyway. */
void    seq_chords_import(const seq_chord_t table[SEQ_CHORD_SLOTS]);

/* Expand slot `idx` into absolute pitches with `transpose` semitones added,
 * each tone clamped (never dropped) to the melodic range. Returns the tone
 * count (0 = undefined slot: fire nothing). */
uint8_t seq_chords_resolve(uint8_t idx, int transpose,
                           uint8_t out[SEQ_CHORD_MAX_NOTES]);

/* One step through the melodic note-selector domain:
 * [SEQ_MEL_NOTE_MIN..SEQ_MEL_NOTE_MAX] ++ [defined chord sentinels], wrapping
 * at both ends (undefined slots are skipped; with no chords defined the plain
 * range simply wraps C1<->C7). `cur` may be a plain note or a sentinel. */
uint8_t seq_chords_selector_step(uint8_t cur, int dir);

#ifdef __cplusplus
}
#endif

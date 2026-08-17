#include "quantizer.h"
#include "seq_clamp.h"
#include <stddef.h>

static const musical_scale_t s_scales[] = {
    { .name = "Chromatic",        .intervals = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}, .size = 12 },
    { .name = "Major (Ionian)",   .intervals = {0, 2, 4, 5, 7, 9, 11},                 .size = 7  },
    { .name = "Natural Minor",    .intervals = {0, 2, 3, 5, 7, 8, 10},                 .size = 7  },
    { .name = "Dorian",           .intervals = {0, 2, 3, 5, 7, 9, 10},                 .size = 7  },
    { .name = "Phrygian",         .intervals = {0, 1, 3, 5, 7, 8, 10},                 .size = 7  },
    { .name = "Lydian",           .intervals = {0, 2, 4, 6, 7, 9, 11},                 .size = 7  },
    { .name = "Mixolydian",       .intervals = {0, 2, 4, 5, 7, 9, 10},                 .size = 7  },
    { .name = "Minor Pentatonic", .intervals = {0, 3, 5, 7, 10},                       .size = 5  },
    { .name = "Major Pentatonic", .intervals = {0, 2, 4, 7, 9},                        .size = 5  },
    /* Append-only past this point: scale_index is persisted in project
     * snapshots, so reordering the table would silently re-key saved scales. */
    { .name = "Harmonic Minor",   .intervals = {0, 2, 3, 5, 7, 8, 11},                 .size = 7  },
    { .name = "Locrian",          .intervals = {0, 1, 3, 5, 6, 8, 10},                 .size = 7  },
    { .name = "Whole Tone",       .intervals = {0, 2, 4, 6, 8, 10},                    .size = 6  }
};

/* Floored division (toward negative infinity) rather than C's truncation, so
 * negative note offsets map to the correct octave (-1/12 = -1, not 0). */
static int32_t quantizer_floor_div(int32_t numerator, int32_t denominator)
{
    int32_t quotient = numerator / denominator;
    int32_t remainder = numerator % denominator;
    /* Remainder with differing signs means C truncated up; correct it. */
    if (remainder != 0 && ((remainder < 0) != (denominator < 0))) {
        quotient -= 1;
    }
    return quotient;
}

uint8_t quantizer_scale_count(void)
{
    return (uint8_t)(sizeof(s_scales) / sizeof(s_scales[0]));
}

const musical_scale_t *quantizer_get_scale(uint8_t scale_index)
{
    if (scale_index >= quantizer_scale_count()) {
        return &s_scales[0];
    }
    return &s_scales[scale_index];
}

uint8_t quantizer_clamp_midi(int32_t midi_note)
{
    /* seq_clamp_u8 widens to int64_t before comparing, so a negative note
     * clamps to 0 rather than wrapping through uint8_t on the way in. */
    return SEQ_CLAMP_U8(midi_note, 0, 127);
}

/* Snap a MIDI note to the nearest note in the given scale/key: generate every
 * scale note across the input's octave plus its neighbours (octave-boundary
 * cases) and keep the closest. Ties break low, for determinism. */
uint8_t quantizer_snap_midi_note(uint8_t midi_note, uint8_t root_note,
                                 const musical_scale_t *scale)
{
    if (scale == NULL || scale->size == 0) {
        return midi_note;
    }

    int32_t best_note = (int32_t)midi_note;
    int32_t best_distance = 9999;
    int32_t relative_note = (int32_t)midi_note - (int32_t)root_note;
    int32_t base_octave = quantizer_floor_div(relative_note, 12);

    /* Neighbouring octaves too, so candidates just across an octave line are
     * still considered. */
    for (int32_t octave = base_octave - 1; octave <= base_octave + 1; octave++) {
        for (uint8_t i = 0; i < scale->size; i++) {
            int32_t candidate = (int32_t)root_note + (octave * 12) + scale->intervals[i];
            if (candidate < 0 || candidate > 127) {
                continue; /* out of MIDI range */
            }

            int32_t distance = candidate - (int32_t)midi_note;
            if (distance < 0) {
                distance = -distance; /* absolute distance in semitones */
            }

            if (distance < best_distance ||
                (distance == best_distance && candidate < best_note)) {
                best_distance = distance;
                best_note = candidate;
            }
        }
    }

    return (uint8_t)best_note;
}

/* Semitone intervals (from root) for each chord type.
 * Terminated by -1. Max 5 notes per chord. */
static const int8_t s_chord_intervals[CHORD_TYPE_COUNT][6] = {
    /* MAJ  */ {  0,  4,  7, -1, -1, -1 },
    /* MIN  */ {  0,  3,  7, -1, -1, -1 },
    /* MAJ7 */ {  0,  4,  7, 11, -1, -1 },
    /* MIN7 */ {  0,  3,  7, 10, -1, -1 },
    /* DOM7 */ {  0,  4,  7, 10, -1, -1 },
    /* SUS2 */ {  0,  2,  7, -1, -1, -1 },
    /* SUS4 */ {  0,  5,  7, -1, -1, -1 },
    /* DIM  */ {  0,  3,  6, -1, -1, -1 },
    /* AUG  */ {  0,  4,  8, -1, -1, -1 },
    /* MIN9 */ {  0,  3,  7, 10, 14, -1 },  // min7 + 9th
    /* MAJ9 */ {  0,  4,  7, 11, 14, -1 },  // maj7 + 9th
    /* OFF  */ {  0, -1, -1, -1, -1, -1 },  // root only - "no chord"
};

/* The -1-terminated semitone interval row for a chord type; NULL if out of
 * range. Shared so other modules (e.g. the drone) voice from one table. */
const int8_t *quantizer_chord_intervals(chord_type_t chord_type)
{
    if ((unsigned)chord_type >= CHORD_TYPE_COUNT) return NULL;
    return s_chord_intervals[chord_type];
}

uint8_t quantizer_snap_to_chord(uint8_t midi_note, uint8_t root,
                                chord_type_t chord_type)
{
    if ((unsigned)chord_type >= CHORD_TYPE_COUNT) return midi_note;
    const int8_t *intervals = s_chord_intervals[chord_type];
    uint8_t root_pc = root % 12;

    int32_t best_note     = (int32_t)midi_note;
    int32_t best_distance = 9999;

    /* Search chord tones across all reachable MIDI octaves. */
    for (int32_t oct = -1; oct <= 10; oct++) {
        for (int i = 0; i < 6; i++) {
            if (intervals[i] < 0) break;
            int32_t candidate = oct * 12 + (int32_t)root_pc + intervals[i];
            if (candidate < 0 || candidate > 127) continue;
            int32_t dist = candidate - (int32_t)midi_note;
            if (dist < 0) dist = -dist;
            if (dist < best_distance ||
                (dist == best_distance && candidate < best_note)) {
                best_distance = dist;
                best_note     = candidate;
            }
        }
    }
    return (uint8_t)best_note;
}

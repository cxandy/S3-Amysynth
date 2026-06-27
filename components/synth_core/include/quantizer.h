#pragma once

#include "chord_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    uint8_t intervals[12];
    uint8_t size;
} musical_scale_t;

typedef struct {
    uint8_t root_note;
    uint8_t scale_index;
    bool    enabled;
} quantizer_state_t;

uint8_t quantizer_scale_count(void);
const musical_scale_t *quantizer_get_scale(uint8_t scale_index);

uint8_t quantizer_clamp_midi(int32_t midi_note);
uint8_t quantizer_degree_to_midi(int32_t scale_degree, uint8_t root_note,
                                 const musical_scale_t *scale);
uint8_t quantizer_snap_midi_note(uint8_t midi_note, uint8_t root_note,
                                 const musical_scale_t *scale);

/* Snap midi_note to the nearest chord tone of (root 0-11, chord_type).
 * root is a chromatic pitch class; the function searches across all octaves. */
uint8_t quantizer_snap_to_chord(uint8_t midi_note, uint8_t root,
                                chord_type_t chord_type);

#ifdef __cplusplus
}
#endif
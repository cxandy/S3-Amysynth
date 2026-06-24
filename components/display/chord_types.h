#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CHORD_MAJ  = 0,
    CHORD_MIN,
    CHORD_MAJ7,
    CHORD_MIN7,
    CHORD_DOM7,
    CHORD_SUS2,
    CHORD_SUS4,
    CHORD_DIM,
    CHORD_AUG,
    CHORD_TYPE_COUNT
} chord_type_t;

static inline const char *chord_type_name(chord_type_t t)
{
    static const char *const s[] = {
        "Maj", "Min", "Maj7", "Min7", "Dom7", "Sus2", "Sus4", "Dim", "Aug"
    };
    return ((unsigned)t < CHORD_TYPE_COUNT) ? s[t] : "?";
}

/* Chromatic root note (0=C .. 11=B) to display name. */
static inline const char *chord_root_name(uint8_t root)
{
    static const char *const s[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    return s[root % 12];
}

#ifdef __cplusplus
}
#endif

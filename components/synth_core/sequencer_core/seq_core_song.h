#pragma once

/* ── Song-mode internal state ──────────────────────────────────────────────
 * Owned by seq_core_song.c, shared only with seq_core_internal.h (which pulls
 * this header in) and the public sequencer_core.h API. The scene-table shape
 * is the audio-core's persistable model; the UI reads/writes it through the
 * public accessors. */

#include "seq_core_config.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One song section: how many bars it lasts and which layers sound. */
typedef struct {
    uint8_t bars;        /* 1..255 bars, gate-guarded against 0 (see service) */
    uint8_t layer_mask;  /* bit n = layer n audible */
} song_scene_t;

typedef struct {
    song_scene_t scenes[SONG_MAX_SCENES];
    uint8_t      count;          /* 1..SONG_MAX_SCENES */
    uint8_t      current;        /* index of the scene sounding now */
    uint32_t     scene_start_bar;/* bars_elapsed when current began */
    bool         enabled;        /* song mode engages on play-start */
    bool         loop;           /* wrap to scene 0 after the last, else stop on the tail */
} song_state_t;

#ifdef __cplusplus
}
#endif
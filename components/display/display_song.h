#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Song screen renderer ────────────────────────────────────────────────
 * Layout (128×64):
 *   Line 0: "SONG  | ONE/LOOP  ON" — enabled toggle "ON/OFF", loop toggle
 *           "ONE"/"LOOP" (ONE = play through once and hold)
 *   Lines 1..N: scene rows — "1. 4b 1111" (index, bars, layer mask; active row
 *           marked with a triangle, selected row inverted)
 *   Status bar: bar-in-scene (left) + "+:B2  -:B1" hints (right)
 * Cursor positions: 0=enabled, 1=loop, 2..count+1=scene rows. While editing a
 * scene, the encoder cycles fields: 0=bars (from the preset set), then one
 * field per layer (1..4 = bit3..bit0) toggling that layer's audibility. */

#define SONG_VIEW_MAX_SCENES 16

typedef struct {
    uint8_t bars;
    uint8_t layer_mask;   /* bit n = layer n audible */
} song_scene_view_t;

typedef struct {
    bool               enabled;
    bool               loop;
    song_scene_view_t  scenes[SONG_VIEW_MAX_SCENES];
    uint8_t            count;
    uint8_t            current_scene; /* currently playing scene (highlighted) */
    uint8_t            cursor;        /* 0=enabled, 1=loop, 2..count+1=scenes */
    bool               editing;
    uint8_t            edit_field;    /* 0=bars, 1..4 = bit3..bit0 (while editing) */
    uint32_t           bars_in_current; /* bars elapsed within current scene   */
} song_view_t;

void display_song_draw_frame(u8g2_t *u8g2, const song_view_t *view);

#ifdef __cplusplus
}
#endif
#pragma once

#include "u8g2.h"
#include "chord_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Track Options screen renderer ────────────────────────────────────────
 * Per-track / per-layer secondary parameters.  Layer and track are
 * selectable directly on this screen (rows 0-1 rendered in the title bar):
 *   TRACK OPTS  [L2] T1    ← inverted when cursor==TO_ROW_LAYER
 *   TRACK OPTS  L2 [T1]    ← inverted when cursor==TO_ROW_TRACK
 *   ─────────────────────
 *   Repeat Rate : 2
 *   Chord Mode  : ON      (melodic layers only)
 *    Root       : A
 *    Type       : Min7
 * Cursor rows 2..5 are the content rows; rows 3..5 hidden for drum layers. */

typedef enum {
    TO_ROW_LAYER  = 0,   /* title-bar: Lx selector  */
    TO_ROW_TRACK  = 1,   /* title-bar: Tx selector  */
    TO_ROW_REPEAT = 2,
    TO_ROW_CHORD  = 3,
    TO_ROW_ROOT   = 4,
    TO_ROW_TYPE   = 5,
    TO_ROW_COUNT,
} trackopts_row_t;

typedef struct {
    uint8_t      layer_idx;    /* 0-based; rendered as 1-based */
    uint8_t      track_idx;    /* 0-based */
    uint8_t      layer_count;  /* total layers (for wrap) */
    uint8_t      track_count;  /* tracks per layer (for wrap) */
    bool         melodic;      /* chord rows only shown when true */
    uint8_t      repeat_rate;  /* 1/2/4/8 */
    bool         chord_mode;
    uint8_t      chord_root;   /* 0–11 */
    chord_type_t chord_type;
    bool         chord_locked; /* true = global progression drives chord (rows read-only) */
    uint8_t      cursor;       /* trackopts_row_t */
    bool         editing;
} trackopts_view_t;

void display_trackopts_draw_frame(u8g2_t *u8g2, const trackopts_view_t *view);

#ifdef __cplusplus
}
#endif

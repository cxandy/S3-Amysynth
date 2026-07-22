#pragma once

#include "u8g2.h"
#include "chord_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Progression screen renderer ──────────────────────────────────────────
 * Layout (128×64):
 *   Line 0: "PROG  INST|BAR  ON" (apply-mode label left of the ON/OFF toggle)
 *   Lines 1..N: entry rows — "1. C  Maj7  4b" (active row inverted)
 *   Status bar: current bar-in-entry / duration
 * Cursor positions: 0=enabled toggle, 1..count=entry rows,
 * count+1=apply-mode toggle (INST = chord edits land immediately, BAR = they
 * hold until the next bar line while playing) */

#define PROG_VIEW_MAX_ENTRIES 8

typedef struct {
    uint8_t      root;
    chord_type_t chord_type;
    uint8_t      duration_bars;
} prog_entry_view_t;

typedef struct {
    bool               enabled;
    bool               apply_at_bar;    /* launch quantize: chord edits wait for bar */
    prog_entry_view_t  entries[PROG_VIEW_MAX_ENTRIES];
    uint8_t            count;
    uint8_t            current_entry;   /* currently playing entry (highlighted) */
    uint8_t            cursor;          /* 0=toggle, 1..count=entries, count+1=apply */
    bool               editing;
    uint8_t            edit_field;       /* 0=root, 1=type, 2=duration (when editing) */
    uint32_t           bars_in_current; /* bars elapsed within current entry     */
} prog_view_t;

void display_prog_draw_frame(u8g2_t *u8g2, const prog_view_t *view);

#ifdef __cplusplus
}
#endif

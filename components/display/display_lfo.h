#pragma once
#include "u8g2.h"
#include "display_seq.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Editor cursor fields. The 5 target checkboxes come first with indices equal
 * to their lfo_target_t value, so `cursor < LFO_TARGET_COUNT` means "cursor is
 * on a target checkbox and its enum value is the cursor". The shared LFO
 * parameters follow. */
enum {
    LFO_FLD_WAVE = LFO_TARGET_COUNT,
    LFO_FLD_RATE,
    LFO_FLD_DEPTH,
    LFO_FLD_FLT_OCT,          /* FILTER swing in +/-octaves (quarter steps)   */
    LFO_FLD_WOB_RATE,         /* WOBBLE (second-order LFO) rate               */
    LFO_FLD_WOB_DEPTH,        /* WOBBLE amount in dB of swing, OFF = off      */
    LFO_FLD_WOB_MODE,         /* WOBBLE reach: depth only vs depth + rate     */
    LFO_FLD_EN,               /* drawn as a header chip, not a panel row      */
    LFO_FLD_COUNT,            /* total navigable fields */
};

/* Right-panel geometry. The parameter rows (WAVE..WOB_MODE) outnumber the slots
 * that fit above the hint strip, so the panel scrolls by one when the cursor
 * reaches the bottom row. A new parameter field only has to sit in this range. */
#define LFO_PANEL_SLOTS  5
#define LFO_PANEL_ROWS   (LFO_FLD_WOB_MODE - LFO_FLD_WAVE + 1)

/* View state for the LFO editor overlay: the working copy being edited plus
 * display metadata (cursor, editing flag, target track). */
typedef struct {
    seq_lfo_t   lfo;
    uint8_t     cursor;       /* 0..LFO_TARGET_COUNT-1 = target checkbox;
                                 then WAVE/RATE/DEPTH/EN (see LFO_FLD_*)   */
    bool        editing;      /* WAVE/RATE/DEPTH adjust mode (never for
                                 checkbox or EN toggle fields)             */
    uint8_t     layer_idx;
    uint8_t     track_idx;
    bool        apply_all;    /* true = commit applies to all tracks        */
    const char *target_label; /* "ARP"/"DRONE" label override, or NULL      */
    bool        wob_native;   /* target runs the native LFO carrier; false =
                                 software stepper, where the WOBBLE rows are
                                 inert and drawn struck-through             */
} lfo_view_t;

void lfo_view_draw(u8g2_t *u8g2, const lfo_view_t *v);

#ifdef __cplusplus
}
#endif

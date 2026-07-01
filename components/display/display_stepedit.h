#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Step Trig editor renderer ─────────────────────────────────────────────
 * Full-screen popup (same "takes over the display" convention as the ADSR /
 * filter / LFO editors) for one step's probability / ratchet / conditional
 * trig, addressed by the sequencer grid's existing cursor (active layer,
 * selected track, selected step) — no separate cursor of its own.
 *
 *   STEP L1 T2 S05
 *   ───────────────
 *  >Prob    : 75%
 *   Ratchet : 2
 *   Cond    : FILL
 *    Param  : 3
 *
 * field_cursor selects which row the encoder currently adjusts; short-press
 * cycles it, encoder turns adjust the value. The Param row is only shown
 * (and only reachable) when cond_type == SEQ_STEP_COND_FILL. */

typedef enum {
    SE_FIELD_PROB    = 0,
    SE_FIELD_RATCHET = 1,
    SE_FIELD_COND    = 2,
    SE_FIELD_PARAM   = 3,
    SE_FIELD_COUNT,
} stepedit_field_t;

typedef struct {
    uint8_t layer_idx;    /* 0-based; rendered as 1-based */
    uint8_t track_idx;    /* 0-based; rendered as 1-based */
    uint8_t step_idx;     /* 0-based; rendered as 1-based */
    uint8_t prob;         /* 0..100 */
    uint8_t ratchet;      /* 1..SEQ_MAX_RATCHET */
    uint8_t cond_type;    /* seq_step_cond_type_t */
    uint8_t cond_param;   /* FILL: 2..8 */
    uint8_t field_cursor; /* stepedit_field_t */
} stepedit_view_t;

void display_stepedit_draw_frame(u8g2_t *u8g2, const stepedit_view_t *view);

#ifdef __cplusplus
}
#endif

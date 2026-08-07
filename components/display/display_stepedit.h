#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Step Trig editor renderer ─────────────────────────────────────────────
 * Full-screen popup (same convention as the ADSR / filter / LFO editors) for
 * one step's probability / ratchet / conditional trig, addressed by the
 * sequencer grid's existing cursor - no separate cursor of its own.
 *
 *   STEP L1 T2 S05
 *   ───────────────
 *  >Prob    : 75%
 *   Ratchet : 2
 *   Every   : 2
 *   Prev    : OFF
 *
 * field_cursor selects which row the encoder currently adjusts; short-press
 * cycles it, encoder turns adjust the value. Every and Prev are independent
 * conditions (both must hold for the step to fire); Every 1 = every loop. */

typedef enum {
    SE_FIELD_PROB    = 0,
    SE_FIELD_RATCHET = 1,
    SE_FIELD_EVERY   = 2,
    SE_FIELD_PREV    = 3,
    SE_FIELD_COUNT,
} stepedit_field_t;

typedef struct {
    uint8_t layer_idx;    /* 0-based; rendered as 1-based */
    uint8_t track_idx;    /* 0-based; rendered as 1-based */
    uint8_t step_idx;     /* 0-based; rendered as 1-based */
    uint8_t prob;         /* 0..100 */
    uint8_t ratchet;      /* 1..SEQ_MAX_RATCHET */
    uint8_t every;        /* 1..SEQ_STEP_EVERY_MAX; 1 = every loop */
    uint8_t prev;         /* 0/1 */
    uint8_t field_cursor; /* stepedit_field_t */
} stepedit_view_t;

void display_stepedit_draw_frame(u8g2_t *u8g2, const stepedit_view_t *view);

#ifdef __cplusplus
}
#endif

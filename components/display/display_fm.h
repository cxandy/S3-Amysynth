#pragma once

#include "u8g2.h"
#include "custompatches/fm_graph.h"   /* fm_graph_view_t */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── FM operator graph screen ────────────────────────────────────────────
 * DX7-chart layout: carriers on the bottom row, each modulator stacked above
 * what it modulates, a self-loop on the feedback operator. Left 80 px hold
 * the graph, the right column the selected operator's parameter rows. The
 * caller (ui_screen_fm.c) formats every row string; this file only lays out
 * and draws. Operator index i is labelled OP(6-i). FB is the one voice-level
 * row among per-operator ones, so it is struck through unless the selected
 * operator carries the feedback loop. */

/* Cursor positions: the six operator boxes, then the panel rows. */
enum {
    FM_CUR_OP_BASE = 0,           /* + operator index 0..5 */
    FM_CUR_RATIO   = 6,
    FM_CUR_LEVEL,
    FM_CUR_TO,
    FM_CUR_FB,
    FM_CUR_ALGO,
    FM_CUR_COUNT,
};
#define FM_PANEL_ROWS  (FM_CUR_COUNT - FM_CUR_RATIO)
#define FM_ROW_LEN     12

typedef struct {
    fm_graph_view_t graph;
    uint8_t selected_op;              /* operator whose rows the panel shows */
    uint8_t cursor;                   /* FM_CUR_* */
    bool    editing;                  /* cursor row's value is being adjusted */
    bool    fb_applies;               /* selected op is the feedback op; else FB row struck */
    char    title[14];                /* "FM ALG 12" / "FM CUSTOM" */
    char    rows[FM_PANEL_ROWS][FM_ROW_LEN];
} fm_view_t;

void display_fm_draw_frame(u8g2_t *u8g2, const fm_view_t *view);

#ifdef __cplusplus
}
#endif

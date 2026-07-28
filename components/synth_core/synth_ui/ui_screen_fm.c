#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "custompatches/fm_voice.h"
#include "seq_clamp.h"
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════════
 *  FM/ALGO voice editor — algorithm + per-operator ratio/level
 * ════════════════════════════════════════════════════════════════════════
 * Edits the single live SEQ_PATCH_FM_CUSTOM voice (s_fm_voice, owned by
 * custompatches/fm_voice.c). Rendered by display_menu.c's generic
 * label/value list (display_menu_draw_frame_titled).
 *
 * Rows: ALGO, FEEDBACK, then (RATIO, LEVEL) for each of the 6 operators.
 * Operators are labelled by array index (OP 0..5) = the AMY algo_source[]
 * slot index; see fm_voice.h for the mapping onto a DX7 chart's OP1..OP6
 * (reversed). Per-algorithm carrier/modulator role labelling is deferred. */

typedef enum {
    FM_ROW_ALGO = 0,
    FM_ROW_FEEDBACK,
    FM_ROW_OP0_RATIO, FM_ROW_OP0_LEVEL,
    FM_ROW_OP1_RATIO, FM_ROW_OP1_LEVEL,
    FM_ROW_OP2_RATIO, FM_ROW_OP2_LEVEL,
    FM_ROW_OP3_RATIO, FM_ROW_OP3_LEVEL,
    FM_ROW_OP4_RATIO, FM_ROW_OP4_LEVEL,
    FM_ROW_OP5_RATIO, FM_ROW_OP5_LEVEL,
    FM_ROW_COUNT,
} fm_row_t;

#define FM_ALGO_COUNT 33   /* AMY algorithms[33] */

/* Curated DX7-style harmonic ratios plus a few inharmonic ones, kept short
 * enough to encoder through. Editing snaps to the nearest step, then walks. */
static const float s_fm_ratio_steps[] = {
    0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f,
    6.0f, 7.0f, 8.0f, 9.0f, 10.0f, 11.0f, 12.0f, 14.0f, 16.0f,
};
#define FM_RATIO_STEP_COUNT ((int)(sizeof(s_fm_ratio_steps) / sizeof(s_fm_ratio_steps[0])))

static uint8_t s_fm_cursor  = 0;
static bool    s_fm_editing = false;

static int fm_ratio_nearest_index(float ratio)
{
    int best = 0;
    float best_d = 1e9f;
    for (int i = 0; i < FM_RATIO_STEP_COUNT; i++) {
        float d = ratio - s_fm_ratio_steps[i];
        if (d < 0) d = -d;
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

static float fm_ratio_step(float current, int delta)
{
    int idx = fm_ratio_nearest_index(current) + delta;
    idx = SEQ_CLAMP_INT(idx, 0, FM_RATIO_STEP_COUNT - 1);
    return s_fm_ratio_steps[idx];
}

static uint8_t fm_row_op_index(fm_row_t row)
{
    return (uint8_t)((row - FM_ROW_OP0_RATIO) / 2);
}

static bool fm_row_is_ratio(fm_row_t row)
{
    return ((row - FM_ROW_OP0_RATIO) % 2) == 0;
}

bool synth_ui_fm_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_FM && !seq_state.menu_open;
}

void fm_build_view(menu_view_t *out)
{
    static menu_item_view_t items[FM_ROW_COUNT];

    snprintf(items[FM_ROW_ALGO].label, MENU_LABEL_LEN, "Algorithm");
    snprintf(items[FM_ROW_ALGO].value, MENU_VALUE_LEN, "%u", (unsigned)s_fm_voice.algorithm);

    snprintf(items[FM_ROW_FEEDBACK].label, MENU_LABEL_LEN, "Feedback");
    snprintf(items[FM_ROW_FEEDBACK].value, MENU_VALUE_LEN, "%.0f%%",
             (double)(s_fm_voice.feedback * 100.0f));

    for (uint8_t op = 0; op < FM_NUM_OPS; op++) {
        fm_row_t ratio_row = (fm_row_t)(FM_ROW_OP0_RATIO + op * 2);
        fm_row_t level_row = (fm_row_t)(ratio_row + 1);
        snprintf(items[ratio_row].label, MENU_LABEL_LEN, "OP%u Ratio", (unsigned)op);
        snprintf(items[ratio_row].value, MENU_VALUE_LEN, "%.2g", (double)s_fm_voice.op_ratio[op]);
        snprintf(items[level_row].label, MENU_LABEL_LEN, "OP%u Level", (unsigned)op);
        snprintf(items[level_row].value, MENU_VALUE_LEN, "%.0f%%",
                 (double)(s_fm_voice.op_level[op] * 100.0f));
    }

    out->items   = items;
    out->count   = FM_ROW_COUNT;
    out->cursor  = s_fm_cursor;
    out->editing = s_fm_editing;
}

uint32_t fm_view_signature(menu_view_t *out)
{
    uint32_t h = FNV1A_OFFSET;
    fm_build_view(out);
    h = fnv1a_bytes(h, &out->cursor, sizeof(out->cursor));
    h = fnv1a_bytes(h, &out->editing, sizeof(out->editing));
    for (uint8_t i = 0; i < out->count; i++) {
        h = fnv1a_bytes(h, out->items[i].label, sizeof(out->items[i].label));
        h = fnv1a_bytes(h, out->items[i].value, sizeof(out->items[i].value));
    }
    return h;
}

bool synth_ui_fm_handle_encoder(int delta)
{
    if (!synth_ui_fm_is_active()) return false;
    if (delta == 0) return true;

    if (s_fm_editing) {
        fm_row_t row = (fm_row_t)s_fm_cursor;
        switch (row) {
            case FM_ROW_ALGO: {
                int n = FM_ALGO_COUNT;
                int a = (int)s_fm_voice.algorithm + delta;
                a = ((a % n) + n) % n;
                s_fm_voice.algorithm = (uint8_t)a;
                break;
            }
            case FM_ROW_FEEDBACK: {
                float fb = s_fm_voice.feedback + (float)delta * 0.05f;
                s_fm_voice.feedback = SEQ_CLAMP_F32(fb, 0.0f, 1.2f);
                break;
            }
            default: {
                uint8_t op = fm_row_op_index(row);
                if (fm_row_is_ratio(row)) {
                    s_fm_voice.op_ratio[op] = fm_ratio_step(s_fm_voice.op_ratio[op], delta);
                } else {
                    float lvl = s_fm_voice.op_level[op] + (float)delta * 0.05f;
                    s_fm_voice.op_level[op] = SEQ_CLAMP_F32(lvl, 0.0f, 1.0f);
                }
                break;
            }
        }
        sequencer_core_fm_voice_changed();
    } else {
        int c = (int)s_fm_cursor + delta;
        c = SEQ_CLAMP_INT(c, 0, (int)FM_ROW_COUNT - 1);
        s_fm_cursor = (uint8_t)c;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_fm_handle_button(void)
{
    if (!synth_ui_fm_is_active()) return false;
    s_fm_editing = !s_fm_editing;
    s_force_redraw = true;
    return true;
}

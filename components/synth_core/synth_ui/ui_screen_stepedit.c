#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "display_stepedit.h"
#include "seq_clamp.h"

/* ════════════════════════════════════════════════════════════════════════
 *  Step Trig editor — per-step probability / ratchet / conditional trig
 * ════════════════════════════════════════════════════════════════════════
 * Reuses the sequencer grid's existing cursor (active_layer_idx /
 * selected_track / selected_step) instead of inventing a parallel one — the
 * user navigates to a step exactly as they do to toggle it on/off, then
 * opens this popup (MY_BUTTON_2 long-press, main.c) to decorate it. */

static bool    s_se_active = false;
static uint8_t s_se_field  = SE_FIELD_PROB;

/* PARAM only means anything (and is only shown) for a FILL conditional. */
static uint8_t se_max_field(uint8_t li, uint8_t t, uint8_t s)
{
    seq_step_cond_type_t type = SEQ_STEP_COND_NONE;
    uint8_t param = 0;
    sequencer_core_get_step_cond(li, t, s, &type, &param);
    return (type == SEQ_STEP_COND_FILL) ? (uint8_t)SE_FIELD_PARAM : (uint8_t)SE_FIELD_COND;
}

bool synth_ui_stepedit_is_active(void)
{
    return s_se_active;
}

void synth_ui_stepedit_open(void)
{
    s_se_active = true;
    s_se_field  = SE_FIELD_PROB;
    s_force_redraw = true;
}

void synth_ui_stepedit_close(void)
{
    s_se_active = false;
    s_force_redraw = true;
}

bool synth_ui_stepedit_handle_button(void)
{
    if (!s_se_active) return false;
    uint8_t li = seq_state.active_layer_idx;
    uint8_t t  = seq_state.selected_track;
    uint8_t s  = seq_state.selected_step;
    uint8_t max_field = se_max_field(li, t, s);
    s_se_field = (uint8_t)((s_se_field + 1) % (max_field + 1));
    s_force_redraw = true;
    return true;
}

bool synth_ui_stepedit_handle_encoder(long delta)
{
    if (!s_se_active) return false;
    uint8_t li = seq_state.active_layer_idx;
    uint8_t t  = seq_state.selected_track;
    uint8_t s  = seq_state.selected_step;
    int d = (int)delta;

    /* Cond type may have changed since the field cursor was last set (e.g.
     * an earlier edit left PARAM selected, then the user cycled COND away
     * from FILL) — clamp defensively before dispatching. */
    uint8_t max_field = se_max_field(li, t, s);
    if (s_se_field > max_field) s_se_field = max_field;

    switch (s_se_field) {
        case SE_FIELD_PROB: {
            int v = (int)sequencer_core_get_step_prob(li, t, s) + d * 5;
            sequencer_core_set_step_prob(li, t, s, SEQ_CLAMP_U8(v, 0, 100));
            break;
        }
        case SE_FIELD_RATCHET: {
            int v = (int)sequencer_core_get_step_ratchet(li, t, s) + d;
            sequencer_core_set_step_ratchet(li, t, s, SEQ_CLAMP_U8(v, 1, SEQ_MAX_RATCHET));
            break;
        }
        case SE_FIELD_COND: {
            seq_step_cond_type_t type = SEQ_STEP_COND_NONE;
            uint8_t param = 0;
            sequencer_core_get_step_cond(li, t, s, &type, &param);
            int v = (int)type + d;
            v %= (int)SEQ_STEP_COND_COUNT;
            if (v < 0) v += (int)SEQ_STEP_COND_COUNT;
            sequencer_core_set_step_cond(li, t, s, (seq_step_cond_type_t)v, param);
            break;
        }
        case SE_FIELD_PARAM: {
            seq_step_cond_type_t type = SEQ_STEP_COND_NONE;
            uint8_t param = 0;
            sequencer_core_get_step_cond(li, t, s, &type, &param);
            int v = (int)param + d;
            sequencer_core_set_step_cond(li, t, s, type, SEQ_CLAMP_U8(v, 2, 8));
            break;
        }
        default:
            break;
    }
    s_force_redraw = true;
    return true;
}

void stepedit_build_view(stepedit_view_t *out)
{
    uint8_t li = seq_state.active_layer_idx;
    uint8_t t  = seq_state.selected_track;
    uint8_t s  = seq_state.selected_step;
    seq_step_cond_type_t type = SEQ_STEP_COND_NONE;
    uint8_t param = 0;
    sequencer_core_get_step_cond(li, t, s, &type, &param);

    out->layer_idx    = li;
    out->track_idx    = t;
    out->step_idx     = s;
    out->prob         = sequencer_core_get_step_prob(li, t, s);
    out->ratchet      = sequencer_core_get_step_ratchet(li, t, s);
    out->cond_type    = (uint8_t)type;
    out->cond_param   = param;
    out->field_cursor = s_se_field;
}

uint32_t stepedit_view_signature(void)
{
    stepedit_view_t v;
    stepedit_build_view(&v);
    return fnv1a_bytes(FNV1A_OFFSET, &v, sizeof(v));
}

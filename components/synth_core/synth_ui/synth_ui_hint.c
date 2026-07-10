#include "synth_ui_hint.h"
#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include <stdio.h>

/* The button-hint strip reads its labels straight from the view descriptor
 * table, indexed by the single precedence resolver (synth_ui_active_view()).
 * This used to be three functions (hint_b1/b2/b3) that each re-walked the
 * overlay precedence ladder by hand; the labels now live one row per view in
 * ui_view_table[] (ui_view_resolve.c), so the hint strip can never disagree
 * with the draw switch about which view is active. A NULL static label means
 * the cell is dynamic — it depends on ui_mode, which the view id does not
 * carry (only the two scope-capable editors' b1 and the LFO editor's b2) — and
 * is filled by the row's b*_fn. */
static const char *hint_cell(const char *label, const char *(*fn)(void))
{
    return label ? label : fn();
}

const char *synth_ui_hint_text(void)
{
    const ui_view_desc_t *d = &ui_view_table[synth_ui_active_view()];
    static char buf[32];
    snprintf(buf, sizeof(buf), "1:%s 2:%s 3:%s",
             hint_cell(d->b1, d->b1_fn),
             hint_cell(d->b2, d->b2_fn),
             d->b3);  /* b3 is always a static label */
    return buf;
}

bool synth_ui_hint_visible(void)
{
    if (seq_state.ui_mode == UI_MODE_PROG) {
        return false;
    }
    if (seq_state.ui_mode == UI_MODE_DRONE && s_drone_vis_open) {
        return false;
    }
    return true;
}

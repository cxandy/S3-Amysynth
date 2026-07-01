#include "synth_ui_hint.h"
#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include <stdio.h>

/* MY_BUTTON_1's top guard in main_button_event_cb (main.c) always resolves,
 * for any event, in this fixed order: filter enable/disable toggle >
 * envelope apply-scope toggle (ADSR/LFO editors, melodic tracks only) > PROG
 * entry delete > default patch-hold latch. Mirrors that order exactly using
 * the same getters main.c calls -- see synth_ui_toggle_editor_apply_scope()'s
 * "no-op in ARP/DRONE modes" contract (ui_editors.c) for the ui_mode check. */
static const char *hint_b1(void)
{
    if (synth_ui_filter_is_active()) {
        return "On/Off";
    }
    bool editor_open = synth_ui_graph_is_active() || synth_ui_lfo_is_active();
    bool scope_capable = editor_open
                       && seq_state.ui_mode != UI_MODE_ARP
                       && seq_state.ui_mode != UI_MODE_DRONE;
    if (scope_capable) {
        return "Scope";
    }
    if (synth_ui_prog_is_active()) {
        return "Del";
    }
    return "Patch";
}

/* MY_BUTTON_2: the per-screen isolation guards (arp/prog/trackopts/drone) are
 * checked before the generic filter-suppress / graph-amp-mode / pitch-hold
 * fallback in main.c, in that exact order -- reproduced here unchanged. */
static const char *hint_b2(void)
{
    if (synth_ui_arp_is_active())       return "-";
    if (synth_ui_prog_is_active())      return "+Add";
    if (synth_ui_trackopts_is_active()) return "DelLy";
    if (synth_ui_drone_is_active())     return "-";
    if (synth_ui_filter_is_active())    return "-";
    if (synth_ui_stepedit_is_active())  return "Close";
    if (synth_ui_graph_is_active())     return "Amp";
    return "Pitch";
}

/* MY_BUTTON_3: editor-cycle vs. menu-toggle. While the ADSR graph editor
 * specifically is open, long-press also toggles its EG0/EG1 breakpoint set
 * (main.c) — collapsed into "Next" below like every other button's long-press
 * nuance, since this hint shows one dominant label per button, not every
 * gesture length. */
static const char *hint_b3(void)
{
    if (synth_ui_graph_is_active() || synth_ui_filter_is_active()
                                    || synth_ui_lfo_is_active()) {
        return "Next";
    }
    return "Menu";
}

const char *synth_ui_hint_text(void)
{
    static char buf[32];
    snprintf(buf, sizeof(buf), "1:%s 2:%s 3:%s",
             hint_b1(), hint_b2(), hint_b3());
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

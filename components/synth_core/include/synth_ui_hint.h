#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── On-screen button hint bar ────────────────────────────────────────────
 * MY_BUTTON_1/2/3 are heavily overloaded across screens and overlays (see
 * main.c's main_button_event_cb priority chain). This module NEVER dispatches
 * input: it only re-queries the same synth_ui_*_is_active() getters main.c
 * consults, in the same priority order, to describe each button. */

/* Static single-line buffer owned by this module, valid until the next call,
 * e.g. "1:Patch 2:Pitch 3:Menu". Cheap enough to call every frame. */
const char *synth_ui_hint_text(void);

/* False for views whose bottom rows are load-bearing content (the drone
 * gate-pattern visualiser) or that already render an equivalent button
 * legend of their own (the chord-progression screen). */
bool synth_ui_hint_visible(void);

#ifdef __cplusplus
}
#endif

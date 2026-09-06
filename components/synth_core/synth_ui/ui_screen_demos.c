#include "sdkconfig.h"
#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"              /* synth_ui_demo_count / _demo_label */
#include <stdio.h>
#include <string.h>

/* ════════════════════════════════════════════════════════════════════════
 *  DEMOS PAGE (Main Menu -> "DEMOS")
 * ════════════════════════════════════════════════════════════════════════
 * Item model for the built-in demo picker; page state and input routing live
 * in ui_screen_menu.c (same split as the FX/Chords/Projects/Wireless pages).
 *
 * One row per demo in the registry (synth_ui_demo_count / _demo_label) plus a
 * Back row. Every demo row is a one-shot action: it queues the load via
 * synth_ui_request_demo and ui_screen_menu.c closes the overlay so the sound
 * is heard immediately (same as the old per-demo rows on the main list).
 *
 * Adding a demo to the registry is all that is needed to get a row here - this
 * file never changes for that, and the registry's compile-time slot budget
 * lives in synth_ui.h (SYNTH_UI_DEMOS_MAX). */

#define DEMOS_ITEMS_MAX (SYNTH_UI_DEMOS_MAX + 1)   /* demos + Back row */

static menu_item_view_t s_items[DEMOS_ITEMS_MAX];

const menu_item_view_t *demos_menu_build_items(void)
{
    uint8_t n = synth_ui_demo_count();
    if (n > DEMOS_ITEMS_MAX) n = DEMOS_ITEMS_MAX;
    for (uint8_t i = 0; i < n; i++) {
        snprintf(s_items[i].label, MENU_LABEL_LEN, "%s", synth_ui_demo_label(i));
        snprintf(s_items[i].value, MENU_VALUE_LEN, ">");
    }
    /* Back row: only reachable when the registry has not filled the budget. */
    if (n < DEMOS_ITEMS_MAX) {
        snprintf(s_items[n].label, MENU_LABEL_LEN, "< Back");
        s_items[n].value[0] = '\0';
    }
    return s_items;
}

uint8_t demos_menu_item_count(void)
{
    uint8_t n = synth_ui_demo_count();
    return (n < DEMOS_ITEMS_MAX) ? (uint8_t)(n + 1u) : n;
}
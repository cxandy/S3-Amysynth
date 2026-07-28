#include "synth_ui/synth_ui_internal.h"
#include "sequencer_core.h"
#include "seq_core_config.h"   /* SEQ_MELODIC_PORTAMENTO_MAX_MS */
#include "seq_clamp.h"
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════════
 *  NOTE FX SUBMENU PAGE
 * ════════════════════════════════════════════════════════════════════════
 * Per-layer melodic note controls, reached from a dive row on the global-FX
 * page (page state and input routing live in ui_screen_menu.c; this file only
 * builds the rows and applies encoder edits). Two controls, both acting on the
 * currently ACTIVE melodic layer (seq_state.active_layer_idx):
 *   GATE   - note-hold as % of the step (10..100; 100 = full-step legato)
 *   GLIDE  - AMY-native portamento between step pitches (0..cap ms)
 *   GROOVE - how much of the baked accent/humanize velocity curve applies
 *            (0..100%; 100 = full feel, 0 = flat velocity 1.0)
 * Per-layer, not global, hence off the global-FX list.
 *
 * Non-melodic active layer: rows render "--" and edits no-op, so the page is
 * always safe to open. */

typedef enum {
    NFX_GATE = 0,
    NFX_GLIDE,
    NFX_GROOVE,
    NFX_BACK,
    NFX_COUNT
} notefx_item_id_t;

static menu_item_view_t s_nfx_items[NFX_COUNT];

/* Active layer index if it is melodic, else 0xFF (nothing to edit). */
static uint8_t notefx_active_melodic_layer(void)
{
    uint8_t li = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return 0xFF;
    if (seq_state.layers[li].type != SEQ_LAYER_MELODIC) return 0xFF;
    return li;
}

uint8_t notefx_menu_item_count(void)
{
    return NFX_COUNT;
}

const menu_item_view_t *notefx_menu_build_items(void)
{
    for (int i = 0; i < NFX_COUNT; i++) {
        s_nfx_items[i].value[0] = '\0';
    }

    uint8_t li = notefx_active_melodic_layer();

    snprintf(s_nfx_items[NFX_GATE].label,   MENU_LABEL_LEN, "Gate");
    snprintf(s_nfx_items[NFX_GLIDE].label,  MENU_LABEL_LEN, "Glide");
    snprintf(s_nfx_items[NFX_GROOVE].label, MENU_LABEL_LEN, "Groove");
    if (li == 0xFF) {
        snprintf(s_nfx_items[NFX_GATE].value,   MENU_VALUE_LEN, "--");
        snprintf(s_nfx_items[NFX_GLIDE].value,  MENU_VALUE_LEN, "--");
        snprintf(s_nfx_items[NFX_GROOVE].value, MENU_VALUE_LEN, "--");
    } else {
        snprintf(s_nfx_items[NFX_GATE].value,   MENU_VALUE_LEN, "%u%%",
                 (unsigned)sequencer_core_get_melodic_gate_pct(li));
        snprintf(s_nfx_items[NFX_GLIDE].value,  MENU_VALUE_LEN, "%ums",
                 (unsigned)sequencer_core_get_melodic_portamento_ms(li));
        snprintf(s_nfx_items[NFX_GROOVE].value, MENU_VALUE_LEN, "%u%%",
                 (unsigned)sequencer_core_get_melodic_groove_pct(li));
    }

    snprintf(s_nfx_items[NFX_BACK].label, MENU_LABEL_LEN, "< Back");

    return s_nfx_items;
}

bool notefx_menu_item_is_value(uint8_t idx)
{
    return idx < NFX_BACK;   /* Gate/Glide/Groove are editable; Back is not */
}

bool notefx_menu_item_is_back(uint8_t idx)
{
    return idx == NFX_BACK;
}

void notefx_menu_edit_value(uint8_t idx, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    if (dir == 0) return;

    uint8_t li = notefx_active_melodic_layer();
    if (li == 0xFF) return;   /* non-melodic layer: nothing to edit */

    switch ((notefx_item_id_t)idx) {
        case NFX_GATE: {
            /* 5%/detent, mirroring the arp GATE control. */
            int v = SEQ_CLAMP_INT(
                (int)sequencer_core_get_melodic_gate_pct(li) + dir * 5, 10, 100);
            sequencer_core_set_melodic_gate_pct(li, (uint8_t)v);
            break;
        }
        case NFX_GLIDE: {
            /* 1ms/detent, matching the arp glide resolution. */
            int v = SEQ_CLAMP_INT(
                (int)sequencer_core_get_melodic_portamento_ms(li) + dir * 1,
                0, (int)SEQ_MELODIC_PORTAMENTO_MAX_MS);
            sequencer_core_set_melodic_portamento_ms(li, (uint16_t)v);
            break;
        }
        case NFX_GROOVE: {
            /* 5%/detent, matching GATE. */
            int v = SEQ_CLAMP_INT(
                (int)sequencer_core_get_melodic_groove_pct(li) + dir * 5, 0, 100);
            sequencer_core_set_melodic_groove_pct(li, (uint8_t)v);
            break;
        }
        default:
            break;
    }
}

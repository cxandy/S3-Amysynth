#include "synth_ui/synth_ui_internal.h"
#include "amy_fx.h"
#include "seq_clamp.h"
#include <stdio.h>

/* ════════════════════════════════════════════════════════════════════════
 *  GLOBAL FX SUBMENU PAGE
 * ════════════════════════════════════════════════════════════════════════
 * Item model for the FX page of the menu overlay; page state and input
 * routing live in ui_screen_menu.c. Covers the three-band EQ, echo/chorus/
 * reverb levels and their extended params (fx_state_t).
 *
 * Extended params use the FX_PARAM_UNSET sentinel: while unset, fx_push_*
 * never writes the matching amy_event field, so AMY keeps its factory value.
 * Rows therefore display the AMY default while unset, and the first encoder
 * step seeds the field from that default - shown and audible never disagree. */

typedef enum {
    FXI_EQ_LOW = 0,
    FXI_EQ_MID,
    FXI_EQ_HIGH,
    FXI_ECHO_LEVEL,
    FXI_ECHO_FEEDBACK,
    FXI_ECHO_TIME,
    FXI_ECHO_TONE,
    FXI_CHORUS_LEVEL,
    FXI_CHORUS_RATE,
    FXI_CHORUS_DEPTH,
    FXI_REVERB_LEVEL,
    FXI_REVERB_LIVENESS,
    FXI_REVERB_DAMPING,
    FXI_REVERB_XOVER,
    FXI_PRESET_GLOBAL_FX,
    FXI_NOTEFX,     /* dive row: opens the per-layer NoteFX (gate/glide) page */
    FXI_BACK,
    FXI_COUNT
} fx_menu_item_id_t;

static menu_item_view_t s_fx_items[FXI_COUNT];

/* Effective value of a sentinel-gated param: while unset, the AMY factory
 * default (what the bus is actually running); the stored value once set. */
static int fx_eff(int16_t v, int def)
{
    return (v == FX_PARAM_UNSET) ? def : (int)v;
}

uint8_t fx_menu_item_count(void)
{
    return FXI_COUNT;
}

const menu_item_view_t *fx_menu_build_items(void)
{
    for (int i = 0; i < FXI_COUNT; i++) {
        s_fx_items[i].value[0] = '\0';
    }

    snprintf(s_fx_items[FXI_EQ_LOW].label, MENU_LABEL_LEN, "EQ Low");
    snprintf(s_fx_items[FXI_EQ_LOW].value, MENU_VALUE_LEN, "%+ddB",
             (int)s_fx.eq_low_db);
    snprintf(s_fx_items[FXI_EQ_MID].label, MENU_LABEL_LEN, "EQ Mid");
    snprintf(s_fx_items[FXI_EQ_MID].value, MENU_VALUE_LEN, "%+ddB",
             (int)s_fx.eq_mid_db);
    snprintf(s_fx_items[FXI_EQ_HIGH].label, MENU_LABEL_LEN, "EQ High");
    snprintf(s_fx_items[FXI_EQ_HIGH].value, MENU_VALUE_LEN, "%+ddB",
             (int)s_fx.eq_high_db);

    snprintf(s_fx_items[FXI_ECHO_LEVEL].label, MENU_LABEL_LEN, "Echo");
    snprintf(s_fx_items[FXI_ECHO_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)s_fx.echo_level);
    snprintf(s_fx_items[FXI_ECHO_FEEDBACK].label, MENU_LABEL_LEN, "Echo Fbk");
    snprintf(s_fx_items[FXI_ECHO_FEEDBACK].value, MENU_VALUE_LEN, "%d%%",
             fx_eff(s_fx.echo_feedback, 0));
    snprintf(s_fx_items[FXI_ECHO_TIME].label, MENU_LABEL_LEN, "Echo Time");
    snprintf(s_fx_items[FXI_ECHO_TIME].value, MENU_VALUE_LEN, "%dms",
             fx_eff(s_fx.echo_delay_ms, 500));
    snprintf(s_fx_items[FXI_ECHO_TONE].label, MENU_LABEL_LEN, "Echo Tone");
    snprintf(s_fx_items[FXI_ECHO_TONE].value, MENU_VALUE_LEN, "%+d",
             fx_eff(s_fx.echo_tone, 0));

    snprintf(s_fx_items[FXI_CHORUS_LEVEL].label, MENU_LABEL_LEN, "Chorus");
    snprintf(s_fx_items[FXI_CHORUS_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)s_fx.chorus_level);
    {
        int r = fx_eff(s_fx.chorus_rate, 50);   /* centi-Hz */
        snprintf(s_fx_items[FXI_CHORUS_RATE].label, MENU_LABEL_LEN, "Cho Rate");
        snprintf(s_fx_items[FXI_CHORUS_RATE].value, MENU_VALUE_LEN, "%d.%02dHz",
                 r / 100, r % 100);
    }
    snprintf(s_fx_items[FXI_CHORUS_DEPTH].label, MENU_LABEL_LEN, "Cho Depth");
    snprintf(s_fx_items[FXI_CHORUS_DEPTH].value, MENU_VALUE_LEN, "%d%%",
             fx_eff(s_fx.chorus_depth, 50));

    snprintf(s_fx_items[FXI_REVERB_LEVEL].label, MENU_LABEL_LEN, "Reverb");
    snprintf(s_fx_items[FXI_REVERB_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)s_fx.reverb_level);
    snprintf(s_fx_items[FXI_REVERB_LIVENESS].label, MENU_LABEL_LEN, "Rev Live");
    snprintf(s_fx_items[FXI_REVERB_LIVENESS].value, MENU_VALUE_LEN, "%d%%",
             fx_eff(s_fx.reverb_liveness, 85));
    snprintf(s_fx_items[FXI_REVERB_DAMPING].label, MENU_LABEL_LEN, "Rev Damp");
    snprintf(s_fx_items[FXI_REVERB_DAMPING].value, MENU_VALUE_LEN, "%d%%",
             fx_eff(s_fx.reverb_damping, 50));
    snprintf(s_fx_items[FXI_REVERB_XOVER].label, MENU_LABEL_LEN, "Rev Xover");
    snprintf(s_fx_items[FXI_REVERB_XOVER].value, MENU_VALUE_LEN, "%dHz",
             fx_eff(s_fx.reverb_xover_hz, 3000));

    /* "Presets alter global FX? y/n" — OFF makes Juno presets per-synth. */
    snprintf(s_fx_items[FXI_PRESET_GLOBAL_FX].label, MENU_LABEL_LEN, "Preset FX");
    snprintf(s_fx_items[FXI_PRESET_GLOBAL_FX].value, MENU_VALUE_LEN, "%s",
             s_fx.presets_alter_global ? "ON" : "OFF");

    /* Per-layer note controls (gate/glide) live on their own page:
     * ui_screen_notefx.c. */
    snprintf(s_fx_items[FXI_NOTEFX].label, MENU_LABEL_LEN, "NoteFX");
    snprintf(s_fx_items[FXI_NOTEFX].value, MENU_VALUE_LEN, ">");

    snprintf(s_fx_items[FXI_BACK].label, MENU_LABEL_LEN, "< Back");

    return s_fx_items;
}

bool fx_menu_item_is_value(uint8_t idx)
{
    /* Every row except the NoteFX dive and Back holds an editable value. */
    return idx < FXI_BACK && idx != FXI_NOTEFX;
}

bool fx_menu_item_is_back(uint8_t idx)
{
    return idx == FXI_BACK;
}

bool fx_menu_item_is_notefx(uint8_t idx)
{
    return idx == FXI_NOTEFX;
}

/* Step a sentinel-gated param: the first edit seeds the field from the AMY
 * default it was displaying, then steps and pushes normally. */
static void fx_step(int16_t *field, int def, int step, int lo, int hi,
                    int dir, void (*push)(void))
{
    int cur = (*field == FX_PARAM_UNSET) ? def : (int)*field;
    *field = (int16_t)SEQ_CLAMP_INT(cur + dir * step, lo, hi);
    push();
}

void fx_menu_edit_value(uint8_t idx, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    if (dir == 0) return;

    switch ((fx_menu_item_id_t)idx) {
        case FXI_EQ_LOW: {
            int v = SEQ_CLAMP_INT((int)s_fx.eq_low_db + dir, -15, 15);
            s_fx.eq_low_db = (int8_t)v; fx_push_eq();
            break;
        }
        case FXI_EQ_MID: {
            int v = SEQ_CLAMP_INT((int)s_fx.eq_mid_db + dir, -15, 15);
            s_fx.eq_mid_db = (int8_t)v; fx_push_eq();
            break;
        }
        case FXI_EQ_HIGH: {
            int v = SEQ_CLAMP_INT((int)s_fx.eq_high_db + dir, -15, 15);
            s_fx.eq_high_db = (int8_t)v; fx_push_eq();
            break;
        }
        case FXI_ECHO_LEVEL: {
            int v = SEQ_CLAMP_INT((int)s_fx.echo_level + dir * 5, 0, 100);
            s_fx.echo_level = (uint8_t)v; fx_push_echo();
            break;
        }
        case FXI_ECHO_FEEDBACK:
            fx_step(&s_fx.echo_feedback, 0, 5, 0, 99, dir, fx_push_echo);
            break;
        case FXI_ECHO_TIME:
            /* 743 ms = AMY's default-allocated echo delay line; longer values
             * get clamped inside config_echo anyway. */
            fx_step(&s_fx.echo_delay_ms, 500, 10, 0, 743, dir, fx_push_echo);
            break;
        case FXI_ECHO_TONE:
            fx_step(&s_fx.echo_tone, 0, 5, -99, 99, dir, fx_push_echo);
            break;
        case FXI_CHORUS_LEVEL: {
            int v = SEQ_CLAMP_INT((int)s_fx.chorus_level + dir * 5, 0, 100);
            s_fx.chorus_level = (uint8_t)v; fx_push_chorus();
            break;
        }
        case FXI_CHORUS_RATE:
            /* centi-Hz: 5..1000 = 0.05..10 Hz in 0.05 Hz steps. */
            fx_step(&s_fx.chorus_rate, 50, 5, 5, 1000, dir, fx_push_chorus);
            break;
        case FXI_CHORUS_DEPTH:
            fx_step(&s_fx.chorus_depth, 50, 5, 0, 100, dir, fx_push_chorus);
            break;
        case FXI_REVERB_LEVEL: {
            int v = SEQ_CLAMP_INT((int)s_fx.reverb_level + dir * 5, 0, 100);
            s_fx.reverb_level = (uint8_t)v; fx_push_reverb();
            break;
        }
        case FXI_REVERB_LIVENESS:
            fx_step(&s_fx.reverb_liveness, 85, 5, 0, 100, dir, fx_push_reverb);
            break;
        case FXI_REVERB_DAMPING:
            fx_step(&s_fx.reverb_damping, 50, 5, 0, 100, dir, fx_push_reverb);
            break;
        case FXI_REVERB_XOVER:
            fx_step(&s_fx.reverb_xover_hz, 3000, 250, 500, 8000, dir, fx_push_reverb);
            break;
        case FXI_PRESET_GLOBAL_FX:
            s_fx.presets_alter_global = !s_fx.presets_alter_global;
            /* Re-arming the guard re-imposes the cached FX at once, undoing
             * whatever the last preset left behind. */
            if (!s_fx.presets_alter_global) synth_ui_fx_reassert_global();
            break;
        default:
            break;
    }
}

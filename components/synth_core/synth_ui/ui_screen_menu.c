#include "sdkconfig.h"
#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/sample_rec.h"
#include "quantizer.h"
#include "amy_fx.h"
#include "seq_clamp.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "synth_ui";

/* ════════════════════════════════════════════════════════════════════════
 *  MENU OVERLAY
 * ════════════════════════════════════════════════════════════════════════
 * A small modal list. Items are either ACTIONS (run on click, no value) or
 * VALUE items (click to enter editing, encoder changes the value, click to
 * exit). The model is a static table; values are read/written live from the
 * sequencer_core quantizer + arp_core + global FX cache. */

typedef enum {
    MI_SCREEN_SEQ = 0,
    MI_SCREEN_ARP,
    MI_SCREEN_DRONE,
    MI_SCREEN_PROG,
    MI_SCREEN_TRACKOPTS,
#if CONFIG_SYNTH_CUSTOM_FM
    MI_SCREEN_FM,
#endif
    MI_BPM,
    MI_QUANT_ENABLED,
    MI_QUANT_SCALE,
    MI_QUANT_ROOT,
    MI_ARP_ENABLED,
    MI_DRONE_ENABLED,
    MI_DRUM_ENGINE,
    MI_ADD_LAYER,
    MI_REMOVE_LAYER,
    MI_EQ_LOW,
    MI_EQ_MID,
    MI_EQ_HIGH,
    MI_ECHO_LEVEL,
    MI_CHORUS_LEVEL,
    MI_REVERB_LEVEL,
    MI_PRESET_GLOBAL_FX,
    MI_VOLUME,
    MI_SAMPLE,
    MI_SAMPLE_CANCEL,
    MI_COUNT
} menu_item_id_t;

static menu_item_view_t s_menu_items[MI_COUNT];

/* Format the current value of each menu item into the flat view array. */
void menu_build_view(menu_view_t *out)
{
    for (uint8_t i = 0; i < MI_COUNT; i++) {
        s_menu_items[i].value[0] = '\0';
    }

    snprintf(s_menu_items[MI_SCREEN_SEQ].label, MENU_LABEL_LEN, "Screen: Seq");
    snprintf(s_menu_items[MI_SCREEN_ARP].label, MENU_LABEL_LEN, "Screen: Arp");
    snprintf(s_menu_items[MI_SCREEN_DRONE].label, MENU_LABEL_LEN, "Screen: Drone");
    snprintf(s_menu_items[MI_SCREEN_PROG].label, MENU_LABEL_LEN, "Screen: Prog");
    snprintf(s_menu_items[MI_SCREEN_TRACKOPTS].label, MENU_LABEL_LEN, "Screen: TrackOpts");
#if CONFIG_SYNTH_CUSTOM_FM
    snprintf(s_menu_items[MI_SCREEN_FM].label, MENU_LABEL_LEN, "Screen: FM");
#endif

    snprintf(s_menu_items[MI_BPM].label, MENU_LABEL_LEN, "BPM");
    snprintf(s_menu_items[MI_BPM].value, MENU_VALUE_LEN, "%u",
             (unsigned)seq_get_bpm());

    snprintf(s_menu_items[MI_QUANT_ENABLED].label, MENU_LABEL_LEN, "Quant");
    snprintf(s_menu_items[MI_QUANT_ENABLED].value, MENU_VALUE_LEN, "%s",
             sequencer_core_get_quantizer_enabled() ? "ON" : "OFF");

    snprintf(s_menu_items[MI_QUANT_SCALE].label, MENU_LABEL_LEN, "Scale");
    {
        const musical_scale_t *sc =
            quantizer_get_scale(sequencer_core_get_quantizer_scale());
        snprintf(s_menu_items[MI_QUANT_SCALE].value, MENU_VALUE_LEN, "%s",
                 sc ? sc->name : "?");
    }

    snprintf(s_menu_items[MI_QUANT_ROOT].label, MENU_LABEL_LEN, "Root");
    {
        char nn[4];
        ui_note_name(sequencer_core_get_quantizer_root_note(), nn);
        snprintf(s_menu_items[MI_QUANT_ROOT].value, MENU_VALUE_LEN, "%s", nn);
    }

    snprintf(s_menu_items[MI_ARP_ENABLED].label, MENU_LABEL_LEN, "Arp");
    snprintf(s_menu_items[MI_ARP_ENABLED].value, MENU_VALUE_LEN, "%s",
             arp_get_enabled() ? "ON" : "OFF");

    snprintf(s_menu_items[MI_DRONE_ENABLED].label, MENU_LABEL_LEN, "Drone");
    snprintf(s_menu_items[MI_DRONE_ENABLED].value, MENU_VALUE_LEN, "%s",
             drone_get_enabled() ? "ON" : "OFF");

    snprintf(s_menu_items[MI_DRUM_ENGINE].label, MENU_LABEL_LEN, "Drum Mode");
    snprintf(s_menu_items[MI_DRUM_ENGINE].value, MENU_VALUE_LEN, "%s",
             sequencer_core_get_drum_engine() == SEQ_DRUM_PCM ? "PCM" : "Synth");

    snprintf(s_menu_items[MI_ADD_LAYER].label, MENU_LABEL_LEN, "Add Layer");
    snprintf(s_menu_items[MI_ADD_LAYER].value, MENU_VALUE_LEN, "%u/%u",
             (unsigned)seq_state.num_layers, (unsigned)MAX_LAYERS);

    {
        uint8_t li = seq_state.active_layer_idx;
        bool can_del = (li > 0 && seq_state.num_layers > 1);
        snprintf(s_menu_items[MI_REMOVE_LAYER].label, MENU_LABEL_LEN, "Del Layer");
        if (can_del)
            snprintf(s_menu_items[MI_REMOVE_LAYER].value, MENU_VALUE_LEN,
                     "L%u", (unsigned)(li + 1));
        else
            snprintf(s_menu_items[MI_REMOVE_LAYER].value, MENU_VALUE_LEN, "--");
    }

    /* Global FX (cached values; AMY has no getters). */
    snprintf(s_menu_items[MI_EQ_LOW].label, MENU_LABEL_LEN, "EQ Low");
    snprintf(s_menu_items[MI_EQ_LOW].value, MENU_VALUE_LEN, "%+ddB",
             (int)s_fx.eq_low_db);
    snprintf(s_menu_items[MI_EQ_MID].label, MENU_LABEL_LEN, "EQ Mid");
    snprintf(s_menu_items[MI_EQ_MID].value, MENU_VALUE_LEN, "%+ddB",
             (int)s_fx.eq_mid_db);
    snprintf(s_menu_items[MI_EQ_HIGH].label, MENU_LABEL_LEN, "EQ High");
    snprintf(s_menu_items[MI_EQ_HIGH].value, MENU_VALUE_LEN, "%+ddB",
             (int)s_fx.eq_high_db);
    snprintf(s_menu_items[MI_ECHO_LEVEL].label, MENU_LABEL_LEN, "Echo");
    snprintf(s_menu_items[MI_ECHO_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)s_fx.echo_level);
    snprintf(s_menu_items[MI_CHORUS_LEVEL].label, MENU_LABEL_LEN, "Chorus");
    snprintf(s_menu_items[MI_CHORUS_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)s_fx.chorus_level);
    snprintf(s_menu_items[MI_REVERB_LEVEL].label, MENU_LABEL_LEN, "Reverb");
    snprintf(s_menu_items[MI_REVERB_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)s_fx.reverb_level);

    /* "Presets alter global FX? y/n" — OFF makes Juno presets per-synth. */
    snprintf(s_menu_items[MI_PRESET_GLOBAL_FX].label, MENU_LABEL_LEN, "Preset FX");
    snprintf(s_menu_items[MI_PRESET_GLOBAL_FX].value, MENU_VALUE_LEN, "%s",
             s_fx.presets_alter_global ? "ON" : "OFF");

    /* Master output volume (0..200%, unity=100%). Written to amy_global.volume[]. */
    snprintf(s_menu_items[MI_VOLUME].label, MENU_LABEL_LEN, "Volume");
    snprintf(s_menu_items[MI_VOLUME].value, MENU_VALUE_LEN, "%.0f%%",
             (double)(amy_fx_get_master_volume() * 100.0f));

    /* Runtime PCM sampler (custompatches/sample_rec): the label previews what
     * ARM will target (the currently selected track), since that selection is
     * only snapshotted at the moment the user actually arms. */
    snprintf(s_menu_items[MI_SAMPLE].label, MENU_LABEL_LEN, "Sample");
    switch (sample_rec_get_state()) {
        case SAMPLE_REC_ARMED:
            snprintf(s_menu_items[MI_SAMPLE].value, MENU_VALUE_LEN, "Rec!");
            break;
        case SAMPLE_REC_RECORDING:
            snprintf(s_menu_items[MI_SAMPLE].value, MENU_VALUE_LEN, "Rec %u%%",
                     (unsigned)sample_rec_get_progress_pct());
            break;
        case SAMPLE_REC_READY:
            snprintf(s_menu_items[MI_SAMPLE].value, MENU_VALUE_LEN, "Assign?");
            break;
        case SAMPLE_REC_IDLE:
        default:
            snprintf(s_menu_items[MI_SAMPLE].value, MENU_VALUE_LEN, "Arm T%u",
                     (unsigned)(seq_state.selected_track + 1));
            break;
    }
    snprintf(s_menu_items[MI_SAMPLE_CANCEL].label, MENU_LABEL_LEN, "Smp Cancel");

    out->items   = s_menu_items;
    out->count   = MI_COUNT;
    out->cursor  = seq_state.menu_cursor;
    out->editing = seq_state.menu_editing;
}

/* Signature of the menu overlay. */
[[gnu::pure]] uint32_t menu_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    menu_view_t v;
    menu_build_view(&v);
    h = fnv1a_bytes(h, &v.cursor, sizeof(v.cursor));
    h = fnv1a_bytes(h, &v.editing, sizeof(v.editing));
    for (uint8_t i = 0; i < v.count; i++) {
        h = fnv1a_bytes(h, v.items[i].label, sizeof(v.items[i].label));
        h = fnv1a_bytes(h, v.items[i].value, sizeof(v.items[i].value));
    }
    return h;
}

/* True for items that hold an adjustable value (vs. one-shot actions). */
static bool menu_item_is_value(menu_item_id_t id)
{
    switch (id) {
        case MI_BPM:
        case MI_QUANT_ENABLED:
        case MI_QUANT_SCALE:
        case MI_QUANT_ROOT:
        case MI_ARP_ENABLED:
        case MI_DRONE_ENABLED:
        case MI_DRUM_ENGINE:
        case MI_EQ_LOW:
        case MI_EQ_MID:
        case MI_EQ_HIGH:
        case MI_ECHO_LEVEL:
        case MI_CHORUS_LEVEL:
        case MI_REVERB_LEVEL:
        case MI_PRESET_GLOBAL_FX:
        case MI_VOLUME:
            return true;
        default:
            return false;
    }
}

/* Apply an encoder delta to the currently-entered menu value. */
static void menu_edit_value(menu_item_id_t id, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    switch (id) {
        case MI_BPM:
            synth_ui_set_bpm((uint16_t)((int)seq_get_bpm() + delta));
            break;
        case MI_QUANT_ENABLED:
            if (dir != 0)
                sequencer_core_set_quantizer_enabled(
                    !sequencer_core_get_quantizer_enabled());
            break;
        case MI_QUANT_SCALE: {
            int n = (int)quantizer_scale_count();
            int cur = (int)sequencer_core_get_quantizer_scale();
            int ni = (cur + dir + n) % n;
            sequencer_core_set_quantizer_scale((uint8_t)ni);
            break;
        }
        case MI_QUANT_ROOT: {
            int r = (int)sequencer_core_get_quantizer_root_note() + delta;
            r = SEQ_CLAMP_INT(r, 0, 127);
            sequencer_core_set_quantizer_root_note((uint8_t)r);
            break;
        }
        case MI_ARP_ENABLED:
            if (dir != 0) arp_set_enabled(!arp_get_enabled());
            break;
        case MI_DRONE_ENABLED:
            if (dir != 0) drone_set_enabled(!drone_get_enabled());
            break;
        case MI_DRUM_ENGINE:
            if (dir != 0) {
                sequencer_core_set_drum_engine(
                    sequencer_core_get_drum_engine() == SEQ_DRUM_PCM
                        ? SEQ_DRUM_SYNTH : SEQ_DRUM_PCM);
            }
            break;
        case MI_EQ_LOW: {
            int v = SEQ_CLAMP_INT((int)s_fx.eq_low_db + dir, -15, 15);
            s_fx.eq_low_db = (int8_t)v; fx_push_eq();
            break;
        }
        case MI_EQ_MID: {
            int v = SEQ_CLAMP_INT((int)s_fx.eq_mid_db + dir, -15, 15);
            s_fx.eq_mid_db = (int8_t)v; fx_push_eq();
            break;
        }
        case MI_EQ_HIGH: {
            int v = SEQ_CLAMP_INT((int)s_fx.eq_high_db + dir, -15, 15);
            s_fx.eq_high_db = (int8_t)v; fx_push_eq();
            break;
        }
        case MI_ECHO_LEVEL: {
            int v = SEQ_CLAMP_INT((int)s_fx.echo_level + dir * 5, 0, 100);
            s_fx.echo_level = (uint8_t)v; fx_push_echo();
            break;
        }
        case MI_CHORUS_LEVEL: {
            int v = SEQ_CLAMP_INT((int)s_fx.chorus_level + dir * 5, 0, 100);
            s_fx.chorus_level = (uint8_t)v; fx_push_chorus();
            break;
        }
        case MI_REVERB_LEVEL: {
            int v = SEQ_CLAMP_INT((int)s_fx.reverb_level + dir * 5, 0, 100);
            s_fx.reverb_level = (uint8_t)v; fx_push_reverb();
            break;
        }
        case MI_PRESET_GLOBAL_FX:
            if (dir != 0) {
                s_fx.presets_alter_global = !s_fx.presets_alter_global;
                /* Turning the guard back ON re-imposes the user's cached FX
                 * immediately, undoing whatever the last preset left behind. */
                if (!s_fx.presets_alter_global) synth_ui_fx_reassert_global();
            }
            break;
        case MI_VOLUME: {
            /* 5% steps, range 0..200% (0.0..2.0 linear). Clamping and the
             * write to amy_global.volume[] are handled by the setter. */
            amy_fx_set_master_volume(amy_fx_get_master_volume() + (float)dir * 0.05f);
            break;
        }
        default:
            break;
    }
}

void synth_ui_menu_toggle(void)
{
    /* The graph editor is the top overlay; don't let the menu fight it. */
    if (synth_ui_graph_is_active()) return;
    seq_state.menu_open    = !seq_state.menu_open;
    seq_state.menu_editing = false;
    if (seq_state.menu_open && seq_state.menu_cursor >= MI_COUNT) {
        seq_state.menu_cursor = 0;
    }
    s_force_redraw = true;
    ESP_LOGI(TAG, "menu %s", seq_state.menu_open ? "open" : "closed");
}

bool synth_ui_menu_is_active(void)
{
    return seq_state.menu_open;
}

bool synth_ui_menu_handle_encoder(long delta)
{
    if (!seq_state.menu_open) return false;
    if (delta == 0) return true;

    if (seq_state.menu_editing) {
        menu_edit_value((menu_item_id_t)seq_state.menu_cursor, (int)delta);
    } else {
        int n = (int)MI_COUNT;
        int c = (int)seq_state.menu_cursor + (int)delta;
        /* clamp (no wrap) so the list feels bounded */
        c = SEQ_CLAMP_INT(c, 0, n - 1);
        seq_state.menu_cursor = (uint8_t)c;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_menu_handle_button(void)
{
    if (!seq_state.menu_open) return false;

    menu_item_id_t id = (menu_item_id_t)seq_state.menu_cursor;

    if (menu_item_is_value(id)) {
        /* Toggle in/out of editing this value. */
        seq_state.menu_editing = !seq_state.menu_editing;
    } else {
        /* Action item: run it and close the menu. */
        switch (id) {
            case MI_SCREEN_SEQ:
                seq_state.ui_mode = UI_MODE_SEQUENCER;
                seq_state.menu_open = false;
                break;
            case MI_SCREEN_ARP:
                seq_state.ui_mode = UI_MODE_ARP;
                seq_state.menu_open = false;
                break;
            case MI_SCREEN_DRONE:
                seq_state.ui_mode = UI_MODE_DRONE;
                seq_state.menu_open = false;
                break;
            case MI_SCREEN_PROG:
                seq_state.ui_mode = UI_MODE_PROG;
                seq_state.menu_open = false;
                break;
            case MI_SCREEN_TRACKOPTS:
                seq_state.ui_mode = UI_MODE_TRACKOPTS;
                seq_state.menu_open = false;
                s_to_layer = seq_state.active_layer_idx;
                s_to_track = seq_state.selected_track;
                break;
#if CONFIG_SYNTH_CUSTOM_FM
            case MI_SCREEN_FM:
                seq_state.ui_mode = UI_MODE_FM;
                seq_state.menu_open = false;
                break;
#endif
            case MI_ADD_LAYER:
                if (seq_state.num_layers < MAX_LAYERS)
                    synth_ui_request_add_layer();
                seq_state.menu_open = false;
                break;
            case MI_REMOVE_LAYER:
                s_to_layer = seq_state.active_layer_idx;
                synth_ui_request_delete_to_layer();
                seq_state.menu_open = false;
                break;
            case MI_SAMPLE:
                switch (sample_rec_get_state()) {
                    case SAMPLE_REC_IDLE:
                        if (seq_state.layers[seq_state.active_layer_idx].type == SEQ_LAYER_DRUM) {
                            sample_rec_arm(seq_state.active_layer_idx, seq_state.selected_track);
                        } else {
                            ESP_LOGW(TAG, "sample_rec: select a drum track first");
                        }
                        break;
                    case SAMPLE_REC_ARMED:
                        sample_rec_start();
                        break;
                    case SAMPLE_REC_READY:
                        sample_rec_assign();
                        break;
                    case SAMPLE_REC_RECORDING:
                    default:
                        break;   /* capture runs in the background regardless of the menu */
                }
                seq_state.menu_open = false;
                break;
            case MI_SAMPLE_CANCEL:
                sample_rec_cancel();
                seq_state.menu_open = false;
                break;
            default:
                break;
        }
        seq_state.menu_editing = false;
    }
    s_force_redraw = true;
    return true;
}

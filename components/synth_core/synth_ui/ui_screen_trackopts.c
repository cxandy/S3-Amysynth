#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "seq_clamp.h"

/* ════════════════════════════════════════════════════════════════════════
 *  Track Options screen — per-track repeat rate/mute/solo + per-layer chord
 * ════════════════════════════════════════════════════════════════════════ */

uint8_t s_to_layer   = 0;  /* owner: this file; task clamps it; menu sets it */
uint8_t s_to_track   = 0;  /* owner: this file; menu sets it */
static uint8_t s_to_cursor  = 0;   /* trackopts-internal only */
static bool    s_to_editing = false; /* trackopts-internal only */

/* Next repeat rate in the 1→2→4→8→1 cycle. */
static uint8_t to_next_repeat_rate(uint8_t rr, int delta)
{
    static const uint8_t rates[] = { 1, 2, 4, 8 };
    int n = (int)(sizeof(rates) / sizeof(rates[0]));
    int idx = 0;
    for (int i = 0; i < n; i++) if (rates[i] == rr) idx = i;
    idx = (idx + delta) % n;
    if (idx < 0) idx += n;
    return rates[idx];
}

void trackopts_build_view(trackopts_view_t *out)
{
    uint8_t li = s_to_layer;
    uint8_t tr = s_to_track;
    bool melodic = (sequencer_core_get_layer_type(li) == SEQ_LAYER_MELODIC);

    /* Drum tracks expose only Repeat Rate, Mute, Solo, and the target
     * selectors — chord rows are melodic-only. */
    if (!melodic && s_to_cursor > TO_ROW_SOLO) s_to_cursor = TO_ROW_SOLO;

    out->layer_idx    = li;                 /* 0-based; renderer displays as 1-based */
    out->track_idx    = tr;
    out->layer_count  = seq_state.num_layers;
    out->track_count  = SEQ_TRACKS;
    out->melodic      = melodic;
    out->repeat_rate  = (uint8_t)sequencer_core_get_track_repeat_rate(li, tr);
    out->track_mute   = sequencer_core_get_track_mute(li, tr);
    out->track_solo   = sequencer_core_get_track_solo(li, tr);
    out->chord_locked = sequencer_core_progression_get_enabled();
    out->cursor       = s_to_cursor;
    out->editing      = s_to_editing;

    bool mode = false; uint8_t root = 0; chord_type_t ct = CHORD_MAJ;
    sequencer_core_get_layer_chord(li, &mode, &root, &ct);
    out->chord_mode = mode;
    out->chord_root = root;
    out->chord_type = ct;
}

uint32_t trackopts_view_signature(trackopts_view_t *out)
{
    uint32_t h = FNV1A_OFFSET;
    trackopts_build_view(out);
    h = fnv1a_bytes(h, &out->layer_idx,   sizeof(out->layer_idx));
    h = fnv1a_bytes(h, &out->track_idx,   sizeof(out->track_idx));
    h = fnv1a_bytes(h, &out->layer_count, sizeof(out->layer_count));
    h = fnv1a_bytes(h, &out->track_count, sizeof(out->track_count));
    h = fnv1a_bytes(h, &out->melodic,     sizeof(out->melodic));
    h = fnv1a_bytes(h, &out->repeat_rate, sizeof(out->repeat_rate));
    h = fnv1a_bytes(h, &out->track_mute,  sizeof(out->track_mute));
    h = fnv1a_bytes(h, &out->track_solo,  sizeof(out->track_solo));
    h = fnv1a_bytes(h, &out->chord_mode,  sizeof(out->chord_mode));
    h = fnv1a_bytes(h, &out->chord_root,  sizeof(out->chord_root));
    h = fnv1a_bytes(h, &out->chord_type,  sizeof(out->chord_type));
    h = fnv1a_bytes(h, &out->cursor,      sizeof(out->cursor));
    h = fnv1a_bytes(h, &out->editing,     sizeof(out->editing));
    return h;
}

bool synth_ui_trackopts_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_TRACKOPTS && !seq_state.menu_open;
}

bool synth_ui_trackopts_handle_encoder(int delta)
{
    if (!synth_ui_trackopts_is_active()) return false;
    uint8_t li = s_to_layer;
    uint8_t tr = s_to_track;
    bool melodic = (sequencer_core_get_layer_type(li) == SEQ_LAYER_MELODIC);
    uint8_t max_row = melodic ? TO_ROW_TYPE : TO_ROW_SOLO;

    if (s_to_editing) {
        switch (s_to_cursor) {
            case TO_ROW_LAYER: {
                int nl = (int)s_to_layer + delta;
                int nc = (int)seq_state.num_layers;
                if (nl < 0) nl += nc; else if (nl >= nc) nl -= nc;
                s_to_layer = (uint8_t)nl;
                /* Re-clamp track and cursor if the new layer is a drum layer. */
                if (sequencer_core_get_layer_type(s_to_layer) != SEQ_LAYER_MELODIC &&
                    s_to_cursor > TO_ROW_SOLO) {
                    s_to_cursor = TO_ROW_SOLO;
                }
                break;
            }
            case TO_ROW_TRACK: {
                int nt = (int)s_to_track + delta;
                if (nt < 0) nt += SEQ_TRACKS; else if (nt >= SEQ_TRACKS) nt -= SEQ_TRACKS;
                s_to_track = (uint8_t)nt;
                break;
            }
            case TO_ROW_REPEAT: {
                uint8_t rr = (uint8_t)sequencer_core_get_track_repeat_rate(li, tr);
                rr = to_next_repeat_rate(rr, delta);
                sequencer_core_set_track_repeat_rate(li, tr, (seq_repeat_rate_t)rr);
                break;
            }
            case TO_ROW_MUTE: {
                sequencer_core_set_track_mute(li, tr, !sequencer_core_get_track_mute(li, tr));
                break;
            }
            case TO_ROW_SOLO: {
                sequencer_core_set_track_solo(li, tr, !sequencer_core_get_track_solo(li, tr));
                break;
            }
            case TO_ROW_CHORD: {
                if (sequencer_core_progression_get_enabled()) break;
                bool mode = false; uint8_t root = 0; chord_type_t ct = CHORD_MAJ;
                sequencer_core_get_layer_chord(li, &mode, &root, &ct);
                if (mode) sequencer_core_progression_clear_layer_chord(li);
                else      sequencer_core_progression_set_layer_chord(li, root, ct);
                break;
            }
            case TO_ROW_ROOT:
            case TO_ROW_TYPE: {
                if (sequencer_core_progression_get_enabled()) break;
                bool mode = false; uint8_t root = 0; chord_type_t ct = CHORD_MAJ;
                sequencer_core_get_layer_chord(li, &mode, &root, &ct);
                if (!mode) break;
                if (s_to_cursor == TO_ROW_ROOT) {
                    int nr = ((int)root + delta) % 12;
                    if (nr < 0) nr += 12;
                    root = (uint8_t)nr;
                } else {
                    int nct = (int)ct + delta;
                    /* Real chords only - CHORD_OFF is a drone affordance. */
                    while (nct < 0) nct += CHORD_REAL_COUNT;
                    nct %= CHORD_REAL_COUNT;
                    ct = (chord_type_t)nct;
                }
                sequencer_core_progression_set_layer_chord(li, root, ct);
                break;
            }
            default: break;
        }
    } else {
        int nc = (int)s_to_cursor + delta;
        if (nc < 0) nc = (int)max_row;
        if (nc > (int)max_row) nc = 0;
        s_to_cursor = (uint8_t)nc;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_trackopts_handle_button(void)
{
    if (!synth_ui_trackopts_is_active()) return false;
    if (sequencer_core_get_layer_type(s_to_layer) != SEQ_LAYER_MELODIC &&
        s_to_cursor > TO_ROW_SOLO) {
        s_to_cursor = TO_ROW_SOLO;
    }
    s_to_editing = !s_to_editing;
    s_force_redraw = true;
    return true;
}

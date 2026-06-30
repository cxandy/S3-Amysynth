#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "seq_clamp.h"

/* ════════════════════════════════════════════════════════════════════════
 *  Track Options screen — per-track repeat rate + per-layer manual chord
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

    /* Drum tracks expose only Repeat Rate and the target selectors. */
    if (!melodic && s_to_cursor > TO_ROW_REPEAT) s_to_cursor = TO_ROW_REPEAT;

    out->layer_idx    = li;                 /* 0-based; renderer displays as 1-based */
    out->track_idx    = tr;
    out->layer_count  = seq_state.num_layers;
    out->track_count  = SEQ_TRACKS;
    out->melodic      = melodic;
    out->repeat_rate  = (uint8_t)sequencer_core_get_track_repeat_rate(li, tr);
    out->chord_locked = sequencer_core_progression_get_enabled();
    out->cursor       = s_to_cursor;
    out->editing      = s_to_editing;

    bool mode = false; uint8_t root = 0; chord_type_t ct = CHORD_MAJ;
    sequencer_core_get_layer_chord(li, &mode, &root, &ct);
    out->chord_mode = mode;
    out->chord_root = root;
    out->chord_type = ct;
}

uint32_t trackopts_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    trackopts_view_t v;
    trackopts_build_view(&v);
    h = fnv1a_bytes(h, &v.layer_idx,   sizeof(v.layer_idx));
    h = fnv1a_bytes(h, &v.track_idx,   sizeof(v.track_idx));
    h = fnv1a_bytes(h, &v.layer_count, sizeof(v.layer_count));
    h = fnv1a_bytes(h, &v.track_count, sizeof(v.track_count));
    h = fnv1a_bytes(h, &v.melodic,     sizeof(v.melodic));
    h = fnv1a_bytes(h, &v.repeat_rate, sizeof(v.repeat_rate));
    h = fnv1a_bytes(h, &v.chord_mode,  sizeof(v.chord_mode));
    h = fnv1a_bytes(h, &v.chord_root,  sizeof(v.chord_root));
    h = fnv1a_bytes(h, &v.chord_type,  sizeof(v.chord_type));
    h = fnv1a_bytes(h, &v.cursor,      sizeof(v.cursor));
    h = fnv1a_bytes(h, &v.editing,     sizeof(v.editing));
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
    uint8_t max_row = melodic ? TO_ROW_TYPE : TO_ROW_REPEAT;

    if (s_to_editing) {
        switch (s_to_cursor) {
            case TO_ROW_LAYER: {
                int nl = (int)s_to_layer + delta;
                int nc = (int)seq_state.num_layers;
                if (nl < 0) nl += nc; else if (nl >= nc) nl -= nc;
                s_to_layer = (uint8_t)nl;
                /* Re-clamp track and cursor if the new layer is a drum layer. */
                if (sequencer_core_get_layer_type(s_to_layer) != SEQ_LAYER_MELODIC &&
                    s_to_cursor > TO_ROW_REPEAT) {
                    s_to_cursor = TO_ROW_REPEAT;
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
                    while (nct < 0) nct += CHORD_TYPE_COUNT;
                    nct %= CHORD_TYPE_COUNT;
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
        s_to_cursor > TO_ROW_REPEAT) {
        s_to_cursor = TO_ROW_REPEAT;
    }
    s_to_editing = !s_to_editing;
    s_force_redraw = true;
    return true;
}

#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "seq_clamp.h"

/* ════════════════════════════════════════════════════════════════════════
 *  SONG SCREEN
 * ════════════════════════════════════════════════════════════════════════ */

static uint8_t s_song_cursor  = 0;   /* 0=enabled, 1=loop, 2..count+1=scenes */
static bool    s_song_editing = false;
static uint8_t s_song_field   = 0;   /* edit sub-field: 0=bars, 1..4=bit3..bit0 */

/* Allowed scene durations, in bars. Loop-oriented lengths: 1 bar, power-of-two
 * blocks up to 32. */
static const uint8_t s_song_durations[] = { 1, 2, 4, 8, 16, 32 };
#define SONG_NUM_DURATIONS ((int)(sizeof(s_song_durations) / sizeof(s_song_durations[0])))

void song_build_view(song_view_t *out)
{
    uint8_t count = sequencer_core_song_get_count();
    out->enabled       = sequencer_core_song_get_enabled();
    out->loop          = sequencer_core_song_get_loop();
    out->count         = count;
    out->current_scene = sequencer_core_song_get_current();
    out->cursor        = s_song_cursor;
    out->editing       = s_song_editing;
    out->edit_field    = s_song_field;
    out->bars_in_current = sequencer_core_song_bars_in_current();

    uint8_t n = (count < SONG_VIEW_MAX_SCENES) ? count : SONG_VIEW_MAX_SCENES;
    for (uint8_t i = 0; i < n; i++) {
        sequencer_core_song_get_scene(i, &out->scenes[i].bars,
                                      &out->scenes[i].layer_mask);
    }
}

uint32_t song_view_signature(song_view_t *out)
{
    uint32_t h = FNV1A_OFFSET;
    song_build_view(out);
    h = fnv1a_bytes(h, &out->enabled,       sizeof(out->enabled));
    h = fnv1a_bytes(h, &out->loop,          sizeof(out->loop));
    h = fnv1a_bytes(h, &out->count,         sizeof(out->count));
    h = fnv1a_bytes(h, &out->current_scene, sizeof(out->current_scene));
    h = fnv1a_bytes(h, &out->cursor,        sizeof(out->cursor));
    h = fnv1a_bytes(h, &out->editing,       sizeof(out->editing));
    h = fnv1a_bytes(h, &out->edit_field,    sizeof(out->edit_field));
    h = fnv1a_bytes(h, &out->bars_in_current, sizeof(out->bars_in_current));
    for (uint8_t i = 0; i < out->count; i++) {
        h = fnv1a_bytes(h, &out->scenes[i], sizeof(out->scenes[i]));
    }
    return h;
}

bool synth_ui_song_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_SONG && !seq_state.menu_open;
}

/* Find the index of `dur` in s_song_durations[], or the nearest-not-greater. */
static int song_duration_index(uint8_t dur)
{
    int idx = 0;
    for (int i = 0; i < SONG_NUM_DURATIONS; i++) {
        if (s_song_durations[i] <= dur) idx = i;
    }
    return idx;
}

bool synth_ui_song_handle_encoder(int delta)
{
    if (!synth_ui_song_is_active()) return false;
    uint8_t count = sequencer_core_song_get_count();
    /* Cursor range: 0=enable, 1=loop, 2..count+1=scenes. */
    uint8_t max_cursor = (uint8_t)(count + 1);

    if (s_song_editing && s_song_cursor >= 2 && s_song_cursor <= count + 1) {
        uint8_t si = (uint8_t)(s_song_cursor - 2);
        uint8_t bars; uint8_t mask;
        sequencer_core_song_get_scene(si, &bars, &mask);
        switch (s_song_field) {
            case 0: {  /* bars: cycle the allowed set */
                int di = song_duration_index(bars) + delta;
                while (di < 0) di += SONG_NUM_DURATIONS;
                di %= SONG_NUM_DURATIONS;
                bars = s_song_durations[di];
                break;
            }
            default: { /* mask: toggle one layer bit (field 1..4 = bit3..bit0) */
                uint8_t bit = (uint8_t)(4 - s_song_field);
                if (delta >= 0) mask |= (1u << bit);
                else            mask &= (uint8_t)~(1u << bit);
                break;
            }
        }
        sequencer_core_song_set_scene(si, bars, mask);
    } else {
        int nc = (int)s_song_cursor + delta;
        if (nc < 0) nc = (int)max_cursor;
        if (nc > (int)max_cursor) nc = 0;
        s_song_cursor = (uint8_t)nc;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_song_handle_button(void)
{
    if (!synth_ui_song_is_active()) return false;
    uint8_t count = sequencer_core_song_get_count();
    if (s_song_cursor == 0) {
        /* Enabled toggle. */
        sequencer_core_song_set_enabled(!sequencer_core_song_get_enabled());
    } else if (s_song_cursor == 1) {
        /* Loop toggle: ONE = play through once, then hold the last scene. */
        sequencer_core_song_set_loop(!sequencer_core_song_get_loop());
    } else if (!s_song_editing) {
        /* Enter field-edit on the focused scene, starting at bars. */
        s_song_editing = true;
        s_song_field   = 0;
    } else {
        /* Advance bars → bit3 → bit2 → bit1 → bit0, then exit edit. */
        if (s_song_field >= 4) {
            s_song_editing = false;
            s_song_field   = 0;
        } else {
            s_song_field++;
        }
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_song_add_scene(void)
{
    if (!synth_ui_song_is_active()) return false;
    if (sequencer_core_song_add_scene()) {
        /* Move the cursor to the freshly-appended row so it can be edited. */
        s_song_cursor  = (uint8_t)(sequencer_core_song_get_count() + 1);
        s_song_editing = false;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_song_delete_scene(void)
{
    if (!synth_ui_song_is_active()) return false;
    if (s_song_cursor >= 2 &&
        s_song_cursor <= (uint8_t)(sequencer_core_song_get_count() + 1)) {
        sequencer_core_song_delete_scene((uint8_t)(s_song_cursor - 2));
        uint8_t count = sequencer_core_song_get_count();
        if (s_song_cursor > count + 1) s_song_cursor = (uint8_t)(count + 1);
        s_song_editing = false;
    }
    s_force_redraw = true;
    return true;
}
#include "synth_ui/synth_ui_internal.h"
#include "sequencer_core.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "synth_ui";

/* ── Sequencer UI state ─────────────────────────────────────────────────── */

synth_ui_state_t seq_state = {
    /* layers[] is zero-initialized by C99 partial-init rules */
    .num_layers       = 0,
    .active_layer_idx = 0,
    .current_pattern  = 1,
    .current_step     = 0,
    .playing          = true,
    .selected_track   = 0,
    .selected_step    = 0,
    .edit_mode        = true,
    .drum_select_mode = false,
};

/* Mirror the UI's grid of active steps into the audio core so the core
 * schedules notes for every step the user has toggled on. Only "on" steps are
 * pushed; "off" steps are the core's default after a fresh layer add. */
void sync_layer_to_core(uint8_t li)
{
    seq_layer_t *layer = &seq_state.layers[li];
    for (int t = 0; t < SEQ_TRACKS; t++) {
        for (int s = 0; s < layer->num_steps; s++) {
            if (layer->grid[t][s]) {
                sequencer_core_set_step(li, t, s, true);
            }
        }
    }
}

/* ── Note-name helper (local; mirrors display_seq.c's static one) ─────── */
void ui_note_name(uint8_t midi_note, char buf[4])
{
    static const char *const names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    int octave = (int)midi_note / 12 - 1;
    snprintf(buf, 4, "%s%d", names[midi_note % 12], octave);
}

/* ── Render-on-change: sequencer view signature ──────────────────────── */

/* Signature of the sequencer view (everything display_seq_draw_frame reads). */
uint32_t seq_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &seq_state.active_layer_idx, sizeof(seq_state.active_layer_idx));
    { uint16_t bpm_snap = sequencer_core_get_bpm(); h = fnv1a_bytes(h, &bpm_snap, sizeof(bpm_snap)); }
    h = fnv1a_bytes(h, &seq_state.playing, sizeof(seq_state.playing));
    h = fnv1a_bytes(h, &seq_state.current_step, sizeof(seq_state.current_step));
    h = fnv1a_bytes(h, &seq_state.edit_mode, sizeof(seq_state.edit_mode));
    h = fnv1a_bytes(h, &seq_state.selected_track, sizeof(seq_state.selected_track));
    h = fnv1a_bytes(h, &seq_state.selected_step, sizeof(seq_state.selected_step));
    h = fnv1a_bytes(h, &seq_state.drum_select_mode, sizeof(seq_state.drum_select_mode));
    h = fnv1a_bytes(h, &seq_state.patch_select_mode, sizeof(seq_state.patch_select_mode));
    /* Drum engine + PCM presets live in the core (sample_rec can change them
     * without going through the UI), so snapshot them into the mirror here —
     * this runs every frame, like the bpm snapshot above — and hash the
     * mirror so a change redraws. */
    seq_state.drum_pcm = (sequencer_core_get_drum_engine() == SEQ_DRUM_PCM);
    h = fnv1a_bytes(h, &seq_state.drum_pcm, sizeof(seq_state.drum_pcm));
    if (seq_state.num_layers > 0) {
        seq_layer_t *L = &seq_state.layers[seq_state.active_layer_idx];
        if (L->type == SEQ_LAYER_DRUM) {
            for (uint8_t t = 0; t < SEQ_TRACKS; t++) {
                L->track_pcm_preset[t] =
                    sequencer_core_get_drum_pcm_preset(seq_state.active_layer_idx, t);
            }
            h = fnv1a_bytes(h, L->track_pcm_preset, sizeof(L->track_pcm_preset));
        }
        h = fnv1a_bytes(h, &L->type, sizeof(L->type));
        h = fnv1a_bytes(h, &L->patch, sizeof(L->patch));
        h = fnv1a_bytes(h, L->track_patch, sizeof(L->track_patch));
        h = fnv1a_bytes(h, &L->num_steps, sizeof(L->num_steps));
        h = fnv1a_bytes(h, &L->step_page, sizeof(L->step_page));
        h = fnv1a_bytes(h, L->track_base_note, sizeof(L->track_base_note));
        h = fnv1a_bytes(h, L->grid, sizeof(L->grid));
    }
    return h;
}

/* ── Public accessors ─────────────────────────────────────────────────── */

uint16_t seq_get_bpm(void)
{
    return sequencer_core_get_bpm();
}

uint8_t seq_get_active_layer_idx(void)
{
    return seq_state.active_layer_idx;
}

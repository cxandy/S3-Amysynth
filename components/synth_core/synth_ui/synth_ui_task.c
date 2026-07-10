#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/fm_voice.h"
#include "custompatches/sample_rec.h"
#include "display_seq.h"
#include "display_drone.h"
#include "display_prog.h"
#include "display_trackopts.h"
#include "display_menu.h"
#include "display_arp.h"
#include "display_hint.h"
#include "synth_ui_hint.h"
#include "amy_helpers.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "seq_clamp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include <string.h>

static const char *TAG_TASK = "synth_ui";

static u8g2_t *s_u8g2 = NULL;

/* Set by mode/layout transitions to force one redraw regardless of the
 * render-on-change signature (guards first-frame-after-transition staleness). */
volatile bool s_force_redraw = true;

/* Deferred layer-delete request, consumed by synth_ui_task each frame. */
static volatile bool    s_layer_delete_pending = false;
static volatile uint8_t s_layer_delete_idx     = 0;

/* Deferred layer-add request, consumed by synth_ui_task each frame.
 * The add path (memset + patch-string parse via amy_send_patch) is too heavy
 * for the esp_timer task's 3584-byte stack — defer it here exactly as delete. */
static volatile bool    s_layer_add_pending    = false;

/* DEBUG: bisect heap corruption inside the init chain. Gated by
 * CONFIG_AMYSYNTH_HEAP_CHECK (menuconfig: Heap Diagnostics); off by default, in
 * which case every checkpoint compiles to nothing. When on, the first
 * "HEAP CORRUPT" line names the exact sub-step that smashed the heap. */
#if CONFIG_AMYSYNTH_HEAP_CHECK
#define SEQ_HEAP_CHECK(where) do { \
    if (!heap_caps_check_integrity_all(true)) { \
        ESP_LOGE(TAG_TASK, "HEAP CORRUPT detected at: %s", where); \
    } else { \
        ESP_LOGI(TAG_TASK, "HEAP OK at: %s", where); \
    } \
} while (0)
#else
#define SEQ_HEAP_CHECK(where) do { (void)(where); } while (0)
#endif

static void synth_ui_task(void *pvParameters)
{
    (void)pvParameters;
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t delay = pdMS_TO_TICKS(50); /* 20 Hz */
    uint32_t last_sig = 0;
    /* Which top-level view was rendered last frame; a change forces a redraw. */
    ui_view_id_t last_view = UI_VIEW_SEQ;
    for (;;) {
        /* Coalesced arp re-emit: setters mark the arp dirty; we perform at most
         * one full re-emit per frame here, collapsing fast encoder edits. */
        arp_core_service();
        /* Drone: advance the tempo-locked filter sweep + keep the LFO in sync.
         * Cheap no-op while the drone is disabled. */
        drone_core_service();
        sequencer_core_lfo_service();
        sequencer_core_progression_service();

        /* Drain deferred layer-delete request (must run in synth_ui_task so that
         * the array compaction is serialized against all other seq_state readers
         * on Core 0). */
        /* Ordering: DELETE is drained before ADD so we never add into a slot
         * that a concurrent delete has not yet freed and compacted. */
        if (s_layer_delete_pending) {
            s_layer_delete_pending = false;
            uint8_t del_idx = s_layer_delete_idx;
            if (sequencer_core_delete_layer(del_idx)) {
                /* Mirror compaction in the UI-side seq_state. */
                uint8_t tail = (uint8_t)(seq_state.num_layers - del_idx - 1);
                if (tail > 0) {
                    memmove(&seq_state.layers[del_idx],
                            &seq_state.layers[del_idx + 1],
                            tail * sizeof(seq_state.layers[0]));
                }
                seq_state.num_layers--;
                /* Clamp indices that may now point past the end. */
                if (seq_state.active_layer_idx >= seq_state.num_layers)
                    seq_state.active_layer_idx = (uint8_t)(seq_state.num_layers - 1);
                if (s_graph_layer >= seq_state.num_layers)
                    s_graph_layer = (uint8_t)(seq_state.num_layers - 1);
                if (s_to_layer >= seq_state.num_layers)
                    s_to_layer = (uint8_t)(seq_state.num_layers - 1);
                /* Drop the Step Trig overlay: after compaction its cached
                 * (layer,track,step) may point at a different layer's steps. */
                synth_ui_stepedit_close();
                ESP_LOGI(TAG_TASK, "UI delete layer %u (%u layers remain)",
                         del_idx, seq_state.num_layers);
            }
        }

        /* Drain deferred layer-add request (runs in synth_ui_task, 4096-byte
         * stack; safe for the heavy memset + patch-string parse that would
         * overflow the esp_timer task's 3584-byte stack). */
        if (s_layer_add_pending) {
            s_layer_add_pending = false;
            /* Re-check cap; num_layers may have changed since flag was set. */
            if (seq_state.num_layers < MAX_LAYERS) {
                synth_ui_add_layer(SEQ_LAYER_MELODIC, SEQ_STEPS);
            }
        }

        seq_state.current_step =
            sequencer_core_get_current_step(seq_state.active_layer_idx);
        if (s_u8g2) {
            /* The whole screen/overlay precedence now lives in one place,
             * synth_ui_active_view(); this task renders whatever it resolves.
             * signature() builds the active view into vw and returns the FNV
             * render-gate hash, and the draw below reuses vw — never built
             * twice. Adding a view is a one-row change in ui_view_table[]. */
            ui_view_id_t view = synth_ui_active_view();
            const ui_view_desc_t *desc = &ui_view_table[view];
            ui_view_vw_t vw;
            uint32_t sig = desc->signature(&vw);
            bool force = s_force_redraw || (view != last_view);

            if (force || sig != last_sig) {
                desc->draw(s_u8g2, &vw);
                /* Persistent button-hint strip: composited into the buffer on
                 * top of whatever the view above just drew, so no per-screen
                 * renderer needs to know about it. Every screen's draw_frame
                 * now only fills the buffer -- it no longer sends -- so this
                 * is the single physical transfer per redraw. Sending twice
                 * (once without the hint, once with) used to make the hint
                 * strip visibly flicker on every redraw, in every UI state. */
                if (synth_ui_hint_visible()) {
                    display_hint_draw(s_u8g2, synth_ui_hint_text());
                }
                u8g2_SendBuffer(s_u8g2);
                last_sig = sig;
                last_view = view;
                s_force_redraw = false;
            }
        }
        vTaskDelayUntil(&last_wake_time, delay);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void synth_ui_init(u8g2_t *u8g2)
{
    s_u8g2 = u8g2;
    seq_state.playing  = true;
    seq_state.ui_mode  = UI_MODE_SEQUENCER;
    seq_state.menu_open = false;
    seq_state.menu_cursor = 0;
    seq_state.menu_editing = false;

    SEQ_HEAP_CHECK("ui_init: entry");
    amy_helpers_init();
    sequencer_core_init();
    SEQ_HEAP_CHECK("ui_init: after sequencer_core_init");
    arp_core_init();
    SEQ_HEAP_CHECK("ui_init: after arp_core_init");
    drone_core_init();
    SEQ_HEAP_CHECK("ui_init: after drone_core_init");
#if CONFIG_SYNTH_CUSTOM_FM
    fm_voice_default(&s_fm_voice);
#endif
    sample_rec_init();
    SEQ_HEAP_CHECK("ui_init: after sample_rec_init");

    /* Add drum layer (index 0). */
    synth_ui_add_layer(SEQ_LAYER_DRUM, SEQ_STEPS);
    SEQ_HEAP_CHECK("ui_init: after add_layer(drum)");

    /* Default pattern: full 4-on-the-floor house groove across all 4 tracks so
     * the boot loop is immediately musical (was kick+snare only, leaving the hat
     * and perc tracks silent). Velocity accent/jitter engine adds the groove.
     *   track 0 kick : every quarter (the "floor")
     *   track 1 snare: backbeat (beats 2 & 4)
     *   track 2 hat  : off-beat 8ths ("tss" between the kicks)
     *   track 3 perc : light syncopation for movement */
    seq_layer_t *drum = &seq_state.layers[0];
    drum->grid[0][0]  = drum->grid[0][4]  =
    drum->grid[0][8]  = drum->grid[0][12] = true;   /* kick  */
    drum->grid[1][4]  = drum->grid[1][12] = true;   /* snare */
    drum->grid[2][0]  = drum->grid[2][2]  = drum->grid[2][4]  = drum->grid[2][6]  =
    drum->grid[2][8]  = drum->grid[2][10] = drum->grid[2][12] = drum->grid[2][14] = true; /* hats: all 8ths */
    drum->grid[3][7]  = drum->grid[3][15] = true;   /* perc  */

    sync_layer_to_core(0);
    SEQ_HEAP_CHECK("ui_init: after sync_layer_to_core(0)");
    sequencer_core_set_playing(true);
    SEQ_HEAP_CHECK("ui_init: after set_playing");

    /* Pin to Core 0: the OLED refresh does blocking I2C and is not latency
     * critical, so keep it off Core 1 where the AMY DSP now runs. */
    xTaskCreatePinnedToCore(synth_ui_task, "seq_ui", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG_TASK, "Sequencer UI + Core initialized");
}

uint8_t synth_ui_add_layer(seq_layer_type_t type, uint8_t num_steps)
{
    uint8_t li = sequencer_core_add_layer(type, num_steps);
    if (li == 0xFF) return 0xFF;

    seq_layer_t *layer = &seq_state.layers[li];
    memset(layer, 0, sizeof(seq_layer_t));
    layer->type       = type;
    layer->num_steps  = (num_steps == SEQ_MAX_STEPS) ? SEQ_MAX_STEPS : SEQ_STEPS;
    layer->num_tracks = SEQ_TRACKS;
    layer->step_page  = 0;

    if (type == SEQ_LAYER_MELODIC) {
        layer->patch = sequencer_core_get_layer_patch(li);
        /* Default: Cmaj7 voicing — C4 E4 G4 B4 */
        static const uint8_t mel_notes[SEQ_TRACKS] = {60, 64, 67, 71};
        for (int t = 0; t < SEQ_TRACKS; t++) {
            layer->track_base_note[t] = mel_notes[t];
            for (int s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = mel_notes[t];
            }
        }
    } else {
        /* Drums are per-track patches with role-based default pitches. Pull the
         * actual per-track patch + source note from the core (single source of
         * truth) so labels and pitch display stay correct as those defaults
         * evolve — no hardcoded mirror to drift out of sync. */
        for (int t = 0; t < SEQ_TRACKS; t++) {
            uint8_t note = sequencer_core_get_track_source_note(li, t);
            layer->track_base_note[t] = note;
            layer->track_patch[t] = sequencer_core_get_drum_patch(li, t);
            for (int s = 0; s < SEQ_MAX_STEPS; s++) {
                layer->step_note[t][s] = note;
            }
        }
        layer->patch = layer->track_patch[0];
    }

    seq_state.num_layers = li + 1;
    ESP_LOGI(TAG_TASK, "UI layer %d added (type=%d steps=%d)",
             li, type, layer->num_steps);
    return li;
}

/* Request a melodic layer add. Sets a pending flag consumed by synth_ui_task
 * on its next frame, so the heavy work (memset + patch-string parse) runs on
 * the 4096-byte task stack rather than the 3584-byte esp_timer task stack.
 * Safe to call from any Core-0 context (esp_timer cb, button handler, etc.). */
void synth_ui_request_add_layer(void)
{
    if (seq_state.num_layers >= MAX_LAYERS) return;
    s_layer_add_pending = true;
}

/* Schedule a delete of the layer currently targeted by the Track Options
 * screen (s_to_layer). Uses a pending flag so array compaction is serialized
 * on Core 0 against all other seq_state readers. */
void synth_ui_request_delete_to_layer(void)
{
    uint8_t layer_idx = s_to_layer;
    /* Drum layer (0) is permanent; must always keep at least one layer. */
    if (layer_idx == 0 || seq_state.num_layers <= 1) return;
    if (layer_idx >= seq_state.num_layers) return;
    if (seq_state.layers[layer_idx].type == SEQ_LAYER_DRUM) return;
    s_layer_delete_idx     = layer_idx;
    s_layer_delete_pending = true;
}

void synth_ui_cycle_active_layer(void)
{
    if (seq_state.num_layers <= 1) return;
    /* Close the Step Trig overlay before the cursor moves: it edits the
     * (layer,track,step) under the old cursor, which is about to change. */
    synth_ui_stepedit_close();
    seq_state.active_layer_idx =
        (uint8_t)((seq_state.active_layer_idx + 1) % seq_state.num_layers);
    seq_state.selected_track = 0;
    seq_state.selected_step  = 0;
    seq_state.edit_mode      = true;
    ESP_LOGI(TAG_TASK, "Active layer -> %d (%s)",
             seq_state.active_layer_idx,
             seq_state.layers[seq_state.active_layer_idx].type == SEQ_LAYER_DRUM
             ? "drum" : "melodic");
}

/* Moves the step cursor by `delta` while in edit mode, or nudges BPM otherwise.
 * The cursor walks the current track's steps; running off either end wraps to
 * the adjacent track (and wraps track index too), so a long turn scans the
 * whole grid track-by-track. */
void synth_ui_handle_encoder(long delta)
{
    if (delta == 0) return;

    if (seq_state.edit_mode) {
        uint8_t li        = seq_state.active_layer_idx;
        uint8_t num_steps = seq_state.layers[li].num_steps;
        int new_step      = (int)seq_state.selected_step + (int)delta;

        if (new_step < 0) {
            /* Walked off the start: jump to the last step of the previous track. */
            new_step = (int)num_steps - 1;
            seq_state.selected_track =
                (uint8_t)((seq_state.selected_track + SEQ_TRACKS - 1) % SEQ_TRACKS);
        } else if (new_step >= (int)num_steps) {
            /* Walked off the end: jump to the first step of the next track. */
            new_step = 0;
            seq_state.selected_track =
                (uint8_t)((seq_state.selected_track + 1) % SEQ_TRACKS);
        }
        seq_state.selected_step = (uint8_t)new_step;

        /* 32-step layers display 16 steps per page; keep the cursor visible by
         * selecting the page (0 or 1) that contains the new step. */
        if (num_steps == SEQ_MAX_STEPS) {
            seq_state.layers[li].step_page = (uint8_t)(new_step / 16);
        }
    } else {
        synth_ui_set_bpm((uint16_t)((int)seq_get_bpm() + (int)delta));
    }
}

/* Encoder push: in edit mode toggles the step under the cursor on/off (and
 * mirrors that to the core); otherwise it acts as a play/pause toggle. */
void synth_ui_handle_button(void)
{
    if (seq_state.edit_mode) {
        uint8_t li = seq_state.active_layer_idx;
        uint8_t t  = seq_state.selected_track;
        uint8_t s  = seq_state.selected_step;
        seq_state.layers[li].grid[t][s] = !seq_state.layers[li].grid[t][s];
        sequencer_core_set_step(li, t, s, seq_state.layers[li].grid[t][s]);
    } else {
        seq_state.playing = !seq_state.playing;
        sequencer_core_set_playing(seq_state.playing);
    }
}

void synth_ui_toggle_playing(void)
{
    seq_state.playing = !seq_state.playing;
    sequencer_core_set_playing(seq_state.playing);
    ESP_LOGI(TAG_TASK, "Playback %s", seq_state.playing ? "started" : "stopped");
}

void synth_ui_set_bpm(uint16_t bpm)
{
    sequencer_core_set_bpm(bpm);
}

/* Transposes the selected track's note by `delta` semitones. We read/write the
 * *source* note (the user's raw choice) so repeated nudges accumulate cleanly;
 * the core may quantize it, so we read back the resolved note for display. */
void synth_ui_adjust_track_note(int delta)
{
    uint8_t li    = seq_state.active_layer_idx;
    uint8_t track = seq_state.selected_track;
    uint8_t note  = sequencer_core_get_track_source_note(li, track);
    uint8_t new_note = SEQ_CLAMP_U8(note + delta, 0, 127);
    sequencer_core_set_track_midi_note(li, track, new_note);
    /* Keep display in sync with resolved note after core clamp/quantize. */
    seq_state.layers[li].track_base_note[track] =
        sequencer_core_get_track_midi_note(li, track);
}

void synth_ui_set_drum_select_mode(bool held)
{
    seq_state.drum_select_mode = held;
}

void synth_ui_set_patch_select_mode(bool held)
{
    seq_state.patch_select_mode = held;
}

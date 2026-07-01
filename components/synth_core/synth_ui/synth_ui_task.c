#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/fm_voice.h"
#include "display_seq.h"
#include "display_drone.h"
#include "display_prog.h"
#include "display_trackopts.h"
#include "display_menu.h"
#include "display_arp.h"
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
    enum { V_SEQ, V_ARP, V_MENU, V_GRAPH, V_FILTER, V_LFO, V_DRONE, V_DRONE_VIS, V_PROG, V_TRACKOPTS, V_FM } last_view = V_SEQ;
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
            /* Precedence: filter editor > graph editor > menu overlay > arp/drone screen > seq. */
            bool graph = synth_ui_graph_is_active();
            int view;
            uint32_t sig;
            if (s_filter_active) {
                view = V_FILTER; sig = filter_view_signature();
            } else if (s_lfo_active) {
                view = V_LFO;    sig = lfo_view_signature();
            } else if (graph) {
                view = V_GRAPH; sig = graph_view_signature();
            } else if (seq_state.menu_open) {
                view = V_MENU;  sig = menu_view_signature();
            } else if (seq_state.ui_mode == UI_MODE_ARP) {
                view = V_ARP;   sig = arp_view_signature();
            } else if (seq_state.ui_mode == UI_MODE_DRONE && s_drone_vis_open) {
                view = V_DRONE_VIS; sig = drone_view_signature(); /* vis params change → redraw */
            } else if (seq_state.ui_mode == UI_MODE_DRONE) {
                view = V_DRONE; sig = drone_view_signature();
            } else if (seq_state.ui_mode == UI_MODE_PROG) {
                view = V_PROG;  sig = prog_view_signature();
            } else if (seq_state.ui_mode == UI_MODE_TRACKOPTS) {
                view = V_TRACKOPTS; sig = trackopts_view_signature();
            } else if (seq_state.ui_mode == UI_MODE_FM) {
                view = V_FM;    sig = fm_view_signature();
            } else {
                view = V_SEQ;   sig = seq_view_signature();
            }
            bool force = s_force_redraw || (view != last_view);

            if (force || sig != last_sig) {
                switch (view) {
                    case V_FILTER:
                        synth_ui_filter_view_draw(s_u8g2);
                        break;
                    case V_LFO:
                        synth_ui_lfo_view_draw(s_u8g2);
                        break;
                    case V_GRAPH:
                        synth_ui_graph_view_draw(s_u8g2);
                        break;
                    case V_MENU: {
                        menu_view_t mv;
                        menu_build_view(&mv);
                        display_menu_draw_frame(s_u8g2, &mv);
                        break;
                    }
                    case V_ARP: {
                        arp_view_t av;
                        arp_build_view(&av);
                        display_arp_draw_frame(s_u8g2, &av);
                        break;
                    }
                    case V_DRONE: {
                        drone_view_t dv;
                        drone_build_view(&dv);
                        display_drone_draw_frame(s_u8g2, &dv);
                        break;
                    }
                    case V_DRONE_VIS: {
                        const float SWEEP_MIN = 100.0f, SWEEP_MAX = 8000.0f;
                        float span = SWEEP_MAX - SWEEP_MIN;
                        float c = drone_get_amp_peak();
                        float m = drone_get_amp_duck();
                        float fl, cl;
                        drone_get_amp_levels_norm(&fl, &cl);
                        drone_vis_t dvis = {
                            .sweep_lo_norm  = (drone_get_sweep_lo() - SWEEP_MIN) / span,
                            .sweep_hi_norm  = (drone_get_sweep_hi() - SWEEP_MIN) / span,
                            .amp_const      = c,
                            .amp_mod        = m,
                            .amp_floor_norm = fl,
                            .amp_ceil_norm  = cl,
                            .resonance      = drone_get_resonance(),
                            .rate_idx       = (uint8_t)drone_get_rate(),
                            .sweep_bars     = drone_get_sweep_bars(),
                            .pattern_mask   = (uint8_t)0xFF, /* filled below */
                            .gate_len       = drone_get_gate_len(),
                            .wave_mode      = (drone_get_source() == DRONE_SRC_WAVE),
                        };
                        static const uint8_t pat_masks[] = {
                            0xFF, 0x55, 0xAA, 0x5B, 0x51
                        };
                        drone_pattern_t pat = drone_get_pattern();
                        if ((size_t)pat < sizeof(pat_masks))
                            dvis.pattern_mask = pat_masks[pat];
                        display_drone_vis_draw(s_u8g2, &dvis);
                        break;
                    }
                    case V_PROG: {
                        prog_view_t pv;
                        prog_build_view(&pv);
                        display_prog_draw_frame(s_u8g2, &pv);
                        break;
                    }
                    case V_TRACKOPTS: {
                        trackopts_view_t tv;
                        trackopts_build_view(&tv);
                        display_trackopts_draw_frame(s_u8g2, &tv);
                        break;
                    }
                    case V_FM: {
                        menu_view_t fv;
                        fm_build_view(&fv);
                        display_menu_draw_frame_titled(s_u8g2, "FM ALGO", &fv);
                        break;
                    }
                    default:
                        display_seq_draw_frame(s_u8g2, &seq_state, seq_get_bpm());
                        break;
                }
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
    fm_voice_default(&s_fm_voice);

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

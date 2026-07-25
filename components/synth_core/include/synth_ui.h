#pragma once
#include <stdint.h>
#include <stdbool.h>

#include "u8g2.h"
#include "display_seq.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef display_seq_state_t synth_ui_state_t;

/* Initialise the display, create the drum layer (index 0), and start
 * the FreeRTOS UI task. Must be called after amy_start(). */
void synth_ui_init(u8g2_t *u8g2);

/* Add a new sequencer layer (drum or melodic). Returns the layer index
 * or 0xFF if the layer table is full. Safe to call after init. */
uint8_t synth_ui_add_layer(seq_layer_type_t type, uint8_t num_steps);
void    synth_ui_request_add_layer(void);
void    synth_ui_request_delete_to_layer(void);

/* Advance the active layer displayed/edited on screen.
 * Resets the cursor to track 0, step 0, edit_mode = true. */
void synth_ui_cycle_active_layer(void);

/* Input dispatch ── called from encoder / button tasks */
void synth_ui_handle_encoder(long delta);
void synth_ui_handle_button(void);
/* Toggle the grid step under the cursor (edit_mode-gated, no play/pause
 * fallback). Returns true if a step was toggled. */
bool synth_ui_toggle_step_at_cursor(void);
void synth_ui_toggle_playing(void);
void synth_ui_set_bpm(uint16_t bpm);
void synth_ui_adjust_track_note(int delta);
void synth_ui_cycle_melodic_patch(int delta);
/* Cycle the selected drum track's patch through the curated drum list. Active
 * layer must be a drum layer; otherwise a no-op. */
void synth_ui_cycle_drum_patch(int delta);
void synth_ui_set_drum_select_mode(bool held);
void synth_ui_set_patch_select_mode(bool held);

/* ── Menu overlay (opened by the GPIO1 menu button) ──────────────────────
 * The menu is a modal overlay above the active screen (but below the graph
 * editor). While open it captures the encoder + encoder-button:
 *   - not editing: encoder scrolls items, click enters an item (or runs an
 *     action item like switching screens)
 *   - editing:     encoder changes the item's value, click exits editing */
void synth_ui_menu_toggle(void);
bool synth_ui_menu_is_active(void);
bool synth_ui_menu_handle_encoder(long delta); /* true if consumed */
bool synth_ui_menu_handle_button(void);        /* true if consumed */

/* Projects-page rename editor: while a project name is being typed, MY_BUTTON_1
 * saves it and MY_BUTTON_2 discards it (so the user never has to walk the cursor
 * to the end of the field or dial the '#' sentinel). These belong to the menu
 * overlay, which composes the projects module's rename state with its own page
 * tracking - so _active() is authoritative (false unless the menu is open on the
 * projects page mid-rename) and the button dispatch can gate on it without any
 * cross-module stale-flag cleanup. _active() also drives the naming-aware hint
 * labels. All three are safe no-ops when CONFIG_SYNTH_PROJECT_STORE is off. */
bool synth_ui_menu_rename_active(void);
void synth_ui_menu_rename_save(void);
void synth_ui_menu_rename_discard(void);

/* True while the menu overlay is open on its Wireless page. The ADSR/filter
 * editors bind to the BLE MIDI live-play voice on this rather than a ui_mode
 * (the page is an overlay, so the mode underneath is the screen the user came
 * from), and the SHIFT+1 open chord uses it to allow the editor from a page
 * rather than only from a mode screen. Stays true while an editor draws over
 * the overlay. Always false when CONFIG_SYNTH_WIRELESS is off. */
bool synth_ui_wireless_page_is_open(void);

/* ── Arp screen ──────────────────────────────────────────────────────────
 * Active when seq_state.ui_mode == UI_MODE_ARP and no overlay is up. */
bool synth_ui_arp_is_active(void);
void synth_ui_arp_handle_encoder(long delta);
void synth_ui_arp_handle_button(void);
/* Cycle the arp's own patch (hold+turn gesture on the arp screen). */
void synth_ui_arp_cycle_patch(int delta);
/* Live-play slot patch (Wireless menu page); defined under CONFIG_SYNTH_WIRELESS. */
void synth_ui_cycle_live_patch(int delta);

/* ── Drone screen ────────────────────────────────────────────────────────
 * Standalone "stutter house drone" synth (custompatches/drone_core). Active
 * when seq_state.ui_mode == UI_MODE_DRONE and no overlay is up. A simple
 * scrollable parameter list; encoder scrolls rows, encoder-click toggles edit,
 * encoder turns the value. */
bool synth_ui_drone_is_active(void);
void synth_ui_drone_handle_encoder(long delta);
void synth_ui_drone_handle_button(void);
/* Cycle the drone's PATCH-mode preset (hold+turn gesture on the drone screen). */
void synth_ui_drone_cycle_patch(int delta);

/* ── Normal drone screen ─────────────────────────────────────────────────
 * Free-running drone (custompatches/drone_std_core). Active when
 * seq_state.ui_mode == UI_MODE_DRONE_STD and no overlay is up. Same list
 * model as the stutter screen; its STUTTER row dives to UI_MODE_DRONE. */
bool synth_ui_drone_std_is_active(void);
void synth_ui_drone_std_handle_encoder(long delta);
void synth_ui_drone_std_handle_button(void);
void synth_ui_drone_std_cycle_patch(int delta);

/* Chord-progression screen — active when seq_state.ui_mode == UI_MODE_PROG and no
 * overlay (menu/graph) is up.  Returns false if not active (caller should fall through). */
bool synth_ui_prog_is_active(void);
bool synth_ui_prog_handle_encoder(int delta);
bool synth_ui_prog_handle_button(void);
bool synth_ui_prog_add_entry(void);
bool synth_ui_prog_delete_entry(void);

/* Track Options screen — per-track repeat rate + per-layer manual chord mode.
 * Active when seq_state.ui_mode == UI_MODE_TRACKOPTS and no overlay is up. */
bool synth_ui_trackopts_is_active(void);
bool synth_ui_trackopts_handle_encoder(int delta);
bool synth_ui_trackopts_handle_button(void);

/* FM/ALGO voice editor — algorithm + per-operator ratio/level for the live
 * SEQ_PATCH_FM_CUSTOM voice (see custompatches/fm_voice.h). Active when
 * seq_state.ui_mode == UI_MODE_FM and no overlay is up. Reached via the menu
 * ("Screen: FM"). Encoder scrolls rows / edits the entered row's value;
 * encoder-click toggles edit on the focused row. */
bool synth_ui_fm_is_active(void);
bool synth_ui_fm_handle_encoder(int delta);
bool synth_ui_fm_handle_button(void);

/* Re-impose the cached global FX (EQ/echo/chorus/reverb) after a synth patch
 * load. Every AMY built-in Juno patch ends with global EQ/chorus commands, so
 * loading a preset onto any synth would otherwise re-skin the whole mix's FX.
 * The sequencer/arp/drone patch-load paths call this immediately after loading;
 * it is a no-op while the user has enabled the "Preset FX" menu toggle (i.e.
 * deliberately letting presets drive the global FX).
 * Declared in amy_fx.h (canonical) — include that header directly. */

/* Accessors for the (module-private) UI state. seq_state itself is static in
 * synth_ui.c — other modules read what they need through these getters rather
 * than reaching into the UI struct. */
uint16_t seq_get_bpm(void);
uint8_t  seq_get_active_layer_idx(void);

/* ── Graph pop-up integration (isolated, easily removable) ───────────────────
 * Demo hooks for the reusable graph_popup widget. main.c calls these; the
 * pop-up state and all U8g2 plumbing live inside synth_ui.c. Removing these
 * declarations and their callers fully reverts the integration. */

/* True while the graph pop-up overlay is open. */
bool synth_ui_graph_is_active(void);

/* Open the curve editor seeded from the current melodic ADSR envelope. */
void synth_ui_graph_open_envelope(void);

/* Route input to the pop-up while it is active. Each returns true if the
 * pop-up consumed the event (caller should then skip normal sequencer input).
 * synth_ui_graph_handle_button(is_long): is_long=true => long-press/cancel. */
bool synth_ui_graph_handle_encoder(long delta);
bool synth_ui_graph_handle_button(bool is_long);

/* Commit the current edits and close the editor (encoder long-press path,
 * symmetric with the long-press that opens it). Distinct from
 * synth_ui_graph_handle_button(true), which discards on cancel. */
bool synth_ui_graph_close_commit(void);

/* Toggle the graph time range SHORT(2s linear) <-> LONG(15s, log-squashed tail)
 * while the editor is open. Re-seeds the curve. Returns true if consumed.
 * NOTE: range is now auto-switched based on total envelope time; this function
 * is kept for completeness but MY_BUTTON_2 no longer calls it. */
bool synth_ui_graph_toggle_range(void);

/* Toggle amp-edit mode while the graph editor is open (MY_BUTTON_2). When
 * active the encoder adjusts the selected target's amplitude trim (0..1)
 * instead of moving ADSR points. Mode and scratch value are committed on
 * editor close (confirm) and reset on every editor open. */
void synth_ui_graph_toggle_amp_mode(void);

/* Flip the sign of the melodic EG1->cutoff sweep depth — MY_BUTTON_SHOULDER
 * while the envelope editor's EG1 page is showing. No-op on the
 * EG0 page, for non-melodic targets, and at 0.0 depth. */
void synth_ui_graph_flip_eg1_polarity(void);

/* Cycle the shown EG's curve type Normal->Linear->DX7->TrueExp (AMY eg_type
 * 0..3) — MY_BUTTON_1 while the envelope editor is open. Applies to AMY
 * immediately, honoring the current apply scope; A/D/S/R times are untouched.
 * (The apply-scope toggle that used to be on MY_BUTTON_1 moved to SHIFT+1.) */
void synth_ui_graph_cycle_eg_type(void);

/* ── Filter editor (per-synth LPF/HPF/BPF/LPF24 curve editor) ───────────────
 * Opened by long-press encoder (same as ADSR); toggled with MY_BUTTON_3 while
 * either editor is open. Controls: encoder adjusts the selected parameter
 * (cutoff/resonance/type), short press cycles the cursor, long-press commits. */
bool synth_ui_filter_is_active(void);
void synth_ui_filter_open(void);
bool synth_ui_filter_handle_encoder(long delta);
bool synth_ui_filter_handle_button(bool is_long);
bool synth_ui_filter_close_commit(void);

/* Toggle the filter on/off (MY_BUTTON_1 while editor is open). No-op when closed. */
void synth_ui_filter_toggle_enabled(void);

/* ── LFO editor (per-track tempo-synced modulator) ─────────────────────────
 * Opened as the third tab in the ADSR→Filter→LFO cycle (MY_BUTTON_3).
 * Controls: encoder scrolls cursor / adjusts field (short press to toggle);
 * encoder long-press commits; MY_BUTTON_0 long-press cancels. */
bool synth_ui_lfo_is_active(void);
void synth_ui_lfo_open(void);
bool synth_ui_lfo_handle_encoder(long delta);
bool synth_ui_lfo_handle_button(bool is_long);
bool synth_ui_lfo_close_commit(void);

/* Toggle whether effect-editor commits apply to only the selected track (false)
 * or all tracks in the active layer (true).  Consumed by MY_BUTTON_1 while the
 * ADSR graph or LFO editor is open.  Returns true when an editor was active. */
bool synth_ui_toggle_editor_apply_scope(void);

/* Cycle between ADSR, Filter, and LFO editors (MY_BUTTON_3 while any is open).
 * Commits the departing editor and opens the next one.  Replaces the old
 * synth_ui_toggle_adsr_filter() two-way swap. */
void synth_ui_cycle_editor(void);

/* ── Step Trig editor (per-step probability / ratchet / conditional trig) ──
 * Full-screen popup addressed by the sequencer grid's existing cursor
 * (active layer / selected track / selected step) — no separate cursor.
 * Opened/closed by MY_BUTTON_2 long-press while navigating the grid
 * (main.c); while open, encoder turns adjust the focused field and a short
 * encoder-button press cycles which field (Prob → Ratchet → Cond → Param)
 * is focused. */
bool synth_ui_stepedit_is_active(void);
void synth_ui_stepedit_open(void);
void synth_ui_stepedit_close(void);
bool synth_ui_stepedit_handle_encoder(long delta);
bool synth_ui_stepedit_handle_button(void);

/* ─── Canonical view precedence — the single source of truth ─────────────
 * The "which screen/overlay is showing" decision used to be re-derived by a
 * hand-copied precedence cascade in four places (draw switch, hint strip,
 * and the two main.c input routers). ui_active_view() is now the ONLY place
 * that precedence lives; every consumer resolves once and dispatches on the
 * result, so input and draw can never disagree about the active view.
 *
 * Order (high → low): FILTER > LFO > STEPEDIT > GRAPH > MENU > mode-tail.
 * The first five are the input-capturing overlays (UI_VIEW_IS_OVERLAY); the
 * mode-tail is resolved from seq_state.ui_mode when no overlay is up. */
typedef enum {
    UI_VIEW_FILTER = 0,
    UI_VIEW_LFO,
    UI_VIEW_STEPEDIT,
    UI_VIEW_GRAPH,
    UI_VIEW_MENU,
    UI_VIEW_ARP,
    UI_VIEW_DRONE_VIS,
    UI_VIEW_DRONE,
    UI_VIEW_DRONE_STD,
    UI_VIEW_PROG,
    UI_VIEW_TRACKOPTS,
    UI_VIEW_FM,
    UI_VIEW_SEQ,
    UI_VIEW_COUNT
} ui_view_id_t;

/* True for the five leading overlays, which capture encoder/button input
 * outright ahead of any mode screen. */
#define UI_VIEW_IS_OVERLAY(v)  ((v) <= UI_VIEW_MENU)

ui_view_id_t synth_ui_active_view(void);

#ifdef __cplusplus
}
#endif

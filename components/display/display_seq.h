#pragma once

#include "u8g2.h"
#include "seq_model.h"      /* engine-owned data model: seq_layer_t, seq_env_t, SEQ_* */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Top-level screen the UI is showing ── */
typedef enum {
    UI_MODE_SEQUENCER = 0,
    UI_MODE_ARP       = 1,
    UI_MODE_DRONE     = 2,
    UI_MODE_PROG      = 3,
    UI_MODE_TRACKOPTS = 4,
    UI_MODE_FM        = 5,
} ui_mode_t;

/* ── Global sequencer display/UI state ── */
typedef struct {
    seq_layer_t layers[MAX_LAYERS];
    uint8_t     num_layers;
    uint8_t     active_layer_idx;
    uint8_t     current_pattern;
    uint8_t     current_step;       /* 0 .. (active layer num_steps - 1) */
    bool        playing;
    uint8_t     selected_track;     /* 0 .. SEQ_TRACKS-1                 */
    uint8_t     selected_step;      /* 0 .. (active layer num_steps - 1) */
    bool        edit_mode;
    bool        drum_select_mode;   /* true while note-select btn held   */
    bool        patch_select_mode;  /* true while patch-select btn held  */
    bool        drum_pcm;           /* true = drum engine is PCM: drum row
                                       labels/banner show PCM presets, not
                                       patches. Mirror of the core engine,
                                       refreshed by seq_view_signature().  */

    /* ── Screen + menu overlay ── */
    ui_mode_t   ui_mode;            /* which top-level screen is active   */
    bool        menu_open;          /* true while the menu overlay is up  */
    uint8_t     menu_cursor;        /* highlighted menu item index        */
    bool        menu_editing;       /* true while editing the entered item*/
} display_seq_state_t;

/**
 * @brief Draw one full sequencer frame from the provided state.
 * @param bpm  Current BPM value, passed explicitly (not stored in state).
 */
void display_seq_draw_frame(u8g2_t *u8g2, const display_seq_state_t *state, uint16_t bpm);

#ifdef __cplusplus
}
#endif

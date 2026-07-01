#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "synth_ui.h"          /* synth_ui_state_t, seq_layer_type_t */
#include "display_drone.h"     /* drone_view_t */
#include "display_prog.h"      /* prog_view_t */
#include "display_trackopts.h" /* trackopts_view_t */
#include "display_menu.h"      /* menu_view_t */
#include "display_arp.h"       /* arp_view_t */
#include "display_stepedit.h"  /* stepedit_view_t */
#include "u8g2.h"              /* u8g2_t */

/* ─── Cross-file shared state (owners noted; only these need extern) ──── */
extern synth_ui_state_t  seq_state;        /* owner: synth_ui_state.c */
extern volatile bool     s_force_redraw;   /* owner: synth_ui_task.c */
extern uint8_t           s_graph_layer;    /* owner: ui_editors.c; task clamps it */
extern uint8_t           s_graph_track;    /* owner: ui_editors.c; task clamps it */
extern bool              s_filter_active;  /* owner: ui_editors.c; task reads for cascade */
extern bool              s_lfo_active;     /* owner: ui_editors.c; task reads for cascade */
extern bool              s_drone_vis_open; /* owner: ui_screen_drone.c; task reads for V_DRONE_VIS */
extern uint8_t           s_to_layer;       /* owner: ui_screen_trackopts.c; task + menu write it */
extern uint8_t           s_to_track;       /* owner: ui_screen_trackopts.c; menu sets it */

/* NOTE: s_to_cursor and s_to_editing are trackopts-internal — NOT extern. */

/* ─── FNV-1a render-on-change (all view signature functions use this) ── */
#define FNV1A_OFFSET 2166136261u
#define FNV1A_PRIME  16777619u
[[gnu::const]] static inline uint32_t fnv1a_bytes(uint32_t h,
                                                    const void *data, size_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) { h ^= b[i]; h *= FNV1A_PRIME; }
    return h;
}

/* ─── Shared private helpers ─────────────────────────────────────────── */
void     ui_note_name(uint8_t midi_note, char buf[4]);
void     sync_layer_to_core(uint8_t li);

/* ─── View signatures (each defined in its screen/editor file) ────────── */
uint32_t seq_view_signature(void);
uint32_t graph_view_signature(void);
uint32_t filter_view_signature(void);
uint32_t lfo_view_signature(void);
uint32_t arp_view_signature(void);
uint32_t menu_view_signature(void);
uint32_t drone_view_signature(void);
uint32_t prog_view_signature(void);
uint32_t trackopts_view_signature(void);
uint32_t stepedit_view_signature(void);
uint32_t fm_view_signature(void);

/* ─── Build-view helpers called from synth_ui_task draw switch ────────── */
void     drone_build_view(drone_view_t *out);
void     prog_build_view(prog_view_t *out);
void     trackopts_build_view(trackopts_view_t *out);
void     menu_build_view(menu_view_t *out);
void     arp_build_view(arp_view_t *out);
void     stepedit_build_view(stepedit_view_t *out);
void     fm_build_view(menu_view_t *out);

/* ─── Draw wrappers (encapsulate private s_fgraph/s_lfo_view/s_graph_popup) */
void     synth_ui_graph_view_draw(u8g2_t *u8g2);
void     synth_ui_filter_view_draw(u8g2_t *u8g2);
void     synth_ui_lfo_view_draw(u8g2_t *u8g2);
/* NOTE: graph_draw_topbar is static in ui_editors.c — only synth_ui_graph_view_draw
 *       calls it. Do NOT forward-declare it here. */

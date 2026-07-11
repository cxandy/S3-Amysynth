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

/* Re-sync the UI mirror (seq_state) from the audio core after a bulk
 * out-of-band change to layer topology/content (project load): copies every
 * core layer's persistable state into seq_state.layers[], resets the
 * transport and cursor state to a safe idle default, and forces one redraw.
 * Does not touch the core itself. Applier-task only (synth_ui_task). */
void     synth_ui_reload_mirror_from_core(void);

/* ─── View signatures (each defined in its screen/editor file) ──────────
 * The view-struct screens build the view once and return it through `out`
 * alongside the hash, so the task's draw switch reuses it instead of
 * running the whole snprintf build a second time in the same frame. */
uint32_t seq_view_signature(void);
uint32_t graph_view_signature(void);
uint32_t filter_view_signature(void);
uint32_t lfo_view_signature(void);
uint32_t arp_view_signature(arp_view_t *out);
uint32_t menu_view_signature(menu_view_t *out);
uint32_t drone_view_signature(drone_view_t *out);
uint32_t prog_view_signature(prog_view_t *out);
uint32_t trackopts_view_signature(trackopts_view_t *out);
uint32_t stepedit_view_signature(stepedit_view_t *out);
uint32_t fm_view_signature(menu_view_t *out);

/* ─── View descriptor table (draw + hint), defined in ui_view_resolve.c ──
 * One scratch union holds whichever view struct the active screen builds;
 * signature() fills it and returns the FNV hash, draw() reuses it. The table
 * is indexed by synth_ui_active_view() so the draw switch and the hint strip
 * share the single precedence resolver instead of re-deriving it. */
typedef union {
    menu_view_t      menu;       /* MENU and FM */
    arp_view_t       arp;
    drone_view_t     drone;      /* DRONE and DRONE_VIS */
    prog_view_t      prog;
    trackopts_view_t trackopts;
    stepedit_view_t  stepedit;
} ui_view_vw_t;

typedef struct {
    const char *name;
    uint32_t  (*signature)(ui_view_vw_t *vw);   /* builds vw, returns FNV hash */
    void      (*draw)(u8g2_t *g, ui_view_vw_t *vw);
    /* Button-hint labels. A NULL static label means "compute dynamically",
     * in which case the matching *_fn is called (only LFO/GRAPH b1 and
     * LFO/GRAPH b2 depend on state the view id does not carry). b3 is always
     * static. */
    const char *b1, *b2, *b3;
    const char *(*b1_fn)(void);
    const char *(*b2_fn)(void);
} ui_view_desc_t;

extern const ui_view_desc_t ui_view_table[UI_VIEW_COUNT];

/* GRAPH b2 hint (owner: ui_editors.c) — "Amp" on EG0, "Env" on melodic EG1. */
const char *synth_ui_graph_hint_b2(void);

/* ─── Build-view helpers called from synth_ui_task draw switch ────────── */
void     drone_build_view(drone_view_t *out);
void     prog_build_view(prog_view_t *out);
void     trackopts_build_view(trackopts_view_t *out);
void     menu_build_view(menu_view_t *out);
void     arp_build_view(arp_view_t *out);
void     stepedit_build_view(stepedit_view_t *out);
void     fm_build_view(menu_view_t *out);

/* ─── Global-FX submenu page (item model in ui_screen_fxmenu.c; the page
 *     state and input routing live in ui_screen_menu.c) ────────────────── */
const menu_item_view_t *fx_menu_build_items(void);
uint8_t  fx_menu_item_count(void);
bool     fx_menu_item_is_value(uint8_t idx);
bool     fx_menu_item_is_back(uint8_t idx);
void     fx_menu_edit_value(uint8_t idx, int delta);
const char *menu_page_title(void);   /* header-bar title for the active page */

/* ─── Projects storage page (item model in ui_screen_projects.c; page state
 *     and input routing live in ui_screen_menu.c). Declared unconditionally
 *     (mirroring the FM screen's prototypes) — the implementation compiles
 *     to nothing and these go unused when CONFIG_SYNTH_PROJECT_STORE is off,
 *     since every call site is itself guarded by that symbol. ─────────── */
const menu_item_view_t *projects_menu_build_items(void);
uint8_t  projects_menu_item_count(void);
bool     projects_menu_item_is_back(uint8_t idx);
bool     projects_menu_item_is_value(uint8_t idx);
bool     projects_menu_handle_click(uint8_t idx);
void     projects_menu_edit_value(uint8_t idx, int delta);
void     projects_menu_reset(void);
void     projects_menu_service(void);   /* drains the deferred load/save */

/* ─── Draw wrappers (encapsulate private s_fgraph/s_lfo_view/s_graph_popup) */
void     synth_ui_graph_view_draw(u8g2_t *u8g2);
void     synth_ui_filter_view_draw(u8g2_t *u8g2);
void     synth_ui_lfo_view_draw(u8g2_t *u8g2);
/* NOTE: graph_draw_topbar is static in ui_editors.c — only synth_ui_graph_view_draw
 *       calls it. Do NOT forward-declare it here. */

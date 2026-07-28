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
 * Contract: signature functions are side-effect-free. The task's redraw gate
 * calls one per frame and compares the hash against the previous frame's to
 * decide whether to rebuild the screen; any state mutation belongs in the
 * live-service hooks that run before the gate, never in a signature.
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
uint32_t drone_std_view_signature(drone_view_t *out);
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
    /* Preferred X of the BLE session badge plate (7 px wide, top row) for this
     * view, chosen per screen to sit just right of its left header label - the
     * top-right corner belongs to editor value readouts and CLIP/LOUD. Only a
     * hint: display_badge_draw() keeps the badge off lit pixels regardless.
     * 0 = no preference, let the placement probe choose. */
    uint8_t badge_x;
} ui_view_desc_t;

extern const ui_view_desc_t ui_view_table[UI_VIEW_COUNT];

/* GRAPH b2 hint (owner: ui_editors.c) — "Amp" on EG0, "Env" on melodic EG1. */
const char *synth_ui_graph_hint_b2(void);

/* ─── Build-view helpers called from synth_ui_task draw switch ────────── */
void     drone_build_view(drone_view_t *out);
void     drone_std_build_view(drone_view_t *out);
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
bool     fx_menu_item_is_notefx(uint8_t idx);  /* dive row into the NoteFX page */
void     fx_menu_edit_value(uint8_t idx, int delta);
const char *menu_page_title(void);   /* header-bar title for the active page */

/* ─── NoteFX page: per-layer melodic gate + glide (item model in
 *     ui_screen_notefx.c; page state and input routing live in
 *     ui_screen_menu.c). Reached from a dive row on the global-FX page. ──── */
const menu_item_view_t *notefx_menu_build_items(void);
uint8_t  notefx_menu_item_count(void);
bool     notefx_menu_item_is_value(uint8_t idx);
bool     notefx_menu_item_is_back(uint8_t idx);
void     notefx_menu_edit_value(uint8_t idx, int delta);

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
/* Rename-editor primitives, composed by the menu overlay's public
 * synth_ui_menu_rename_* hooks (which add the page-state gating). */
bool     projects_menu_is_renaming(void);
void     projects_menu_rename_commit(void);
void     projects_menu_rename_cancel(void);

/* ─── Wireless page: BLE MIDI session control (item model in
 *     ui_screen_wireless.c; page state and input routing live in
 *     ui_screen_menu.c). Declared unconditionally like the Projects page -
 *     every call site is guarded by CONFIG_SYNTH_WIRELESS. ─────────────── */
const menu_item_view_t *wireless_menu_build_items(void);
uint8_t  wireless_menu_item_count(void);
bool     wireless_menu_item_is_back(uint8_t idx);
bool     wireless_menu_item_is_value(uint8_t idx);
bool     wireless_menu_handle_click(uint8_t idx);
void     wireless_menu_edit_value(uint8_t idx, int delta);
void     wireless_menu_reset(void);
/* synth_ui_wireless_page_is_open() (public, synth_ui.h) is defined in
 * ui_screen_menu.c alongside the page state; the editors and main.c's shift
 * chord both bind the live-play voice on it instead of a ui_mode. */

/* ─── Chords page: chord-preset editor (item model in ui_screen_chords.c;
 *     page state and input routing live in ui_screen_menu.c). Slot list +
 *     per-slot edit view; every edit commits through seq_chords_set and
 *     auditions on the selected melodic track. ──────────────────────────── */
const menu_item_view_t *chords_menu_build_items(void);
uint8_t  chords_menu_item_count(void);
bool     chords_menu_item_is_back(uint8_t idx);
bool     chords_menu_item_is_value(uint8_t idx);
bool     chords_menu_handle_click(uint8_t idx);
void     chords_menu_edit_value(uint8_t idx, int delta);
void     chords_menu_reset(void);
const char *chords_menu_title(void);

/* Editor live-preview service: flushes any pending throttled apply (currently
 * only the graph editor's amp trim, whose melodic apply re-emits the track's
 * steps). Called from synth_ui_task's 50 ms loop; no-op when no editor is
 * open or nothing is pending. */
void     synth_ui_editors_live_service(void);

/* ─── Draw wrappers (encapsulate private s_fgraph/s_lfo_view/s_graph_popup) */
void     synth_ui_graph_view_draw(u8g2_t *u8g2);
void     synth_ui_filter_view_draw(u8g2_t *u8g2);
void     synth_ui_lfo_view_draw(u8g2_t *u8g2);
/* NOTE: graph_draw_topbar is static in ui_editors.c — only synth_ui_graph_view_draw
 *       calls it. Do NOT forward-declare it here. */

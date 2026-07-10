#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "custompatches/drone_core.h"
#include "sdkconfig.h"

/* ════════════════════════════════════════════════════════════════════════
 *  Single source of view precedence + the descriptor table over it
 * ════════════════════════════════════════════════════════════════════════
 * synth_ui_active_view() is the ONE place the screen/overlay precedence lives
 * (it used to be hand-copied into the draw switch, the hint strip, and both
 * main.c input routers). Every consumer calls it, indexes ui_view_table[], and
 * dispatches — so input and draw always resolve the same view. This mirrors the
 * former draw cascade (synth_ui_task.c) branch-for-branch, and depends on the
 * Phase-1 gate inside synth_ui_stepedit_is_active(): the STEPEDIT branch is only
 * correct because that accessor now self-gates to the sequencer screen. */

ui_view_id_t synth_ui_active_view(void)
{
    if (s_filter_active)                return UI_VIEW_FILTER;
    if (s_lfo_active)                   return UI_VIEW_LFO;
    if (synth_ui_stepedit_is_active())  return UI_VIEW_STEPEDIT; /* gated, Phase 1 */
    if (synth_ui_graph_is_active())     return UI_VIEW_GRAPH;
    if (seq_state.menu_open)            return UI_VIEW_MENU;
    switch (seq_state.ui_mode) {
        case UI_MODE_ARP:       return UI_VIEW_ARP;
        case UI_MODE_DRONE:     return s_drone_vis_open ? UI_VIEW_DRONE_VIS : UI_VIEW_DRONE;
        case UI_MODE_PROG:      return UI_VIEW_PROG;
        case UI_MODE_TRACKOPTS: return UI_VIEW_TRACKOPTS;
#if CONFIG_SYNTH_CUSTOM_FM
        case UI_MODE_FM:        return UI_VIEW_FM;
#endif
        default:                return UI_VIEW_SEQ;
    }
}

/* ─── Signature thunks (build vw once, return the FNV render-gate hash) ─── */
static uint32_t sig_filter(ui_view_vw_t *vw)    { (void)vw; return filter_view_signature(); }
static uint32_t sig_lfo(ui_view_vw_t *vw)       { (void)vw; return lfo_view_signature(); }
static uint32_t sig_stepedit(ui_view_vw_t *vw)  { return stepedit_view_signature(&vw->stepedit); }
static uint32_t sig_graph(ui_view_vw_t *vw)     { (void)vw; return graph_view_signature(); }
static uint32_t sig_menu(ui_view_vw_t *vw)      { return menu_view_signature(&vw->menu); }
static uint32_t sig_arp(ui_view_vw_t *vw)       { return arp_view_signature(&vw->arp); }
static uint32_t sig_drone(ui_view_vw_t *vw)     { return drone_view_signature(&vw->drone); }
static uint32_t sig_prog(ui_view_vw_t *vw)      { return prog_view_signature(&vw->prog); }
static uint32_t sig_trackopts(ui_view_vw_t *vw) { return trackopts_view_signature(&vw->trackopts); }
static uint32_t sig_seq(ui_view_vw_t *vw)       { (void)vw; return seq_view_signature(); }

/* ─── Draw thunks (reuse the vw the signature already built) ───────────── */
static void draw_filter(u8g2_t *g, ui_view_vw_t *vw)    { (void)vw; synth_ui_filter_view_draw(g); }
static void draw_lfo(u8g2_t *g, ui_view_vw_t *vw)       { (void)vw; synth_ui_lfo_view_draw(g); }
static void draw_stepedit(u8g2_t *g, ui_view_vw_t *vw)  { display_stepedit_draw_frame(g, &vw->stepedit); }
static void draw_graph(u8g2_t *g, ui_view_vw_t *vw)     { (void)vw; synth_ui_graph_view_draw(g); }
static void draw_menu(u8g2_t *g, ui_view_vw_t *vw)      { display_menu_draw_frame_titled(g, menu_page_title(), &vw->menu); }
static void draw_arp(u8g2_t *g, ui_view_vw_t *vw)       { display_arp_draw_frame(g, &vw->arp); }
static void draw_drone(u8g2_t *g, ui_view_vw_t *vw)     { display_drone_draw_frame(g, &vw->drone); }
static void draw_prog(u8g2_t *g, ui_view_vw_t *vw)      { display_prog_draw_frame(g, &vw->prog); }
static void draw_trackopts(u8g2_t *g, ui_view_vw_t *vw) { display_trackopts_draw_frame(g, &vw->trackopts); }
static void draw_seq(u8g2_t *g, ui_view_vw_t *vw)       { (void)vw; display_seq_draw_frame(g, &seq_state, seq_get_bpm()); }

/* DRONE_VIS composites the sweep/amp visualiser from the live drone getters
 * rather than a prebuilt view struct (the signature still uses drone_view for
 * render-gating). Lifted verbatim from the former draw switch. */
static void draw_drone_vis(u8g2_t *g, ui_view_vw_t *vw)
{
    (void)vw;
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
    static const uint8_t pat_masks[] = { 0xFF, 0x55, 0xAA, 0x5B, 0x51 };
    drone_pattern_t pat = drone_get_pattern();
    if ((size_t)pat < sizeof(pat_masks))
        dvis.pattern_mask = pat_masks[pat];
    display_drone_vis_draw(g, &dvis);
}

#if CONFIG_SYNTH_CUSTOM_FM
static uint32_t sig_fm(ui_view_vw_t *vw)  { return fm_view_signature(&vw->menu); }
static void     draw_fm(u8g2_t *g, ui_view_vw_t *vw) { display_menu_draw_frame_titled(g, "FM ALGO", &vw->menu); }
#endif

/* ─── Dynamic hint labels (state the view id does not carry) ─────────────
 * Reproduces the former synth_ui_hint.c ladder for exactly the cells that
 * varied on ui_mode: the scope/patch b1 of the two scope-capable editors, and
 * the LFO editor's b2 (which fell through to the underlying screen). */
static const char *hint_scope_or_patch(void)   /* LFO + GRAPH b1 */
{
    return (seq_state.ui_mode != UI_MODE_ARP && seq_state.ui_mode != UI_MODE_DRONE)
         ? "Scope" : "Patch";
}
static const char *hint_lfo_b2(void)
{
    switch (seq_state.ui_mode) {
        case UI_MODE_ARP:
        case UI_MODE_DRONE:     return "-";
        case UI_MODE_PROG:      return "+Add";
        case UI_MODE_TRACKOPTS: return "DelLy";
        default:                return "Pitch";
    }
}

/* ─── The table: one row per view, indexed by ui_view_id_t ─────────────── */
const ui_view_desc_t ui_view_table[UI_VIEW_COUNT] = {
    [UI_VIEW_FILTER]    = { "FILTER", sig_filter,    draw_filter,    "On/Off", "-",     "Next",  NULL,               NULL },
    [UI_VIEW_LFO]       = { "LFO",    sig_lfo,       draw_lfo,       NULL,     NULL,    "Next",  hint_scope_or_patch, hint_lfo_b2 },
    [UI_VIEW_STEPEDIT]  = { "STEP",   sig_stepedit,  draw_stepedit,  "Patch",  "-",     "Close", NULL,               NULL },
    [UI_VIEW_GRAPH]     = { "GRAPH",  sig_graph,     draw_graph,     NULL,     "Amp",   "Next",  hint_scope_or_patch, NULL },
    [UI_VIEW_MENU]      = { "MENU",   sig_menu,      draw_menu,      "Patch",  "Pitch", "Menu",  NULL,               NULL },
    [UI_VIEW_ARP]       = { "ARP",    sig_arp,       draw_arp,       "Patch",  "-",     "Menu",  NULL,               NULL },
    [UI_VIEW_DRONE_VIS] = { "DRONEV", sig_drone,     draw_drone_vis, "Patch",  "-",     "Menu",  NULL,               NULL },
    [UI_VIEW_DRONE]     = { "DRONE",  sig_drone,     draw_drone,     "Patch",  "-",     "Menu",  NULL,               NULL },
    [UI_VIEW_PROG]      = { "PROG",   sig_prog,      draw_prog,      "Del",    "+Add",  "Menu",  NULL,               NULL },
    [UI_VIEW_TRACKOPTS] = { "TRKOPT", sig_trackopts, draw_trackopts, "Patch",  "DelLy", "Menu",  NULL,               NULL },
#if CONFIG_SYNTH_CUSTOM_FM
    [UI_VIEW_FM]        = { "FM",     sig_fm,        draw_fm,        "Patch",  "Pitch", "Menu",  NULL,               NULL },
#else
    [UI_VIEW_FM]        = { "FM",     NULL,          NULL,           "Patch",  "Pitch", "Menu",  NULL,               NULL },
#endif
    [UI_VIEW_SEQ]       = { "SEQ",    sig_seq,       draw_seq,       "Patch",  "Pitch", "Menu",  NULL,               NULL },
};

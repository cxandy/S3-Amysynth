#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "graph_popup.h"
#include "filter_graph.h"
#include "display_lfo.h"
#include "seq_defaults.h"
#include "amy_helpers.h"
#include "seq_clamp.h"
#include "esp_log.h"
#include <math.h>
#include <string.h>

static const char *TAG = "synth_ui";

/* ── Graph pop-up integration (isolated, easily removable) ───────────────────
 * Everything between this block and the matching "graph pop-up: end" marker is
 * the demo wiring for the reusable graph_popup widget. Deleting this block plus
 * the single gated branch in synth_ui_task() and the three public
 * synth_ui_graph_* entry points fully restores the original behaviour.
 *
 * The melodic envelope defaults come from seq_defaults.h, then map to/from the
 * widget via the AMY adapter. */

static gpopup_t s_graph_popup;
static bool     s_graph_popup_inited = false;

/* Which (layer,track) the open editor is bound to, captured at open time so the
 * write-back targets the same row even if the selection moves underneath. */
uint8_t  s_graph_layer = 0;
uint8_t  s_graph_track = 0;

/* Which backend the open editor edits. Captured at open time so seed/commit
 * route to the right envelope store (melodic row, the drone, or the arp). The
 * same widget + range mapping + commit math serve all three. */
typedef enum {
    GRAPH_TGT_MELODIC = 0,
    GRAPH_TGT_DRONE   = 1,
    GRAPH_TGT_ARP     = 2,
} graph_target_t;
static graph_target_t s_graph_target = GRAPH_TGT_MELODIC;

/* Which of the target's two independent AMY breakpoint generators the open
 * editor is showing/editing. 0 = EG0 (amp, the historical default), 1 = EG1
 * (typically the filter sweep — see sequencer_core_push_envelope_eg1()).
 * Reset to 0 on every editor open; toggled by MY_BUTTON_3 long-press while
 * the editor is open (synth_ui_graph_toggle_eg_index()). */
static uint8_t s_graph_eg_index = 0;

/* ── Time-range mapping ──────────────────────────────────────────────────────
 * The graph X axis is normalised 0..1. We map it to absolute milliseconds with
 * a switchable full-width budget so the same 3-point editor serves both plucky
 * short notes and long pads.
 *   SHORT: 0..2000 ms, LINEAR  (fine control where short notes live)
 *   LONG : 0..15000 ms, LOG-SQUASHED (the long right-hand tail is compressed so
 *          the musically-interesting early portion keeps most of the pixels). */
#define GRAPH_RANGE_SHORT_MS 2000u
#define GRAPH_RANGE_LONG_MS  15000u
/* Curvature of the long-view squash: larger = more compression of the tail. */
#define GRAPH_LONG_SQUASH    9.0f

static bool s_graph_long_range = false;   /* false = SHORT, true = LONG (auto-switched) */

/* Graph editor amp-mode state (MY_BUTTON_2 while ADSR editor is open).
 * s_graph_amp_edit holds the scratch trim value during editing; it is committed
 * to the target on close. Shown in the topbar right slot (replaces "S/L" flag
 * which is now set automatically based on envelope duration). */
static bool  s_graph_amp_mode = false;
static float s_graph_amp_edit = 1.0f;   /* scratch 0..1, seeds from target on open */
static bool  s_graph_env_dirty = false; /* set only when user moves an ADSR point */

/* Layer-apply scope: when true, effect-editor commits write to all SEQ_TRACKS
 * in the active layer instead of only the selected track.  Toggled by
 * MY_BUTTON_1 while the ADSR graph or LFO editor is open.  The filter editor
 * repurposes MY_BUTTON_1 for enable/disable, so scope is set there or in LFO. */
static bool s_editor_apply_all = false;

/* Normalised X (0..1) -> milliseconds, range/curve aware. */
static uint32_t graph_x_to_ms(float x)
{
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    if (!s_graph_long_range) {
        return (uint32_t)(x * (float)GRAPH_RANGE_SHORT_MS + 0.5f);
    }
    /* Long view: expand the squashed display X back to a linear time fraction.
     * Display compresses with log1p(k*t)/log1p(k); invert it here. */
    float k = GRAPH_LONG_SQUASH;
    float t = (expf(x * logf(1.0f + k)) - 1.0f) / k;   /* 0..1 linear time */
    return (uint32_t)(t * (float)GRAPH_RANGE_LONG_MS + 0.5f);
}

/* Milliseconds -> normalised X (0..1), range/curve aware (inverse of above). */
static float graph_ms_to_x(uint32_t ms)
{
    if (!s_graph_long_range) {
        float x = (float)ms / (float)GRAPH_RANGE_SHORT_MS;
        return x > 1.0f ? 1.0f : x;
    }
    float k = GRAPH_LONG_SQUASH;
    float t = (float)ms / (float)GRAPH_RANGE_LONG_MS;   /* 0..1 linear time */
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    /* Compress: more pixels to small t, fewer to the long tail. */
    return logf(1.0f + k * t) / logf(1.0f + k);
}

/* Yellow top-bar height on the dual-colour panel: rows 0..15 render yellow and
 * are used as the editor's context bar, the plot fills rows 16..63. */
#define GRAPH_TOPBAR_H 16

/* ── Auto-decay rule ─────────────────────────────────────────────────────────
 * The decay TIME is derived, not user-dragged: the sustain point's X is locked
 * (Y-only) in the widget and recomputed here from attack time + sustain level.
 * Lower sustain -> longer, more audible fall; decay also scales gently with
 * attack. Tunable constants; promote to Kconfig later if desired. */
#define DECAY_BASE_MS          120u //note 06-20 testing some params moving up from 40
#define DECAY_ATTACK_K         0.5f
#define DECAY_SUSTAIN_SPAN_MS  400.0f
#define DECAY_MIN_MS           20u
#define DECAY_MAX_MS           2000u

static uint32_t graph_decay_ms(uint32_t attack_ms, float sustain_frac)
{
    if (sustain_frac < 0.0f) sustain_frac = 0.0f;
    if (sustain_frac > 1.0f) sustain_frac = 1.0f;
    float d = (float)DECAY_BASE_MS
            + (float)attack_ms * DECAY_ATTACK_K
            + (1.0f - sustain_frac) * DECAY_SUSTAIN_SPAN_MS;
    if (d < (float)DECAY_MIN_MS) d = (float)DECAY_MIN_MS;
    if (d > (float)DECAY_MAX_MS) d = (float)DECAY_MAX_MS;
    return (uint32_t)(d + 0.5f);
}

static void graph_popup_ensure_init(void)
{
    if (s_graph_popup_inited) return;
    /* Full-screen plot under the yellow context bar (rows 16..63). */
    graph_popup_init(&s_graph_popup, 0, GRAPH_TOPBAR_H, 128,
                     (uint8_t)(64 - GRAPH_TOPBAR_H));
    graph_popup_set_style(&s_graph_popup, GPOPUP_STYLE_ADSR);
    /* Sustain point is Y-only: its X (decay time) is auto-derived here. */
    graph_popup_set_adsr_lock_sx(&s_graph_popup, true);
    s_graph_popup_inited = true;
}

/* Recompute the sustain point's X (decay time) from the current attack time and
 * sustain level, then write it back. Keeps all ms math host-side so the widget
 * stays AMY-agnostic. Expects the standard 4-point ADSR layout. */
static void graph_recompute_decay(void)
{
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    if (n < 4) return;

    uint32_t attack_ms   = graph_x_to_ms(pts[1].x);
    float    sustain_frac = pts[2].y;
    uint32_t decay_ms    = graph_decay_ms(attack_ms, sustain_frac);

    pts[2].x = graph_ms_to_x(attack_ms + decay_ms);
    /* Preserve edit state across the points rewrite. */
    uint8_t saved_cursor  = s_graph_popup.cursor;
    bool    saved_editing = s_graph_popup.editing_value;
    bool    saved_axis_y  = s_graph_popup.adjust_axis_y;
    graph_popup_set_points(&s_graph_popup, pts, n);
    s_graph_popup.cursor        = saved_cursor;
    s_graph_popup.editing_value = saved_editing;
    s_graph_popup.adjust_axis_y = saved_axis_y;
}

/* Push the bottom-margin time tick positions for the active range. Mapped
 * through graph_ms_to_x() so spacing reflects the (non-linear in LONG) axis. */
static void graph_update_ticks(void)
{
    float xs[6];
    uint8_t n = 0;
    if (!s_graph_long_range) {
        /* SHORT (2s linear): 0, 0.5, 1, 1.5, 2 s. */
        static const uint32_t tms[] = { 0, 500, 1000, 1500, 2000 };
        for (uint8_t i = 0; i < 5; ++i) xs[n++] = graph_ms_to_x(tms[i]);
    } else {
        /* LONG (15s log-squashed): 0, 0.1, 0.5, 1, 5, 15 s. */
        static const uint32_t tms[] = { 0, 100, 500, 1000, 5000, 15000 };
        for (uint8_t i = 0; i < 6; ++i) xs[n++] = graph_ms_to_x(tms[i]);
    }
    graph_popup_set_ticks(&s_graph_popup, xs, n);
}

bool synth_ui_graph_is_active(void)
{
    return graph_popup_is_active(&s_graph_popup);
}

/* Seed the editor's 3 points from the stored envelope, applying the current
 * time-range mapping to the X (time) axis. Cumulative time is squashed segment
 * by segment so the curve shape matches what write-back will reconstruct. */
static void graph_seed_from_env(const seq_env_t *env)
{
    uint32_t cum_a = env->attack_ms;
    uint32_t cum_d = env->attack_ms + env->decay_ms;
    uint32_t cum_r = env->attack_ms + env->decay_ms + env->release_ms;

    gpopup_point_t pts[4];
    pts[0].x = 0.0f;                 pts[0].y = 0.0f;                       /* origin     */
    pts[1].x = graph_ms_to_x(cum_a); pts[1].y = 1.0f;                       /* attack peak*/
    pts[2].x = graph_ms_to_x(cum_d); pts[2].y = (float)env->sustain_pct / 100.0f; /* sustain */
    pts[3].x = graph_ms_to_x(cum_r); pts[3].y = 0.0f;                       /* release end*/
    graph_popup_set_points(&s_graph_popup, pts, 4);
    /* Snap the sustain point's X to the auto-decay rule so the opening curve
     * already obeys it (decay time is derived, not whatever was stored). */
    graph_recompute_decay();
}

/* Read the bound target's current envelope into `env`, for eg_index (0=EG0,
 * 1=EG1). Returns false only if the melodic target has no valid row (caller
 * then seeds compile-time defaults). */
static bool graph_read_target_env_idx(seq_env_t *env, uint8_t eg_index)
{
    if (eg_index == 1) {
        switch (s_graph_target) {
            case GRAPH_TGT_DRONE:
                drone_get_envelope2(env);
                return true;
            case GRAPH_TGT_ARP:
                arp_get_envelope2(env);
                return true;
            case GRAPH_TGT_MELODIC:
            default:
                return sequencer_core_get_melodic_envelope2(s_graph_layer,
                                                            s_graph_track, env);
        }
    }
    switch (s_graph_target) {
        case GRAPH_TGT_DRONE:
            drone_get_envelope(env);
            return true;
        case GRAPH_TGT_ARP:
            arp_get_envelope(env);
            return true;
        case GRAPH_TGT_MELODIC:
        default:
            return sequencer_core_get_melodic_envelope(s_graph_layer,
                                                       s_graph_track, env);
    }
}

/* Write `env` back to the bound target's store for eg_index (0=EG0, 1=EG1). */
static void graph_write_target_env_idx(const seq_env_t *env, uint8_t eg_index)
{
    if (eg_index == 1) {
        switch (s_graph_target) {
            case GRAPH_TGT_DRONE:
                drone_set_envelope2(env);
                break;
            case GRAPH_TGT_ARP:
                arp_set_envelope2(env);
                break;
            case GRAPH_TGT_MELODIC:
            default:
                if (s_editor_apply_all) {
                    for (uint8_t t = 0; t < SEQ_TRACKS; ++t)
                        sequencer_core_set_melodic_envelope2(s_graph_layer, t, env);
                } else {
                    sequencer_core_set_melodic_envelope2(s_graph_layer, s_graph_track, env);
                }
                break;
        }
        return;
    }
    switch (s_graph_target) {
        case GRAPH_TGT_DRONE:
            drone_set_envelope(env);
            break;
        case GRAPH_TGT_ARP:
            arp_set_envelope(env);
            break;
        case GRAPH_TGT_MELODIC:
        default:
            if (s_editor_apply_all) {
                for (uint8_t t = 0; t < SEQ_TRACKS; ++t)
                    sequencer_core_set_melodic_envelope(s_graph_layer, t, env);
            } else {
                sequencer_core_set_melodic_envelope(s_graph_layer, s_graph_track, env);
            }
            break;
    }
}

/* Backward-compatible wrapper: reads whichever eg_index is currently shown. */
static bool graph_read_target_env(seq_env_t *env)
{
    return graph_read_target_env_idx(env, s_graph_eg_index);
}

/* Open the editor seeded from the active screen's envelope. The target is chosen
 * by which top-level screen is showing: the drone screen edits the drone env,
 * the arp screen the arp env, otherwise the selected melodic row. */
void synth_ui_graph_open_envelope(void)
{
    graph_popup_ensure_init();

    if (seq_state.ui_mode == UI_MODE_DRONE) {
        s_graph_target = GRAPH_TGT_DRONE;
    } else if (seq_state.ui_mode == UI_MODE_ARP) {
        s_graph_target = GRAPH_TGT_ARP;
    } else {
        s_graph_target = GRAPH_TGT_MELODIC;
    }

    /* Melodic write-back targets a specific row; capture it at open time. */
    s_graph_layer = seq_state.active_layer_idx;
    s_graph_track = seq_state.selected_track;
    /* Always open on EG0 (amp); MY_BUTTON_3 long-press switches to EG1. */
    s_graph_eg_index = 0;

    seq_env_t env;
    if (!graph_read_target_env(&env)) {
        env = seq_default_melodic_env();
    }

    /* Set initial range from total env time BEFORE seeding so graph_ms_to_x()
     * uses the correct mapping when seed runs. No remap needed here since there
     * are no popup points yet — just flip the flag directly. */
    uint32_t total_env_ms = env.attack_ms + env.decay_ms + env.release_ms;
    s_graph_long_range = (total_env_ms >= GRAPH_RANGE_SHORT_MS);

    /* Seed amp scratch from the target's current trim; reset amp mode and dirty flag. */
    s_graph_amp_mode  = false;
    s_graph_env_dirty = false;
    switch (s_graph_target) {
        case GRAPH_TGT_DRONE:
            s_graph_amp_edit = drone_get_amp_trim();
            break;
        case GRAPH_TGT_ARP:
            s_graph_amp_edit = arp_get_amp_scale();
            break;
        case GRAPH_TGT_MELODIC:
        default:
            s_graph_amp_edit = sequencer_core_get_melodic_amp_scale(
                s_graph_layer, s_graph_track);
            break;
    }

    graph_seed_from_env(&env);
    graph_update_ticks();
    graph_popup_open(&s_graph_popup, GPOPUP_MODE_EDIT, NULL);
    graph_popup_set_style(&s_graph_popup, GPOPUP_STYLE_ADSR);
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph editor open: target=%d L%u row%u range=%s amp=%.2f",
             (int)s_graph_target, s_graph_layer, s_graph_track,
             s_graph_long_range ? "LONG" : "SHORT", (double)s_graph_amp_edit);
}

/* Convert the popup's current points to a seq_env_t and write it to the given
 * eg_index's store on the bound target. Shared by graph_commit_to_env() (the
 * currently-shown eg_index) and synth_ui_graph_toggle_eg_index() (writes the
 * DEPARTING eg_index through before switching the view). */
static void graph_write_points_to_env(uint8_t eg_index)
{
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    if (n < 4) return;   /* expect origin + A + D + R */

    uint32_t cum_a = graph_x_to_ms(pts[1].x);
    uint32_t cum_d = graph_x_to_ms(pts[2].x);
    uint32_t cum_r = graph_x_to_ms(pts[3].x);

    /* Convert cumulative times back to per-segment durations (clamp monotonic). */
    uint32_t a = cum_a;
    if (a < 2) a = 2;  /* minimum 2 ms attack prevents DAC pop on trigger */
    uint32_t d = (cum_d > cum_a) ? (cum_d - cum_a) : 0;
    uint32_t r = (cum_r > cum_d) ? (cum_r - cum_d) : 0;

    seq_env_t env;
    if (!graph_read_target_env_idx(&env, eg_index)) return;  /* keep eg_type from the target */
    env.attack_ms   = a;
    env.decay_ms    = d;
    env.release_ms  = r;
    env.sustain_pct = (uint8_t)(pts[2].y * 100.0f + 0.5f);

    graph_write_target_env_idx(&env, eg_index);
}

/* Read the edited points back, convert X->ms via the active range mapping, and
 * push the result to the bound row's envelope (which applies it to AMY). */
static void graph_commit_to_env(void)
{
    /* Only rewrite the envelope if the user actually moved a control point.
     * A volume-only edit (amp mode only) must not overwrite the stored envelope. */
    if (s_graph_env_dirty) {
        graph_write_points_to_env(s_graph_eg_index);
    }

    /* Commit per-target amplitude trim (s_graph_amp_edit seeded at open; setters
     * are no-ops when the value is unchanged so this is always safe to call). */
    switch (s_graph_target) {
        case GRAPH_TGT_DRONE:
            drone_set_amp_trim(s_graph_amp_edit);
            break;
        case GRAPH_TGT_ARP:
            arp_set_amp_scale(s_graph_amp_edit);
            break;
        case GRAPH_TGT_MELODIC:
        default:
            if (s_editor_apply_all) {
                for (uint8_t t = 0; t < SEQ_TRACKS; ++t)
                    sequencer_core_set_melodic_amp_scale(s_graph_layer, t, s_graph_amp_edit);
            } else {
                sequencer_core_set_melodic_amp_scale(s_graph_layer, s_graph_track,
                                                     s_graph_amp_edit);
            }
            break;
    }
    s_graph_amp_mode = false;   /* clear mode so topbar reverts on next open */
}

/* Set the time-range mode (long_range=true → LONG 15s, false → SHORT 2s) and
 * remap current on-screen points so in-progress edits survive the switch.
 * No-op when already in the target range. This is the single place that flips
 * s_graph_long_range — all callers (manual toggle, auto-range) go through here. */
static void graph_set_range(bool long_range)
{
    if (s_graph_long_range == long_range) return;

    /* Snapshot each point's time in ms under the OLD range, then flip the range
     * and remap ms → x under the NEW range.  Y is range-independent. */
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    uint32_t ms[GPOPUP_MAX_POINTS];
    for (uint8_t i = 0; i < n; ++i) ms[i] = graph_x_to_ms(pts[i].x);

    s_graph_long_range = long_range;

    for (uint8_t i = 0; i < n; ++i) pts[i].x = graph_ms_to_x(ms[i]);
    uint8_t saved_cursor  = s_graph_popup.cursor;
    bool    saved_editing = s_graph_popup.editing_value;
    bool    saved_axis_y  = s_graph_popup.adjust_axis_y;
    graph_popup_set_points(&s_graph_popup, pts, n);
    s_graph_popup.cursor        = saved_cursor;
    s_graph_popup.editing_value = saved_editing;
    s_graph_popup.adjust_axis_y = saved_axis_y;

    graph_update_ticks();
    ESP_LOGI(TAG, "graph range -> %s", long_range ? "LONG(15s)" : "SHORT(2s)");
}

/* Auto-switch the range based on the total envelope time (pts[3].x = cum_r).
 * Metric: rightmost point because that is the literal x-extent of the curve —
 * a long attack also overflows the SHORT axis, and cum_r = A+D+R captures all.
 * Hysteresis: SHORT→LONG at ≥2000ms, LONG→SHORT only at ≤1700ms so the range
 * does not flicker at the boundary. Called after every encoder edit and once at
 * editor open. No-op when the popup is not active. */
static void graph_auto_range_check(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return;
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    if (n < 4) return;
    uint32_t total_ms = graph_x_to_ms(pts[3].x);
    if (!s_graph_long_range && total_ms >= GRAPH_RANGE_SHORT_MS) {
        graph_set_range(true);   /* grow to LONG at the SHORT axis ceiling */
    } else if (s_graph_long_range && total_ms <= 1700u) {
        graph_set_range(false);  /* shrink back to SHORT below hysteresis floor */
    }
}

/* Toggle SHORT<->LONG time range manually (kept for any external callers).
 * Now delegates to graph_set_range() to keep the remap logic in one place. */
bool synth_ui_graph_toggle_range(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;
    graph_set_range(!s_graph_long_range);
    return true;
}

/* Toggle amp-edit mode: when active the encoder adjusts the selected target's
 * amplitude trim instead of moving ADSR points. MY_BUTTON_2 activates this while
 * the graph editor is open. The mode is reset on editor open/close. */
void synth_ui_graph_toggle_amp_mode(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return;
    s_graph_amp_mode = !s_graph_amp_mode;
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph amp mode %s", s_graph_amp_mode ? "ON" : "OFF");
}

/* Switch the editor between the target's EG0 (amp) and EG1 (typically filter)
 * breakpoint sets. Any in-progress, uncommitted edit on the departing
 * eg_index is written through first (mirrors the "deferred authority" model:
 * a commit here only reaches AMY if that row/target was already authored, or
 * becomes authored now) so flipping tabs never silently discards work. The
 * curve is then fully reseeded (and range re-derived) from the other
 * eg_index's own stored envelope — the two can have very different shapes. */
void synth_ui_graph_toggle_eg_index(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return;

    if (s_graph_env_dirty) {
        graph_write_points_to_env(s_graph_eg_index);
    }

    s_graph_eg_index = (s_graph_eg_index == 0) ? 1 : 0;

    seq_env_t env;
    if (!graph_read_target_env(&env)) {
        env = (s_graph_eg_index == 1) ? seq_default_melodic_env1()
                                       : seq_default_melodic_env();
    }
    uint32_t total_env_ms = env.attack_ms + env.decay_ms + env.release_ms;
    s_graph_long_range = (total_env_ms >= GRAPH_RANGE_SHORT_MS);

    graph_seed_from_env(&env);
    graph_update_ticks();
    s_graph_env_dirty = false;
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph eg index -> EG%u", s_graph_eg_index);
}

/* Route an encoder delta to the pop-up. Returns true if the pop-up consumed it
 * (host should then skip its normal sequencer routing). */
bool synth_ui_graph_handle_encoder(long delta)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;

    if (s_graph_amp_mode) {
        /* Amp mode: encoder adjusts per-target amplitude trim in 5% steps. */
        float v = s_graph_amp_edit + (float)delta * 0.05f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        s_graph_amp_edit = v;
        s_force_redraw = true;
        return true;
    }

    s_graph_env_dirty = true;   /* user moved an ADSR control point */
    graph_popup_handle_encoder(&s_graph_popup, delta);
    /* Moving A (time) or the S level changes the derived decay; re-snap S.x. */
    graph_recompute_decay();
    /* Auto-switch range if total envelope time crosses the threshold. */
    graph_auto_range_check();
    return true;
}

/* Route a button event to the pop-up. is_long selects short vs long press.
 * Returns true if the pop-up consumed the event. On confirm/cancel the pop-up
 * is closed here. */
bool synth_ui_graph_handle_button(bool is_long)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;

    gpopup_result_t r = is_long
        ? graph_popup_handle_button_long(&s_graph_popup)
        : graph_popup_handle_button(&s_graph_popup);

    if (r == GPOPUP_RESULT_CONFIRMED) {
        ESP_LOGI(TAG, "graph popup confirmed -> writing envelope");
        graph_commit_to_env();
        graph_popup_close(&s_graph_popup);
    } else if (r == GPOPUP_RESULT_CANCELLED) {
        ESP_LOGI(TAG, "graph popup cancelled (envelope unchanged)");
        graph_popup_close(&s_graph_popup);
    }
    return true;
}

/* Commit the current edits and close the editor. Used by the encoder long-press
 * (symmetric with the long-press that opens it): closing keeps your work. The
 * separate discard path is synth_ui_graph_handle_button(true) (cancel). */
bool synth_ui_graph_close_commit(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;
    ESP_LOGI(TAG, "graph editor close (commit envelope)");
    graph_commit_to_env();
    graph_popup_close(&s_graph_popup);
    s_force_redraw = true;
    return true;
}

/* ── graph pop-up: end ──────────────────────────────────────────────────── */

/* ── Filter editor ──────────────────────────────────────────────────────── */

bool         s_filter_active = false;
static filter_graph_t s_fgraph;
static seq_filter_t s_filter_edit;   /* scratch copy while editing */

/* ── LFO editor state ── */
bool       s_lfo_active = false;
static lfo_view_t s_lfo_view;

/* Normalisation helpers shared between open and commit. */
static float filter_hz_to_norm(float hz)
{
    if (hz < FGRAPH_CUTOFF_HZ_MIN) hz = FGRAPH_CUTOFF_HZ_MIN;
    if (hz > FGRAPH_CUTOFF_HZ_MAX) hz = FGRAPH_CUTOFF_HZ_MAX;
    return log2f(hz / FGRAPH_CUTOFF_HZ_MIN) /
           log2f(FGRAPH_CUTOFF_HZ_MAX / FGRAPH_CUTOFF_HZ_MIN);
}

static float filter_norm_to_hz(float norm)
{
    return FGRAPH_CUTOFF_HZ_MIN *
           powf(FGRAPH_CUTOFF_HZ_MAX / FGRAPH_CUTOFF_HZ_MIN,
                SEQ_CLAMP_F32(norm, 0.0f, 1.0f));
}

static float filter_q_to_norm(float q)
{
    float n = (q - FGRAPH_RES_MIN) / (FGRAPH_RES_MAX - FGRAPH_RES_MIN);
    return SEQ_CLAMP_F32(n, 0.0f, 1.0f);
}

static float filter_norm_to_q(float n)
{
    return FGRAPH_RES_MIN + SEQ_CLAMP_F32(n, 0.0f, 1.0f) *
           (FGRAPH_RES_MAX - FGRAPH_RES_MIN);
}

/* Populate s_fgraph from s_filter_edit and the current graph target. */
static void filter_sync_fgraph(void)
{
    s_fgraph.filter_type    = s_filter_edit.filter_type;
    s_fgraph.cutoff_norm    = filter_hz_to_norm(s_filter_edit.cutoff_hz);
    s_fgraph.resonance_norm = filter_q_to_norm(s_filter_edit.resonance);
    s_fgraph.enabled        = s_filter_edit.enabled;
    /* cursor and editing stay unchanged */
}

/* Read the current filter state from the active target into s_filter_edit. */
static void filter_load_from_target(void)
{
    seq_filter_t f = {0};
    if (seq_state.ui_mode == UI_MODE_DRONE) {
        /* Drone: always LPF24, cutoff = sweep midpoint, Q from drone_get_resonance. */
        float lo = drone_get_sweep_lo();
        float hi = drone_get_sweep_hi();
        f.filter_type = 4;   /* SEQ_FILTER_LPF24 */
        f.cutoff_hz   = SEQ_CLAMP_F32((lo + hi) * 0.5f, FGRAPH_CUTOFF_HZ_MIN, FGRAPH_CUTOFF_HZ_MAX);
        f.resonance   = drone_get_resonance();
        f.enabled     = true;

        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "DRONE");
    } else if (seq_state.ui_mode == UI_MODE_ARP) {
        arp_get_filter(&f);
        /* Only apply display defaults when the filter was never authored
         * (cutoff_hz == 0 from zero-init); preserve authored values even
         * when disabled so enabled=false round-trips correctly. */
        if (f.cutoff_hz <= 0.0f) {
            f.filter_type = SEQ_FILTER_LPF;
            f.cutoff_hz   = 800.0f;
            f.resonance   = 1.0f;
            f.enabled     = true;
        }
        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "ARP");
    } else {
        uint8_t li = seq_state.active_layer_idx;
        uint8_t tr = seq_state.selected_track;
        sequencer_core_get_melodic_filter(li, tr, &f);
        /* Same sentinel: zero cutoff means never-authored; disabled-but-authored
         * keeps its authored values for correct round-trip display. */
        if (f.cutoff_hz <= 0.0f) {
            f.filter_type = SEQ_FILTER_LPF;
            f.cutoff_hz   = 800.0f;
            f.resonance   = 1.0f;
            f.enabled     = true;
        }
        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "L%u T%u%s",
                 (unsigned)(li + 1), (unsigned)(tr + 1),
                 s_editor_apply_all ? ">L" : ">T");
    }
    s_filter_edit = f;
}

bool synth_ui_filter_is_active(void)
{
    return s_filter_active;
}

void synth_ui_filter_open(void)
{
    filter_load_from_target();
    s_fgraph.cursor  = 0;
    s_fgraph.editing = false;
    /* Drone filter is always LPF24 (type fixed) — skip the type cursor. */
    s_fgraph.enabled = s_filter_edit.enabled;
    filter_sync_fgraph();
    s_filter_active = true;
    s_force_redraw  = true;
    ESP_LOGI(TAG, "filter editor open: %s type%u %.0fHz Q%.2f",
             s_fgraph.label, s_filter_edit.filter_type,
             (double)s_filter_edit.cutoff_hz, (double)s_filter_edit.resonance);
}

bool synth_ui_filter_handle_encoder(long delta)
{
    if (!s_filter_active) return false;
    bool drone = (seq_state.ui_mode == UI_MODE_DRONE);

    if (!s_fgraph.editing) {
        /* Not editing: scroll cursor position.
         * Drone: 0=cutoff 1=resonance (type fixed, no EN cursor).
         * Non-drone: 0=cutoff 1=resonance 2=type 3=enable. */
        uint8_t max_cursor = drone ? 1 : 3;
        if (delta > 0) {
            s_fgraph.cursor = (uint8_t)((s_fgraph.cursor + 1) % (max_cursor + 1));
        } else if (delta < 0) {
            s_fgraph.cursor = (s_fgraph.cursor == 0) ? max_cursor : (s_fgraph.cursor - 1);
        }
        s_force_redraw = true;
        return true;
    }

    /* Editing: adjust the selected parameter. */
    switch (s_fgraph.cursor) {
        case 0: {   /* cutoff */
            float step = 0.015f * (float)delta;
            s_fgraph.cutoff_norm = SEQ_CLAMP_F32(s_fgraph.cutoff_norm + step, 0.0f, 1.0f);
            s_filter_edit.cutoff_hz = filter_norm_to_hz(s_fgraph.cutoff_norm);
            break;
        }
        case 1: {   /* resonance */
            float step = 0.02f * (float)delta;
            s_fgraph.resonance_norm = SEQ_CLAMP_F32(s_fgraph.resonance_norm + step, 0.0f, 1.0f);
            s_filter_edit.resonance = filter_norm_to_q(s_fgraph.resonance_norm);
            break;
        }
        case 2: {   /* type (melodic/arp only) */
            if (!drone) {
                int t = (int)s_filter_edit.filter_type + (int)delta;
                /* Wrap within 1..4 (NONE is toggled via MY_BUTTON_1, not the type cursor). */
                if (t < 1) t = 4;
                if (t > 4) t = 1;
                s_filter_edit.filter_type = (uint8_t)t;
                s_fgraph.filter_type      = (uint8_t)t;
            }
            break;
        }
        case 3: {   /* enable toggle (melodic/arp only; drone filter is always on) */
            if (!drone) {
                s_filter_edit.enabled = !s_filter_edit.enabled;
                s_fgraph.enabled      = s_filter_edit.enabled;
                ESP_LOGI(TAG, "filter enabled -> %d", (int)s_filter_edit.enabled);
            }
            break;
        }
        default: break;
    }
    filter_sync_fgraph();
    s_force_redraw = true;
    return true;
}

bool synth_ui_filter_handle_button(bool is_long)
{
    if (!s_filter_active) return false;
    if (is_long) {
        /* Long press = cancel: reload original. */
        filter_load_from_target();
        filter_sync_fgraph();
        s_filter_active = false;
        s_force_redraw  = true;
        ESP_LOGI(TAG, "filter editor cancelled");
        return true;
    }
    /* Short press: toggle editing on/off for the current cursor. */
    s_fgraph.editing = !s_fgraph.editing;
    s_force_redraw = true;
    return true;
}

void synth_ui_filter_toggle_enabled(void)
{
    if (!s_filter_active) return;
    s_filter_edit.enabled = !s_filter_edit.enabled;
    s_fgraph.enabled      = s_filter_edit.enabled;
    s_force_redraw = true;
    ESP_LOGI(TAG, "filter enabled -> %d", (int)s_filter_edit.enabled);
}

bool synth_ui_filter_close_commit(void)
{
    if (!s_filter_active) return false;

    if (seq_state.ui_mode == UI_MODE_DRONE) {
        /* Drone: update resonance + move sweep range to new midpoint. */
        float lo = drone_get_sweep_lo();
        float hi = drone_get_sweep_hi();
        float half = (hi - lo) * 0.5f;
        float new_mid = filter_norm_to_hz(s_fgraph.cutoff_norm);
        drone_set_sweep_lo(SEQ_CLAMP_F32(new_mid - half, 65.0f, 7900.0f));
        drone_set_sweep_hi(SEQ_CLAMP_F32(new_mid + half, 100.0f, 8000.0f));
        drone_set_resonance(s_filter_edit.resonance);
        ESP_LOGI(TAG, "filter commit drone: sweepMid=%.0f Q=%.2f",
                 (double)new_mid, (double)s_filter_edit.resonance);
    } else if (seq_state.ui_mode == UI_MODE_ARP) {
        arp_set_filter(&s_filter_edit);
        ESP_LOGI(TAG, "filter commit arp: type%u %.0fHz Q%.2f",
                 s_filter_edit.filter_type,
                 (double)s_filter_edit.cutoff_hz, (double)s_filter_edit.resonance);
    } else {
        uint8_t li = seq_state.active_layer_idx;
        if (s_editor_apply_all) {
            for (uint8_t t = 0; t < SEQ_TRACKS; ++t)
                sequencer_core_set_melodic_filter(li, t, &s_filter_edit);
        } else {
            sequencer_core_set_melodic_filter(li, seq_state.selected_track, &s_filter_edit);
        }
    }
    s_filter_active = false;
    s_force_redraw  = true;
    return true;
}

/* ── Filter editor: end ──────────────────────────────────────────────────── */

/* ── LFO editor ──────────────────────────────────────────────────────────── */

uint32_t lfo_view_signature(void)
{
    const seq_lfo_t *l = &s_lfo_view.lfo;
    return (uint32_t)l->enabled
         | ((uint32_t)l->wave   <<  4)
         | ((uint32_t)l->target <<  8)
         | ((uint32_t)l->depth  << 12)
         | ((uint32_t)l->rate   << 20)
         | ((uint32_t)s_lfo_view.cursor  << 24)
         | ((uint32_t)s_lfo_view.editing << 28);
}

bool synth_ui_lfo_is_active(void) { return s_lfo_active; }

void synth_ui_lfo_open(void)
{
    // TODO: drone LFO = BPM-multiplier stutter only; implement constrained editor when needed
    if (seq_state.ui_mode == UI_MODE_DRONE) return; // drone has no free LFO editor
    uint8_t li = seq_state.active_layer_idx;
    uint8_t tr = seq_state.selected_track;
    seq_lfo_t existing = {
        .enabled = false,
        .mode    = LFO_MODE_FREE,
        .wave    = LFO_WAVE_SINE,
        .rate    = LFO_RATE_1BAR,
        .depth   = 50,
        .target  = LFO_TARGET_FILTER,
    };
    if (seq_state.ui_mode == UI_MODE_ARP)
        arp_get_lfo(&existing);
    else
        sequencer_core_get_melodic_lfo(li, tr, &existing);
    s_lfo_view.lfo          = existing;
    s_lfo_view.cursor       = 0;
    s_lfo_view.editing      = false;
    s_lfo_view.layer_idx    = li;
    s_lfo_view.track_idx    = tr;
    s_lfo_view.apply_all    = s_editor_apply_all;
    s_lfo_view.target_label = (seq_state.ui_mode == UI_MODE_ARP)   ? "ARP"
                            : (seq_state.ui_mode == UI_MODE_DRONE) ? "DRONE"
                            : NULL;
    s_lfo_active   = true;
    s_force_redraw = true;
    ESP_LOGI(TAG, "LFO editor open L%u T%u", li, tr);
}

bool synth_ui_lfo_handle_encoder(long delta)
{
    if (!s_lfo_active) return false;
    /* 5 fields: TGT, WAV, RTE, DEP, EN  (MODE removed — RETRIG not yet impl.) */
    const uint8_t N = 5;
    if (!s_lfo_view.editing) {
        if (delta > 0)      s_lfo_view.cursor = (s_lfo_view.cursor + 1) % N;
        else if (delta < 0) s_lfo_view.cursor = (s_lfo_view.cursor + N - 1) % N;
        s_force_redraw = true;
        return true;
    }
    seq_lfo_t *l = &s_lfo_view.lfo;
    int d = (delta > 0) ? 1 : -1;
    switch (s_lfo_view.cursor) {
        case 0: /* target */
            l->target = (lfo_target_t)((l->target + LFO_TARGET_COUNT + d) % LFO_TARGET_COUNT);
            break;
        case 1: /* wave */
            l->wave = (lfo_wave_t)((l->wave + LFO_WAVE_COUNT + d) % LFO_WAVE_COUNT);
            break;
        case 2: /* rate */
            l->rate = (lfo_rate_t)((l->rate + LFO_RATE_COUNT + d) % LFO_RATE_COUNT);
            break;
        case 3: /* depth: ±5%, clamped 0..100 */
            if (d > 0) l->depth = (l->depth < 95) ? l->depth + 5 : 100;
            else       l->depth = (l->depth >  5) ? l->depth - 5 : 0;
            break;
        case 4: /* enabled */
            l->enabled = !l->enabled;
            break;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_lfo_handle_button(bool is_long)
{
    if (!s_lfo_active) return false;
    if (is_long) {
        s_lfo_active   = false;
        s_force_redraw = true;
        ESP_LOGI(TAG, "LFO editor cancelled");
        return true;
    }
    s_lfo_view.editing = !s_lfo_view.editing;
    s_force_redraw = true;
    return true;
}

bool synth_ui_lfo_close_commit(void)
{
    if (!s_lfo_active) return false;
    if (seq_state.ui_mode == UI_MODE_DRONE) return false; // drone has no free LFO editor
    if (seq_state.ui_mode == UI_MODE_ARP) {
        arp_set_lfo(&s_lfo_view.lfo);
    } else if (s_editor_apply_all) {
        for (uint8_t t = 0; t < SEQ_TRACKS; ++t)
            sequencer_core_set_melodic_lfo(s_lfo_view.layer_idx, t, &s_lfo_view.lfo);
    } else {
        sequencer_core_set_melodic_lfo(s_lfo_view.layer_idx,
                                       s_lfo_view.track_idx,
                                       &s_lfo_view.lfo);
    }
    s_lfo_active   = false;
    s_force_redraw = true;
    ESP_LOGI(TAG, "LFO editor committed (apply_all=%d)", (int)s_editor_apply_all);
    return true;
}

/* Toggle layer-wide vs single-track commit scope for the effects editors.
 * Returns true if an appropriate editor was active and the event was consumed.
 * Called from MY_BUTTON_1 while ADSR graph or LFO editor is open.
 * ARP and DRONE modes have no "apply to all tracks" concept — no-op there. */
bool synth_ui_toggle_editor_apply_scope(void)
{
    if (seq_state.ui_mode == UI_MODE_ARP || seq_state.ui_mode == UI_MODE_DRONE)
        return false;
    bool graph_open = graph_popup_is_active(&s_graph_popup);
    if (!graph_open && !s_lfo_active) return false;

    s_editor_apply_all = !s_editor_apply_all;

    /* Keep the LFO view in sync so lfo_view_draw() shows the correct indicator. */
    if (s_lfo_active) {
        s_lfo_view.apply_all = s_editor_apply_all;
    }

    s_force_redraw = true;
    ESP_LOGI(TAG, "editor scope -> %s", s_editor_apply_all ? "LAYER" : "TRACK");
    return true;
}

/* Cycle ADSR → Filter → LFO → ADSR (MY_BUTTON_3 while any editor is open). */
void synth_ui_cycle_editor(void)
{
    if (graph_popup_is_active(&s_graph_popup)) {
        synth_ui_graph_close_commit();
        synth_ui_filter_open();
    } else if (s_filter_active) {
        synth_ui_filter_close_commit();
        if (seq_state.ui_mode == UI_MODE_DRONE)
            synth_ui_graph_open_envelope(); // drone: skip LFO tab, cycle back to ADSR
        else
            synth_ui_lfo_open();
    } else if (s_lfo_active) {
        synth_ui_lfo_close_commit();
        synth_ui_graph_open_envelope();
    }
}

/* ── LFO editor: end ─────────────────────────────────────────────────────── */

/* ── View signatures ─────────────────────────────────────────────────────── */

[[gnu::pure]] uint32_t filter_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &s_fgraph, sizeof(s_fgraph));
    return h;
}

[[gnu::pure]] uint32_t graph_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &s_graph_popup.cursor, sizeof(s_graph_popup.cursor));
    h = fnv1a_bytes(h, &s_graph_popup.editing_value, sizeof(s_graph_popup.editing_value));
    h = fnv1a_bytes(h, &s_graph_popup.adjust_axis_y, sizeof(s_graph_popup.adjust_axis_y));
    h = fnv1a_bytes(h, &s_graph_long_range, sizeof(s_graph_long_range));
    h = fnv1a_bytes(h, &s_graph_layer, sizeof(s_graph_layer));
    h = fnv1a_bytes(h, &s_graph_track, sizeof(s_graph_track));
    h = fnv1a_bytes(h, &s_graph_amp_mode, sizeof(s_graph_amp_mode));
    h = fnv1a_bytes(h, &s_graph_amp_edit, sizeof(s_graph_amp_edit));
    h = fnv1a_bytes(h, &s_graph_eg_index, sizeof(s_graph_eg_index));
    h = fnv1a_bytes(h, s_graph_popup.points,
                    s_graph_popup.num_points * sizeof(gpopup_point_t));
    return h;
}

/* ── Draw wrappers (encapsulate private editor state from synth_ui_task) ── */

/* Draw the yellow context top bar (rows 0..15) for the envelope editor. */
static void graph_draw_topbar(u8g2_t *u8g2)
{
    char buf[24];

    /* Left: which target is being edited (ARP/DRONE get named labels), plus
     * which of the two independent breakpoint generators (EG0/EG1) is shown. */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    const char *eg_tag = (s_graph_eg_index == 1) ? "EG1" : "EG0";
    if (s_graph_target == GRAPH_TGT_ARP) {
        snprintf(buf, sizeof(buf), "ARP %s", eg_tag);
    } else if (s_graph_target == GRAPH_TGT_DRONE) {
        snprintf(buf, sizeof(buf), "DRONE %s", eg_tag);
    } else {
        snprintf(buf, sizeof(buf), "L%u T%u %s%s",
                 s_graph_layer + 1, s_graph_track + 1, eg_tag,
                 s_editor_apply_all ? ">L" : ">T");
    }
    u8g2_DrawStr(u8g2, 2, 8, buf);

    /* Right: amp indicator when in amp mode (replaces the old "S/L" range flag
     * which is now set automatically and no longer meaningful to the user). */
    uint8_t rw = 0;
    if (s_graph_amp_mode) {
        char amp_buf[10];
        snprintf(amp_buf, sizeof(amp_buf), "AMP%d%%",
                 (int)(s_graph_amp_edit * 100.0f + 0.5f));
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        rw = (uint8_t)u8g2_GetStrWidth(u8g2, amp_buf);
        u8g2_DrawStr(u8g2, (uint8_t)(128 - rw - 2), 8, amp_buf);
    }

    /* Middle: live readout of the selected point's real value (ms / %). */
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    uint8_t c = s_graph_popup.cursor;
    if (!s_graph_amp_mode && n >= 4 && c >= 1 && c <= 3) {
        uint32_t cum_a = graph_x_to_ms(pts[1].x);
        uint32_t cum_d = graph_x_to_ms(pts[2].x);
        uint32_t cum_r = graph_x_to_ms(pts[3].x);
        if (c == 1) {
            snprintf(buf, sizeof(buf), "A %lums", (unsigned long)cum_a);
        } else if (c == 2) {
            uint32_t d = (cum_d > cum_a) ? (cum_d - cum_a) : 0;
            snprintf(buf, sizeof(buf), "D %lums S %u%%",
                     (unsigned long)d, (unsigned)(pts[2].y * 100.0f + 0.5f));
        } else {
            uint32_t r = (cum_r > cum_d) ? (cum_r - cum_d) : 0;
            snprintf(buf, sizeof(buf), "R %lums", (unsigned long)r);
        }
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, buf);
        /* Centre-ish, between the left label (~x=56) and the right indicator. */
        int mx = 60 + (int)((128 - 60 - (int)rw - 4 - (int)tw) / 2);
        if (mx < 60) mx = 60;
        u8g2_DrawStr(u8g2, (uint8_t)mx, 8, buf);
    }

    /* Divider at the yellow/blue boundary. */
    u8g2_DrawHLine(u8g2, 0, GRAPH_TOPBAR_H - 1, 128);
}

void synth_ui_graph_view_draw(u8g2_t *u8g2)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    graph_draw_topbar(u8g2);
    graph_popup_draw(u8g2, &s_graph_popup);
    u8g2_SendBuffer(u8g2);
}

void synth_ui_filter_view_draw(u8g2_t *u8g2)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    filter_graph_draw(u8g2, &s_fgraph);
    u8g2_SendBuffer(u8g2);
}

void synth_ui_lfo_view_draw(u8g2_t *u8g2)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    lfo_view_draw(u8g2, &s_lfo_view);
    u8g2_SendBuffer(u8g2);
}

#include "synth_ui.h"
#include "display_seq.h"
#include "display_menu.h"
#include "display_arp.h"
#include "display_drone.h"
#include "seq_clamp.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "quantizer.h"
#include "graph_popup.h"
#include "patch_names.h"
#include "amy.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <math.h>

static const char *TAG = "synth_ui";

/* DEBUG: bisect heap corruption inside the init chain. Gated by
 * CONFIG_AMYSYNTH_HEAP_CHECK (menuconfig: Heap Diagnostics); off by default, in
 * which case every checkpoint compiles to nothing. When on, the first
 * "HEAP CORRUPT" line names the exact sub-step that smashed the heap. */
#if CONFIG_AMYSYNTH_HEAP_CHECK
#define SEQ_HEAP_CHECK(where) do { \
    if (!heap_caps_check_integrity_all(true)) { \
        ESP_LOGE(TAG, "HEAP CORRUPT detected at: %s", where); \
    } else { \
        ESP_LOGI(TAG, "HEAP OK at: %s", where); \
    } \
} while (0)
#else
#define SEQ_HEAP_CHECK(where) do { (void)(where); } while (0)
#endif

/* ── Graph pop-up integration (isolated, easily removable) ───────────────────
 * Everything between this block and the matching "graph pop-up: end" marker is
 * the demo wiring for the reusable graph_popup widget. Deleting this block plus
 * the single gated branch in synth_ui_task() and the three public
 * synth_ui_graph_* entry points fully restores the original behaviour.
 *
 * The melodic envelope is defined by compile-time CONFIG_SEQ_MELODIC_ENV_*
 * values (see sequencer_core.c). We mirror those defaults here to seed the
 * editor, then map them to/from the widget via the AMY adapter. */
#ifndef CONFIG_SEQ_MELODIC_ENV_ATTACK_MS
#define CONFIG_SEQ_MELODIC_ENV_ATTACK_MS 12
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_DECAY_MS
#define CONFIG_SEQ_MELODIC_ENV_DECAY_MS 220
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT
#define CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT 58
#endif
#ifndef CONFIG_SEQ_MELODIC_ENV_RELEASE_MS
#define CONFIG_SEQ_MELODIC_ENV_RELEASE_MS 280
#endif

static gpopup_t s_graph_popup;
static bool     s_graph_popup_inited = false;

/* Set by mode/layout transitions to force one redraw regardless of the
 * render-on-change signature (guards first-frame-after-transition staleness). */
static volatile bool s_force_redraw = true;

/* Which (layer,track) the open editor is bound to, captured at open time so the
 * write-back targets the same row even if the selection moves underneath. */
static uint8_t  s_graph_layer = 0;
static uint8_t  s_graph_track = 0;

/* Which backend the open editor edits. Captured at open time so seed/commit
 * route to the right envelope store (melodic row, the drone, or the arp). The
 * same widget + range mapping + commit math serve all three. */
typedef enum {
    GRAPH_TGT_MELODIC = 0,
    GRAPH_TGT_DRONE   = 1,
    GRAPH_TGT_ARP     = 2,
} graph_target_t;
static graph_target_t s_graph_target = GRAPH_TGT_MELODIC;

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

static bool s_graph_long_range = false;   /* false = SHORT, true = LONG */

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
#define DECAY_BASE_MS          40u
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

/* Read the bound target's current envelope into `env`. Returns false only if the
 * melodic target has no valid row (caller then seeds compile-time defaults). */
static bool graph_read_target_env(seq_env_t *env)
{
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

    seq_env_t env;
    if (!graph_read_target_env(&env)) {
        env.attack_ms   = CONFIG_SEQ_MELODIC_ENV_ATTACK_MS;
        env.decay_ms    = CONFIG_SEQ_MELODIC_ENV_DECAY_MS;
        env.sustain_pct = CONFIG_SEQ_MELODIC_ENV_SUSTAIN_PCT;
        env.release_ms  = CONFIG_SEQ_MELODIC_ENV_RELEASE_MS;
        env.eg_type     = CONFIG_SEQ_MELODIC_ENV_EG0_TYPE;
    }

    graph_seed_from_env(&env);
    graph_update_ticks();
    graph_popup_open(&s_graph_popup, GPOPUP_MODE_EDIT, NULL);
    graph_popup_set_style(&s_graph_popup, GPOPUP_STYLE_ADSR);
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph editor open: target=%d L%u row%u range=%s",
             (int)s_graph_target, s_graph_layer, s_graph_track,
             s_graph_long_range ? "LONG" : "SHORT");
}

/* Read the edited points back, convert X->ms via the active range mapping, and
 * push the result to the bound row's envelope (which applies it to AMY). */
static void graph_commit_to_env(void)
{
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    if (n < 4) return;   /* expect origin + A + D + R */

    uint32_t cum_a = graph_x_to_ms(pts[1].x);
    uint32_t cum_d = graph_x_to_ms(pts[2].x);
    uint32_t cum_r = graph_x_to_ms(pts[3].x);

    /* Convert cumulative times back to per-segment durations (clamp monotonic). */
    uint32_t a = cum_a;
    uint32_t d = (cum_d > cum_a) ? (cum_d - cum_a) : 0;
    uint32_t r = (cum_r > cum_d) ? (cum_r - cum_d) : 0;

    seq_env_t env;
    if (!graph_read_target_env(&env)) return;  /* keep eg_type from the target */
    env.attack_ms   = a;
    env.decay_ms    = d;
    env.release_ms  = r;
    env.sustain_pct = (uint8_t)(pts[2].y * 100.0f + 0.5f);

    switch (s_graph_target) {
        case GRAPH_TGT_DRONE:
            drone_set_envelope(&env);
            break;
        case GRAPH_TGT_ARP:
            arp_set_envelope(&env);
            break;
        case GRAPH_TGT_MELODIC:
        default:
            sequencer_core_set_melodic_envelope(s_graph_layer, s_graph_track, &env);
            break;
    }
}

/* Toggle SHORT<->LONG time range while the editor is open and re-seed so the
 * displayed curve stays anchored to the same underlying envelope. */
bool synth_ui_graph_toggle_range(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;

    /* Convert the CURRENT on-screen points through the range change instead of
     * re-seeding from storage, so in-progress edits are preserved (no reset).
     * Snapshot the points, read each X back to ms under the OLD range, flip the
     * range, then re-map ms -> X under the NEW range. Y is range-independent. */
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);

    uint32_t ms[GPOPUP_MAX_POINTS];
    for (uint8_t i = 0; i < n; ++i) {
        ms[i] = graph_x_to_ms(pts[i].x);   /* OLD range mapping */
    }

    s_graph_long_range = !s_graph_long_range;

    for (uint8_t i = 0; i < n; ++i) {
        pts[i].x = graph_ms_to_x(ms[i]);   /* NEW range mapping */
    }
    /* Preserve cursor / edit state across the conversion. */
    uint8_t  saved_cursor   = s_graph_popup.cursor;
    bool     saved_editing  = s_graph_popup.editing_value;
    bool     saved_axis_y   = s_graph_popup.adjust_axis_y;
    graph_popup_set_points(&s_graph_popup, pts, n);
    s_graph_popup.cursor        = saved_cursor;
    s_graph_popup.editing_value = saved_editing;
    s_graph_popup.adjust_axis_y = saved_axis_y;

    graph_update_ticks();
    ESP_LOGI(TAG, "graph range -> %s", s_graph_long_range ? "LONG(15s)" : "SHORT(2s)");
    return true;
}

/* Route an encoder delta to the pop-up. Returns true if the pop-up consumed it
 * (host should then skip its normal sequencer routing). */
bool synth_ui_graph_handle_encoder(long delta)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;
    graph_popup_handle_encoder(&s_graph_popup, delta);
    /* Moving A (time) or the S level changes the derived decay; re-snap S.x. */
    graph_recompute_decay();
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

/* Flip the active adjust axis (vertical level <-> horizontal time). Only does
 * anything while the editor is open; returns true when it consumed the event so
 * the host knows the press belonged to the graph and not a background action. */
bool synth_ui_graph_toggle_axis(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;
    bool toggled = graph_popup_toggle_axis(&s_graph_popup);
    if (toggled) {
        ESP_LOGI(TAG, "graph popup axis -> %s",
                 graph_popup_axis_is_y(&s_graph_popup) ? "vertical" : "horizontal");
    }
    /* Consume the event whenever the graph is open, even if the toggle was a
     * no-op (e.g. VIEW mode), so the button never leaks to layer switching. */
    return true;
}
/* ── graph pop-up: end ──────────────────────────────────────────────────── */

#if !CONFIG_SEQ_PATCH_BROWSE_FULL_RANGE
/* Runtime patch cycling shortlist: intentionally small and musical.
 * Values map to AMY built-ins (Juno/DX7/piano). Used only when the browse mode
 * is "preselected"; the full-range mode walks 0..SEQ_PATCH_FULL_MAX instead. */
static const uint16_t s_melodic_patch_cycle[] = {
    138, /* DX7 E.PIANO 1 */
    135, /* DX7 PIANO 1 */
    141, /* DX7 SYN-LEAD 1 */
    151, /* DX7 FLUTE 1 */
    7,   /* Juno A18 Piano I */
    104, /* Juno B61 E. Piano with Tremolo */
    256, /* Built-in piano */
};
#define SEQ_RUNTIME_PATCH_COUNT ((int)(sizeof(s_melodic_patch_cycle) / sizeof(s_melodic_patch_cycle[0])))
#endif

/* Full-range browse covers every AMY built-in: Juno 0..127, DX7 128..255,
 * built-in piano 256 (matches the sequencer_core clamp upper bound). */
#define SEQ_PATCH_FULL_MAX 256

synth_ui_state_t seq_state = {
    /* layers[] is zero-initialized by C99 partial-init rules */
    .num_layers       = 0,
    .active_layer_idx = 0,
    .bpm              = 120,
    .current_pattern  = 1,
    .current_step     = 0,
    .playing          = true,
    .selected_track   = 0,
    .selected_step    = 0,
    .edit_mode        = true,
    .drum_select_mode = false,
};

static u8g2_t *s_u8g2 = NULL;

/* Mirror the UI's grid of active steps into the audio core so the core
 * schedules notes for every step the user has toggled on. Only "on" steps are
 * pushed; "off" steps are the core's default after a fresh layer add. */
static void sync_layer_to_core(uint8_t li)
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

#if !CONFIG_SEQ_PATCH_BROWSE_FULL_RANGE
static int sequencer_patch_cycle_index_for(uint16_t patch)
{
    for (int i = 0; i < SEQ_RUNTIME_PATCH_COUNT; i++) {
        if (s_melodic_patch_cycle[i] == patch) {
            return i;
        }
    }
    return 0;
}
#endif

static void synth_ui_sync_melodic_patch_cache(void)
{
    uint16_t patch = sequencer_core_get_melodic_patch();
    for (uint8_t i = 0; i < seq_state.num_layers; i++) {
        if (seq_state.layers[i].type == SEQ_LAYER_MELODIC) {
            seq_state.layers[i].patch = patch;
        }
    }
}

/* ── Render-on-change support ────────────────────────────────────────────────
 * The OLED is full-buffer; a SendBuffer is ~20 ms of blocking I2C. At 20 Hz we
 * were redrawing unconditionally even when nothing changed. We compute a cheap
 * 32-bit FNV-1a signature of everything the frame depends on and only run the
 * Clear/draw/SendBuffer cycle when it differs from the last rendered one. */
#define FNV1A_OFFSET 2166136261u
#define FNV1A_PRIME  16777619u

static inline uint32_t fnv1a_bytes(uint32_t h, const void *data, size_t len)
{
    const uint8_t *b = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        h ^= b[i];
        h *= FNV1A_PRIME;
    }
    return h;
}

/* Signature of the sequencer view (everything display_seq_draw_frame reads). */
static uint32_t seq_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &seq_state.active_layer_idx, sizeof(seq_state.active_layer_idx));
    h = fnv1a_bytes(h, &seq_state.bpm, sizeof(seq_state.bpm));
    h = fnv1a_bytes(h, &seq_state.playing, sizeof(seq_state.playing));
    h = fnv1a_bytes(h, &seq_state.current_step, sizeof(seq_state.current_step));
    h = fnv1a_bytes(h, &seq_state.edit_mode, sizeof(seq_state.edit_mode));
    h = fnv1a_bytes(h, &seq_state.selected_track, sizeof(seq_state.selected_track));
    h = fnv1a_bytes(h, &seq_state.selected_step, sizeof(seq_state.selected_step));
    h = fnv1a_bytes(h, &seq_state.drum_select_mode, sizeof(seq_state.drum_select_mode));
    h = fnv1a_bytes(h, &seq_state.patch_select_mode, sizeof(seq_state.patch_select_mode));
    if (seq_state.num_layers > 0) {
        const seq_layer_t *L = &seq_state.layers[seq_state.active_layer_idx];
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

/* Forward decls for menu/arp/drone view builders used by the signature + task. */
static void menu_build_view(menu_view_t *out);
static void arp_build_view(arp_view_t *out);
static void drone_build_view(drone_view_t *out);
static uint32_t drone_view_signature(void);

/* Signature of the menu overlay. */
static uint32_t menu_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    menu_view_t v;
    menu_build_view(&v);
    h = fnv1a_bytes(h, &v.cursor, sizeof(v.cursor));
    h = fnv1a_bytes(h, &v.editing, sizeof(v.editing));
    for (uint8_t i = 0; i < v.count; i++) {
        h = fnv1a_bytes(h, v.items[i].label, sizeof(v.items[i].label));
        h = fnv1a_bytes(h, v.items[i].value, sizeof(v.items[i].value));
    }
    return h;
}

/* Signature of the arp screen. */
static uint32_t arp_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    arp_view_t v;
    arp_build_view(&v);
    h = fnv1a_bytes(h, &v.enabled, sizeof(v.enabled));
    h = fnv1a_bytes(h, &v.octaves, sizeof(v.octaves));
    h = fnv1a_bytes(h, &v.gate_pct, sizeof(v.gate_pct));
    h = fnv1a_bytes(h, &v.cursor, sizeof(v.cursor));
    h = fnv1a_bytes(h, &v.editing, sizeof(v.editing));
    h = fnv1a_bytes(h, &v.patch, sizeof(v.patch));
    h = fnv1a_bytes(h, &v.patch_select, sizeof(v.patch_select));
    h = fnv1a_bytes(h, v.rate_str, 4);
    h = fnv1a_bytes(h, v.mode_str, 4);
    for (uint8_t i = 0; i < ARP_VIEW_SLOTS; i++) {
        h = fnv1a_bytes(h, &v.slot_active[i], sizeof(v.slot_active[i]));
        h = fnv1a_bytes(h, v.slot_name[i], sizeof(v.slot_name[i]));
    }
    return h;
}

/* Signature of the graph editor (everything graph_popup_draw + the top bar read). */
static uint32_t graph_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &s_graph_popup.cursor, sizeof(s_graph_popup.cursor));
    h = fnv1a_bytes(h, &s_graph_popup.editing_value, sizeof(s_graph_popup.editing_value));
    h = fnv1a_bytes(h, &s_graph_popup.adjust_axis_y, sizeof(s_graph_popup.adjust_axis_y));
    h = fnv1a_bytes(h, &s_graph_long_range, sizeof(s_graph_long_range));
    h = fnv1a_bytes(h, &s_graph_layer, sizeof(s_graph_layer));
    h = fnv1a_bytes(h, &s_graph_track, sizeof(s_graph_track));
    h = fnv1a_bytes(h, s_graph_popup.points,
                    s_graph_popup.num_points * sizeof(gpopup_point_t));
    return h;
}

/* Draw the yellow context top bar (rows 0..15) for the envelope editor. */
static void graph_draw_topbar(u8g2_t *u8g2)
{
    char buf[24];

    /* Left: which row is being edited. */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    snprintf(buf, sizeof(buf), "L%u T%u ENV", s_graph_layer, s_graph_track);
    u8g2_DrawStr(u8g2, 2, 8, buf);

    /* Right: SHORT/LONG range flag. */
    const char *rng = s_graph_long_range ? "L" : "S";
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    uint8_t rw = (uint8_t)u8g2_GetStrWidth(u8g2, rng);
    u8g2_DrawStr(u8g2, (uint8_t)(128 - rw - 2), 8, rng);

    /* Middle: live readout of the selected point's real value (ms / %). */
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    uint8_t c = s_graph_popup.cursor;
    if (n >= 4 && c >= 1 && c <= 3) {
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
        /* Centre-ish, between the left label (~x=56) and the range flag. */
        int mx = 60 + (int)((128 - 60 - rw - 4 - tw) / 2);
        if (mx < 60) mx = 60;
        u8g2_DrawStr(u8g2, (uint8_t)mx, 8, buf);
    }

    /* Divider at the yellow/blue boundary. */
    u8g2_DrawHLine(u8g2, 0, GRAPH_TOPBAR_H - 1, 128);
}

static void synth_ui_task(void *pvParameters)
{
    (void)pvParameters;
    TickType_t last_wake_time = xTaskGetTickCount();
    const TickType_t delay = pdMS_TO_TICKS(50); /* 20 Hz */
    uint32_t last_sig = 0;
    /* Which top-level view was rendered last frame; a change forces a redraw. */
    enum { V_SEQ, V_ARP, V_MENU, V_GRAPH, V_DRONE } last_view = V_SEQ;
    for (;;) {
        /* Coalesced arp re-emit: setters mark the arp dirty; we perform at most
         * one full re-emit per frame here, collapsing fast encoder edits. */
        arp_core_service();
        /* Drone: advance the tempo-locked filter sweep + keep the LFO in sync.
         * Cheap no-op while the drone is disabled. */
        drone_core_service();

        seq_state.current_step =
            sequencer_core_get_current_step(seq_state.active_layer_idx);
        if (s_u8g2) {
            /* Precedence: graph editor > menu overlay > arp/drone screen > seq. */
            bool graph = graph_popup_is_active(&s_graph_popup);
            int view;
            uint32_t sig;
            if (graph) {
                view = V_GRAPH; sig = graph_view_signature();
            } else if (seq_state.menu_open) {
                view = V_MENU;  sig = menu_view_signature();
            } else if (seq_state.ui_mode == UI_MODE_ARP) {
                view = V_ARP;   sig = arp_view_signature();
            } else if (seq_state.ui_mode == UI_MODE_DRONE) {
                view = V_DRONE; sig = drone_view_signature();
            } else {
                view = V_SEQ;   sig = seq_view_signature();
            }
            bool force = s_force_redraw || (view != last_view);

            if (force || sig != last_sig) {
                switch (view) {
                    case V_GRAPH:
                        u8g2_ClearBuffer(s_u8g2);
                        u8g2_SetDrawColor(s_u8g2, 1);
                        graph_draw_topbar(s_u8g2);
                        graph_popup_draw(s_u8g2, &s_graph_popup);
                        u8g2_SendBuffer(s_u8g2);
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
                    default:
                        display_seq_draw_frame(s_u8g2, &seq_state);
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
    sequencer_core_init();
    SEQ_HEAP_CHECK("ui_init: after sequencer_core_init");
    arp_core_init();
    SEQ_HEAP_CHECK("ui_init: after arp_core_init");
    drone_core_init();
    SEQ_HEAP_CHECK("ui_init: after drone_core_init");

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
    drum->grid[2][2]  = drum->grid[2][6]  =
    drum->grid[2][10] = drum->grid[2][14] = true;   /* hats  */
    drum->grid[3][7]  = drum->grid[3][15] = true;   /* perc  */

    sync_layer_to_core(0);
    SEQ_HEAP_CHECK("ui_init: after sync_layer_to_core(0)");
    sequencer_core_set_playing(true);
    SEQ_HEAP_CHECK("ui_init: after set_playing");

    /* Pin to Core 0: the OLED refresh does blocking I2C and is not latency
     * critical, so keep it off Core 1 where the AMY DSP now runs. */
    xTaskCreatePinnedToCore(synth_ui_task, "seq_ui", 4096, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "Sequencer UI + Core initialized");
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
        layer->patch = sequencer_core_get_melodic_patch();
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
    ESP_LOGI(TAG, "UI layer %d added (type=%d steps=%d)",
             li, type, layer->num_steps);
    return li;
}

void synth_ui_cycle_active_layer(void)
{
    if (seq_state.num_layers <= 1) return;
    seq_state.active_layer_idx =
        (uint8_t)((seq_state.active_layer_idx + 1) % seq_state.num_layers);
    seq_state.selected_track = 0;
    seq_state.selected_step  = 0;
    seq_state.edit_mode      = true;
    ESP_LOGI(TAG, "Active layer -> %d (%s)",
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
        synth_ui_set_bpm((uint16_t)((int)seq_state.bpm + (int)delta));
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
    ESP_LOGI(TAG, "Playback %s", seq_state.playing ? "started" : "stopped");
}

void synth_ui_set_bpm(uint16_t bpm)
{
    bpm = SEQ_CLAMP_U16(bpm, 40, 300);
    seq_state.bpm = bpm;
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

/* Shared patch-cycle stepping used by both the melodic layers and the arp.
 * Returns the next patch number after `current` walked `dir` (±1) steps through
 * the active browse mode (curated shortlist or full 0..256 range), wrapping at
 * the ends. One patch per call regardless of encoder sub-steps. */
static uint16_t next_patch_in_cycle(uint16_t current, int dir)
{
    dir = (dir > 0) ? 1 : -1;
#if CONFIG_SEQ_PATCH_BROWSE_FULL_RANGE
    int n = SEQ_PATCH_FULL_MAX + 1;          /* inclusive count */
    int cur = (int)current;
    if (cur < 0 || cur > SEQ_PATCH_FULL_MAX) cur = 0;  /* off-range -> start */
    int idx = (cur + dir + n) % n;
    return (uint16_t)idx;
#else
    int n = SEQ_RUNTIME_PATCH_COUNT;
    int idx = sequencer_patch_cycle_index_for(current);
    int ni = (idx + dir + n) % n;
    return s_melodic_patch_cycle[ni];
#endif
}

void synth_ui_cycle_melodic_patch(int delta)
{
    if (delta == 0) return;

    uint8_t li = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return;
    if (seq_state.layers[li].type != SEQ_LAYER_MELODIC) return;

    int dir = (delta > 0) ? 1 : -1;
    uint16_t next = next_patch_in_cycle(sequencer_core_get_melodic_patch(), dir);

    sequencer_core_set_melodic_patch(next);
    synth_ui_sync_melodic_patch_cache();

    uint16_t applied = sequencer_core_get_melodic_patch();
    const char *name = patch_name_for(applied);
    if (name) {
        ESP_LOGI(TAG, "melodic patch cycle -> %u (%s)", (unsigned)applied, name);
    } else {
        ESP_LOGI(TAG, "melodic patch cycle -> %u", (unsigned)applied);
    }
}

/* Cycle the SELECTED drum track's patch through the curated drum list. The drum
 * layer is per-track, so this targets seq_state.selected_track on the active
 * layer (must be a drum layer). Mirrors the applied patch into the UI copy so
 * the on-screen patch number updates. */
void synth_ui_cycle_drum_patch(int delta)
{
    if (delta == 0) return;

    uint8_t li = seq_state.active_layer_idx;
    if (li >= seq_state.num_layers) return;
    if (seq_state.layers[li].type != SEQ_LAYER_DRUM) return;

    uint8_t track = seq_state.selected_track;
    if (track >= SEQ_TRACKS) return;

    int dir = (delta > 0) ? 1 : -1;
    uint16_t applied = sequencer_core_cycle_drum_patch(li, track, dir);

    /* Keep the UI mirror in sync (track_patch[] drives the label). */
    seq_state.layers[li].track_patch[track] = applied;
    if (track == 0) seq_state.layers[li].patch = applied;

    const char *name = patch_name_for(applied);
    if (name) {
        ESP_LOGI(TAG, "drum patch cycle L%u t%u -> %u (%s)",
                 li, track, (unsigned)applied, name);
    } else {
        ESP_LOGI(TAG, "drum patch cycle L%u t%u -> %u", li, track, (unsigned)applied);
    }
}

/* Cycle the arp's OWN patch (independent of the sequencer's melodic patch).
 * Reuses the same browse-mode stepping + name lookup as the sequencer. */
void synth_ui_arp_cycle_patch(int delta)
{
    if (delta == 0) return;
    int dir = (delta > 0) ? 1 : -1;
    uint16_t next = next_patch_in_cycle(arp_get_patch(), dir);
    arp_set_patch(next);

    const char *name = patch_name_for(next);
    if (name) {
        ESP_LOGI(TAG, "arp patch cycle -> %u (%s)", (unsigned)next, name);
    } else {
        ESP_LOGI(TAG, "arp patch cycle -> %u", (unsigned)next);
    }
}

/* Cycle the drone's PATCH-mode preset (hold+turn gesture on the drone screen).
 * Reuses the same browse-mode stepping + name lookup as the others. */
void synth_ui_drone_cycle_patch(int delta)
{
    if (delta == 0) return;
    int dir = (delta > 0) ? 1 : -1;
    uint16_t next = next_patch_in_cycle(drone_get_patch(), dir);
    drone_set_patch(next);

    const char *name = patch_name_for(next);
    if (name) {
        ESP_LOGI(TAG, "drone patch cycle -> %u (%s)", (unsigned)next, name);
    } else {
        ESP_LOGI(TAG, "drone patch cycle -> %u", (unsigned)next);
    }
    s_force_redraw = true;
}

/* ── Note-name helper (local; mirrors display_seq.c's static one) ─────── */
static void ui_note_name(uint8_t midi_note, char buf[4])
{
    static const char *const names[] = {
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
    };
    int octave = (int)midi_note / 12 - 1;
    snprintf(buf, 4, "%s%d", names[midi_note % 12], octave);
}

/* ════════════════════════════════════════════════════════════════════════
 *  GLOBAL FX (EQ / Echo / Chorus / Reverb)
 * ════════════════════════════════════════════════════════════════════════
 * These are built-in AMY global effects driven purely by setting amy_event
 * fields and queuing the event (amy_add_event). AMY exposes no getters for the
 * current values, so we cache what we set here for the menu display. All sends
 * use a local scratch event guarded by a mutex (amy_event is ~800 B; never on a
 * task stack). */
typedef struct {
    int8_t  eq_low_db;     /* -15..+15 dB */
    int8_t  eq_mid_db;
    int8_t  eq_high_db;
    uint8_t echo_level;    /* 0..100 -> 0..1 */
    uint8_t chorus_level;  /* 0..100 -> 0..1 */
    uint8_t reverb_level;  /* 0..100 -> 0..1 */
    /* When false, loading a synth patch must NOT change the global FX: every
     * AMY built-in Juno patch string ends with `x<eq>k<chorus>` commands that
     * write the single global EQ/chorus (amy_global.*), so without this guard a
     * preset change on any one synth (sequencer row, arp, drone) re-skins the
     * whole mix's FX. The patch-load sites call synth_ui_fx_reassert_global()
     * which, when this is false, re-imposes these cached user values right after
     * the patch's FX deltas, making presets effectively per-synth (timbre only).
     * When true, the most-recently-loaded preset's FX is allowed to apply
     * globally (the original behaviour). */
    bool    presets_alter_global;
} fx_state_t;

static fx_state_t s_fx = {
    .eq_low_db = 0, .eq_mid_db = 0, .eq_high_db = 0,
    .echo_level = 0, .chorus_level = 0, .reverb_level = 0,
    .presets_alter_global = false,
};

static amy_event         s_fx_ev;
static SemaphoreHandle_t s_fx_ev_mutex = NULL;

static void fx_ensure_mutex(void)
{
    if (s_fx_ev_mutex == NULL) s_fx_ev_mutex = xSemaphoreCreateMutex();
}

static void fx_push_eq(void)
{
    fx_ensure_mutex();
    xSemaphoreTake(s_fx_ev_mutex, portMAX_DELAY);
    s_fx_ev = amy_default_event();
    s_fx_ev.eq_l = (float)s_fx.eq_low_db;   /* AMY interprets these as dB */
    s_fx_ev.eq_m = (float)s_fx.eq_mid_db;
    s_fx_ev.eq_h = (float)s_fx.eq_high_db;
    amy_add_event(&s_fx_ev);
    xSemaphoreGive(s_fx_ev_mutex);
}

static void fx_push_echo(void)
{
    fx_ensure_mutex();
    xSemaphoreTake(s_fx_ev_mutex, portMAX_DELAY);
    s_fx_ev = amy_default_event();
    s_fx_ev.echo_level = (float)s_fx.echo_level / 100.0f;
    amy_add_event(&s_fx_ev);
    xSemaphoreGive(s_fx_ev_mutex);
}

static void fx_push_chorus(void)
{
    fx_ensure_mutex();
    xSemaphoreTake(s_fx_ev_mutex, portMAX_DELAY);
    s_fx_ev = amy_default_event();
    s_fx_ev.chorus_level = (float)s_fx.chorus_level / 100.0f;
    amy_add_event(&s_fx_ev);
    xSemaphoreGive(s_fx_ev_mutex);
}

static void fx_push_reverb(void)
{
    fx_ensure_mutex();
    xSemaphoreTake(s_fx_ev_mutex, portMAX_DELAY);
    s_fx_ev = amy_default_event();
    s_fx_ev.reverb_level = (float)s_fx.reverb_level / 100.0f;
    amy_add_event(&s_fx_ev);
    xSemaphoreGive(s_fx_ev_mutex);
}

/* Re-impose the cached global FX after a patch load so a synth's preset cannot
 * hijack the shared EQ/chorus/echo/reverb. No-op when the user has opted into
 * letting presets drive global FX. Safe to call from the sequencer/arp/drone
 * task contexts: each fx_push_* takes s_fx_ev_mutex internally.
 *
 * Cost is four queued events per patch change (a rare, user-driven action) and
 * zero in the render loop. The reassert events are queued AFTER the patch's own
 * FX deltas, so they win; both drain in the same render quantum on Core 1, so
 * there is no audible blip. */
void synth_ui_fx_reassert_global(void)
{
    if (s_fx.presets_alter_global) return;
    fx_push_eq();
    fx_push_chorus();
    fx_push_echo();
    fx_push_reverb();
}

/* ════════════════════════════════════════════════════════════════════════
 *  MENU OVERLAY
 * ════════════════════════════════════════════════════════════════════════
 * A small modal list. Items are either ACTIONS (run on click, no value) or
 * VALUE items (click to enter editing, encoder changes the value, click to
 * exit). The model is a static table; values are read/written live from the
 * sequencer_core quantizer + arp_core + global FX cache. */

typedef enum {
    MI_SCREEN_SEQ = 0,
    MI_SCREEN_ARP,
    MI_SCREEN_DRONE,
    MI_BPM,
    MI_QUANT_ENABLED,
    MI_QUANT_SCALE,
    MI_QUANT_ROOT,
    MI_ARP_ENABLED,
    MI_DRONE_ENABLED,
    MI_DRUM_ENGINE,
    MI_EQ_LOW,
    MI_EQ_MID,
    MI_EQ_HIGH,
    MI_ECHO_LEVEL,
    MI_CHORUS_LEVEL,
    MI_REVERB_LEVEL,
    MI_PRESET_GLOBAL_FX,
    MI_COUNT
} menu_item_id_t;

static menu_item_view_t s_menu_items[MI_COUNT];

/* Format the current value of each menu item into the flat view array. */
static void menu_build_view(menu_view_t *out)
{
    for (uint8_t i = 0; i < MI_COUNT; i++) {
        s_menu_items[i].value[0] = '\0';
    }

    snprintf(s_menu_items[MI_SCREEN_SEQ].label, MENU_LABEL_LEN, "Screen: Seq");
    snprintf(s_menu_items[MI_SCREEN_ARP].label, MENU_LABEL_LEN, "Screen: Arp");
    snprintf(s_menu_items[MI_SCREEN_DRONE].label, MENU_LABEL_LEN, "Screen: Drone");

    snprintf(s_menu_items[MI_BPM].label, MENU_LABEL_LEN, "BPM");
    snprintf(s_menu_items[MI_BPM].value, MENU_VALUE_LEN, "%u",
             (unsigned)seq_state.bpm);

    snprintf(s_menu_items[MI_QUANT_ENABLED].label, MENU_LABEL_LEN, "Quant");
    snprintf(s_menu_items[MI_QUANT_ENABLED].value, MENU_VALUE_LEN, "%s",
             sequencer_core_get_quantizer_enabled() ? "ON" : "OFF");

    snprintf(s_menu_items[MI_QUANT_SCALE].label, MENU_LABEL_LEN, "Scale");
    {
        const musical_scale_t *sc =
            quantizer_get_scale(sequencer_core_get_quantizer_scale());
        snprintf(s_menu_items[MI_QUANT_SCALE].value, MENU_VALUE_LEN, "%s",
                 sc ? sc->name : "?");
    }

    snprintf(s_menu_items[MI_QUANT_ROOT].label, MENU_LABEL_LEN, "Root");
    {
        char nn[4];
        ui_note_name(sequencer_core_get_quantizer_root_note(), nn);
        snprintf(s_menu_items[MI_QUANT_ROOT].value, MENU_VALUE_LEN, "%s", nn);
    }

    snprintf(s_menu_items[MI_ARP_ENABLED].label, MENU_LABEL_LEN, "Arp");
    snprintf(s_menu_items[MI_ARP_ENABLED].value, MENU_VALUE_LEN, "%s",
             arp_get_enabled() ? "ON" : "OFF");

    snprintf(s_menu_items[MI_DRONE_ENABLED].label, MENU_LABEL_LEN, "Drone");
    snprintf(s_menu_items[MI_DRONE_ENABLED].value, MENU_VALUE_LEN, "%s",
             drone_get_enabled() ? "ON" : "OFF");

    snprintf(s_menu_items[MI_DRUM_ENGINE].label, MENU_LABEL_LEN, "Drum Mode");
    snprintf(s_menu_items[MI_DRUM_ENGINE].value, MENU_VALUE_LEN, "%s",
             sequencer_core_get_drum_engine() == SEQ_DRUM_PCM ? "PCM" : "Synth");

    /* Global FX (cached values; AMY has no getters). */
    snprintf(s_menu_items[MI_EQ_LOW].label, MENU_LABEL_LEN, "EQ Low");
    snprintf(s_menu_items[MI_EQ_LOW].value, MENU_VALUE_LEN, "%+ddB",
             (int)s_fx.eq_low_db);
    snprintf(s_menu_items[MI_EQ_MID].label, MENU_LABEL_LEN, "EQ Mid");
    snprintf(s_menu_items[MI_EQ_MID].value, MENU_VALUE_LEN, "%+ddB",
             (int)s_fx.eq_mid_db);
    snprintf(s_menu_items[MI_EQ_HIGH].label, MENU_LABEL_LEN, "EQ High");
    snprintf(s_menu_items[MI_EQ_HIGH].value, MENU_VALUE_LEN, "%+ddB",
             (int)s_fx.eq_high_db);
    snprintf(s_menu_items[MI_ECHO_LEVEL].label, MENU_LABEL_LEN, "Echo");
    snprintf(s_menu_items[MI_ECHO_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)s_fx.echo_level);
    snprintf(s_menu_items[MI_CHORUS_LEVEL].label, MENU_LABEL_LEN, "Chorus");
    snprintf(s_menu_items[MI_CHORUS_LEVEL].value, MENU_VALUE_LEN, "%u%%",
             (unsigned)s_fx.chorus_level);
    snprintf(s_menu_items[MI_REVERB_LEVEL].label, MENU_LABEL_LEN, "Reverb");
    /* If AMY could not allocate the reverb delay lines (OOM), it forces reverb
     * off and flags it here. Surface "OOM!" instead of a percentage so the
     * failure is visible on the OLED without a serial monitor. */
    if (amy_reverb_alloc_failed())
        snprintf(s_menu_items[MI_REVERB_LEVEL].value, MENU_VALUE_LEN, "OOM!");
    else
        snprintf(s_menu_items[MI_REVERB_LEVEL].value, MENU_VALUE_LEN, "%u%%",
                 (unsigned)s_fx.reverb_level);

    /* "Presets alter global FX? y/n" — OFF makes Juno presets per-synth. */
    snprintf(s_menu_items[MI_PRESET_GLOBAL_FX].label, MENU_LABEL_LEN, "Preset FX");
    snprintf(s_menu_items[MI_PRESET_GLOBAL_FX].value, MENU_VALUE_LEN, "%s",
             s_fx.presets_alter_global ? "ON" : "OFF");

    out->items   = s_menu_items;
    out->count   = MI_COUNT;
    out->cursor  = seq_state.menu_cursor;
    out->editing = seq_state.menu_editing;
}

/* True for items that hold an adjustable value (vs. one-shot actions). */
static bool menu_item_is_value(menu_item_id_t id)
{
    switch (id) {
        case MI_BPM:
        case MI_QUANT_ENABLED:
        case MI_QUANT_SCALE:
        case MI_QUANT_ROOT:
        case MI_ARP_ENABLED:
        case MI_DRONE_ENABLED:
        case MI_DRUM_ENGINE:
        case MI_EQ_LOW:
        case MI_EQ_MID:
        case MI_EQ_HIGH:
        case MI_ECHO_LEVEL:
        case MI_CHORUS_LEVEL:
        case MI_REVERB_LEVEL:
        case MI_PRESET_GLOBAL_FX:
            return true;
        default:
            return false;
    }
}

/* Apply an encoder delta to the currently-entered menu value. */
static void menu_edit_value(menu_item_id_t id, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    switch (id) {
        case MI_BPM:
            synth_ui_set_bpm((uint16_t)((int)seq_state.bpm + delta));
            break;
        case MI_QUANT_ENABLED:
            if (dir != 0)
                sequencer_core_set_quantizer_enabled(
                    !sequencer_core_get_quantizer_enabled());
            break;
        case MI_QUANT_SCALE: {
            int n = (int)quantizer_scale_count();
            int cur = (int)sequencer_core_get_quantizer_scale();
            int ni = (cur + dir + n) % n;
            sequencer_core_set_quantizer_scale((uint8_t)ni);
            break;
        }
        case MI_QUANT_ROOT: {
            int r = (int)sequencer_core_get_quantizer_root_note() + delta;
            r = SEQ_CLAMP_INT(r, 0, 127);
            sequencer_core_set_quantizer_root_note((uint8_t)r);
            break;
        }
        case MI_ARP_ENABLED:
            if (dir != 0) arp_set_enabled(!arp_get_enabled());
            break;
        case MI_DRONE_ENABLED:
            if (dir != 0) drone_set_enabled(!drone_get_enabled());
            break;
        case MI_DRUM_ENGINE:
            if (dir != 0) {
                sequencer_core_set_drum_engine(
                    sequencer_core_get_drum_engine() == SEQ_DRUM_PCM
                        ? SEQ_DRUM_SYNTH : SEQ_DRUM_PCM);
            }
            break;
        case MI_EQ_LOW: {
            int v = SEQ_CLAMP_INT((int)s_fx.eq_low_db + dir, -15, 15);
            s_fx.eq_low_db = (int8_t)v; fx_push_eq();
            break;
        }
        case MI_EQ_MID: {
            int v = SEQ_CLAMP_INT((int)s_fx.eq_mid_db + dir, -15, 15);
            s_fx.eq_mid_db = (int8_t)v; fx_push_eq();
            break;
        }
        case MI_EQ_HIGH: {
            int v = SEQ_CLAMP_INT((int)s_fx.eq_high_db + dir, -15, 15);
            s_fx.eq_high_db = (int8_t)v; fx_push_eq();
            break;
        }
        case MI_ECHO_LEVEL: {
            int v = SEQ_CLAMP_INT((int)s_fx.echo_level + dir * 5, 0, 100);
            s_fx.echo_level = (uint8_t)v; fx_push_echo();
            break;
        }
        case MI_CHORUS_LEVEL: {
            int v = SEQ_CLAMP_INT((int)s_fx.chorus_level + dir * 5, 0, 100);
            s_fx.chorus_level = (uint8_t)v; fx_push_chorus();
            break;
        }
        case MI_REVERB_LEVEL: {
            int v = SEQ_CLAMP_INT((int)s_fx.reverb_level + dir * 5, 0, 100);
            s_fx.reverb_level = (uint8_t)v; fx_push_reverb();
            break;
        }
        case MI_PRESET_GLOBAL_FX:
            if (dir != 0) {
                s_fx.presets_alter_global = !s_fx.presets_alter_global;
                /* Turning the guard back ON re-imposes the user's cached FX
                 * immediately, undoing whatever the last preset left behind. */
                if (!s_fx.presets_alter_global) synth_ui_fx_reassert_global();
            }
            break;
        default:
            break;
    }
}

void synth_ui_menu_toggle(void)
{
    /* The graph editor is the top overlay; don't let the menu fight it. */
    if (synth_ui_graph_is_active()) return;
    seq_state.menu_open    = !seq_state.menu_open;
    seq_state.menu_editing = false;
    if (seq_state.menu_open && seq_state.menu_cursor >= MI_COUNT) {
        seq_state.menu_cursor = 0;
    }
    s_force_redraw = true;
    ESP_LOGI(TAG, "menu %s", seq_state.menu_open ? "open" : "closed");
}

bool synth_ui_menu_is_active(void)
{
    return seq_state.menu_open;
}

bool synth_ui_menu_handle_encoder(long delta)
{
    if (!seq_state.menu_open) return false;
    if (delta == 0) return true;

    if (seq_state.menu_editing) {
        menu_edit_value((menu_item_id_t)seq_state.menu_cursor, (int)delta);
    } else {
        int n = (int)MI_COUNT;
        int c = (int)seq_state.menu_cursor + (int)delta;
        /* clamp (no wrap) so the list feels bounded */
        c = SEQ_CLAMP_INT(c, 0, n - 1);
        seq_state.menu_cursor = (uint8_t)c;
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_menu_handle_button(void)
{
    if (!seq_state.menu_open) return false;

    menu_item_id_t id = (menu_item_id_t)seq_state.menu_cursor;

    if (menu_item_is_value(id)) {
        /* Toggle in/out of editing this value. */
        seq_state.menu_editing = !seq_state.menu_editing;
    } else {
        /* Action item: run it and close the menu. */
        switch (id) {
            case MI_SCREEN_SEQ:
                seq_state.ui_mode = UI_MODE_SEQUENCER;
                seq_state.menu_open = false;
                break;
            case MI_SCREEN_ARP:
                seq_state.ui_mode = UI_MODE_ARP;
                seq_state.menu_open = false;
                break;
            case MI_SCREEN_DRONE:
                seq_state.ui_mode = UI_MODE_DRONE;
                seq_state.menu_open = false;
                break;
            default:
                break;
        }
        seq_state.menu_editing = false;
    }
    s_force_redraw = true;
    return true;
}

/* ════════════════════════════════════════════════════════════════════════
 *  ARP SCREEN
 * ════════════════════════════════════════════════════════════════════════ */

bool synth_ui_arp_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_ARP
        && !seq_state.menu_open
        && !synth_ui_graph_is_active();
}

/* The arp screen keeps its own cursor + editing flags, independent of the
 * menu's. We stash them in file-static state (the screen is a singleton). */
static uint8_t s_arp_cursor  = ARP_CUR_ENABLE;
static bool    s_arp_editing = false;

/* Build the flat arp view from arp_core for the renderer. */
static void arp_build_view(arp_view_t *out)
{
    out->enabled  = arp_get_enabled();
    out->mode_str = (arp_get_direction() == ARP_DOWN) ? "DOWN" : "UP";
    out->octaves  = arp_get_octaves();
    out->rate_str = arp_rate_name(arp_get_rate());
    out->gate_pct = arp_get_gate_pct();
    for (uint8_t i = 0; i < ARP_VIEW_SLOTS; i++) {
        int16_t snapped = arp_get_slot_snapped(i);
        if (snapped >= 0) {
            out->slot_active[i] = true;
            ui_note_name((uint8_t)snapped, out->slot_name[i]);
        } else {
            out->slot_active[i] = false;
            out->slot_name[i][0] = '\0';
        }
    }
    out->cursor  = s_arp_cursor;
    out->editing = s_arp_editing;

    /* Patch indicator: mirror the sequencer view. Number is always available;
     * the name banner shows only while the patch hold+turn gesture is active. */
    out->patch        = arp_get_patch();
    out->patch_select = seq_state.patch_select_mode;
    out->patch_name   = patch_name_for(out->patch);
}

static void arp_edit_value(uint8_t cursor, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    switch (cursor) {
        case ARP_CUR_ENABLE:
            if (dir != 0) arp_set_enabled(!arp_get_enabled());
            break;
        case ARP_CUR_MODE:
            if (dir != 0)
                arp_set_direction(arp_get_direction() == ARP_UP ? ARP_DOWN : ARP_UP);
            break;
        case ARP_CUR_OCT:
            arp_set_octaves((uint8_t)SEQ_CLAMP_INT(
                (int)arp_get_octaves() + dir, 1, ARP_OCT_MAX));
            break;
        case ARP_CUR_RATE: {
            int r = (int)arp_get_rate() + dir;
            r = SEQ_CLAMP_INT(r, 0, ARP_RATE_COUNT - 1);
            arp_set_rate((arp_rate_t)r);
            break;
        }
        case ARP_CUR_GATE:
            arp_set_gate_pct((uint8_t)SEQ_CLAMP_INT(
                (int)arp_get_gate_pct() + dir * 5, 10, 100));
            break;
        default: {
            /* Slot edit: chromatic note, or clear below the floor. */
            uint8_t slot = (uint8_t)(cursor - ARP_CUR_SLOT0);
            if (slot >= ARP_VIEW_SLOTS) break;
            int16_t cur = arp_get_slot(slot);
            if (cur < 0) {
                /* Empty: first turn seeds from the arp root note. */
                if (dir > 0) arp_set_slot(slot, (int16_t)arp_get_root_note());
            } else {
                int nv = (int)cur + dir;
                if (nv < 24) {
                    arp_set_slot(slot, -1);   /* turn below floor clears slot */
                } else {
                    arp_set_slot(slot, (int16_t)nv);
                }
            }
            break;
        }
    }
}

void synth_ui_arp_handle_encoder(long delta)
{
    if (delta == 0) return;
    if (s_arp_editing) {
        arp_edit_value(s_arp_cursor, (int)delta);
    } else {
        int c = (int)s_arp_cursor + (int)delta;
        c = SEQ_CLAMP_INT(c, 0, ARP_CUR_COUNT - 1);
        s_arp_cursor = (uint8_t)c;
    }
    s_force_redraw = true;
}

void synth_ui_arp_handle_button(void)
{
    s_arp_editing = !s_arp_editing;
    s_force_redraw = true;
}

/* ════════════════════════════════════════════════════════════════════════
 *  DRONE SCREEN
 * ════════════════════════════════════════════════════════════════════════
 * Standalone stutter-drone synth (custompatches/drone_core). A simple
 * scrollable parameter list. Rows shown depend on the WAVE/PATCH source: the
 * WAVE/FREQ rows are WAVE-only, the PATCH row is PATCH-only. The cursor walks
 * the currently-visible rows; encoder-click toggles edit; turning edits the
 * focused row's value. */

/* Logical rows (superset). visible_rows() filters by source each frame. */
typedef enum {
    DROW_ENABLE = 0,
    DROW_SOURCE,
    DROW_WAVE,        /* WAVE only */
    DROW_CHORD,
    DROW_RES,
    DROW_CONST,       /* WAVE only */
    DROW_MOD,         /* WAVE only */
    DROW_RATE,        /* WAVE only */
    DROW_SWEEP_LO,
    DROW_SWEEP_HI,
    DROW_SWEEP_BARS,
    DROW_SUB,
    DROW_SUB_INTVL,
    DROW_PATCH,       /* PATCH only */
    DROW_ALL_COUNT
} drone_logical_row_t;

static uint8_t s_drone_cursor  = 0;   /* index into the visible-row list */
static bool    s_drone_editing = false;

/* Fill `out` with the logical rows visible for the current source; return count.
 * out[] must hold at least DROW_ALL_COUNT entries. */
static uint8_t drone_visible_rows(drone_logical_row_t out[DROW_ALL_COUNT])
{
    bool wave = (drone_get_source() == DRONE_SRC_WAVE);
    uint8_t n = 0;
    for (drone_logical_row_t r = 0; r < DROW_ALL_COUNT; r++) {
        if (wave && r == DROW_PATCH) continue;
        if (!wave && (r == DROW_WAVE || r == DROW_CONST || r == DROW_MOD
                      || r == DROW_RATE)) continue;
        out[n++] = r;
    }
    return n;
}

static void drone_row_label_value(drone_logical_row_t r,
                                   char label[DRONE_LABEL_LEN],
                                   char value[DRONE_VALUE_LEN])
{
    switch (r) {
        case DROW_ENABLE:
            snprintf(label, DRONE_LABEL_LEN, "DRONE");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_get_enabled() ? "ON" : "OFF");
            break;
        case DROW_SOURCE:
            snprintf(label, DRONE_LABEL_LEN, "SOURCE");
            snprintf(value, DRONE_VALUE_LEN, "%s",
                     drone_get_source() == DRONE_SRC_PATCH ? "PATCH" : "WAVE");
            break;
        case DROW_WAVE:
            snprintf(label, DRONE_LABEL_LEN, "WAVE");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_wave_name(drone_get_wave()));
            break;
        case DROW_CHORD:
            snprintf(label, DRONE_LABEL_LEN, "CHORD");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_chord_name(drone_get_chord()));
            break;
        case DROW_RES:
            snprintf(label, DRONE_LABEL_LEN, "RES");
            snprintf(value, DRONE_VALUE_LEN, "%.2f", (double)drone_get_resonance());
            break;
        case DROW_CONST:
            snprintf(label, DRONE_LABEL_LEN, "CONST");
            snprintf(value, DRONE_VALUE_LEN, "%.1f", (double)drone_get_amp_const());
            break;
        case DROW_MOD:
            snprintf(label, DRONE_LABEL_LEN, "MOD");
            snprintf(value, DRONE_VALUE_LEN, "%.1f", (double)drone_get_amp_mod());
            break;
        case DROW_RATE:
            snprintf(label, DRONE_LABEL_LEN, "STUTTER");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_rate_name(drone_get_rate()));
            break;
        case DROW_SWEEP_LO:
            snprintf(label, DRONE_LABEL_LEN, "SWEEP LO");
            snprintf(value, DRONE_VALUE_LEN, "%dHz", (int)(drone_get_sweep_lo() + 0.5f));
            break;
        case DROW_SWEEP_HI:
            snprintf(label, DRONE_LABEL_LEN, "SWEEP HI");
            snprintf(value, DRONE_VALUE_LEN, "%dHz", (int)(drone_get_sweep_hi() + 0.5f));
            break;
        case DROW_SWEEP_BARS:
            snprintf(label, DRONE_LABEL_LEN, "SWEEP SPD");
            snprintf(value, DRONE_VALUE_LEN, "%ubar", (unsigned)drone_get_sweep_bars());
            break;
        case DROW_SUB:
            snprintf(label, DRONE_LABEL_LEN, "SUB");
            snprintf(value, DRONE_VALUE_LEN, "%s", drone_get_sub_enabled() ? "ON" : "OFF");
            break;
        case DROW_SUB_INTVL:
            snprintf(label, DRONE_LABEL_LEN, "SUB INT");
            snprintf(value, DRONE_VALUE_LEN, "%d", (int)drone_get_sub_interval());
            break;
        case DROW_PATCH:
            snprintf(label, DRONE_LABEL_LEN, "PATCH");
            snprintf(value, DRONE_VALUE_LEN, "%u", (unsigned)drone_get_patch());
            break;
        default:
            label[0] = '\0'; value[0] = '\0';
            break;
    }
}

/* Static backing store for the flat view rows the renderer reads. */
static drone_row_view_t s_drone_rows[DROW_ALL_COUNT];

static void drone_build_view(drone_view_t *out)
{
    drone_logical_row_t vis[DROW_ALL_COUNT];
    uint8_t n = drone_visible_rows(vis);
    for (uint8_t i = 0; i < n; i++) {
        drone_row_label_value(vis[i], s_drone_rows[i].label, s_drone_rows[i].value);
    }
    if (s_drone_cursor >= n) s_drone_cursor = (n ? (uint8_t)(n - 1) : 0);
    out->rows    = s_drone_rows;
    out->count   = n;
    out->cursor  = s_drone_cursor;
    out->editing = s_drone_editing;
}

static void drone_edit_row(drone_logical_row_t r, int delta)
{
    int dir = (delta > 0) ? 1 : (delta < 0 ? -1 : 0);
    switch (r) {
        case DROW_ENABLE:
            if (dir != 0) drone_set_enabled(!drone_get_enabled());
            break;
        case DROW_SOURCE:
            if (dir != 0)
                drone_set_source(drone_get_source() == DRONE_SRC_WAVE
                                 ? DRONE_SRC_PATCH : DRONE_SRC_WAVE);
            break;
        case DROW_WAVE: {
            /* Cycle through SAW_DOWN, SAW_UP, PULSE, TRIANGLE, SINE. */
            static const uint16_t waves[] = { SAW_DOWN, SAW_UP, PULSE, TRIANGLE, SINE };
            const int wn = (int)(sizeof(waves) / sizeof(waves[0]));
            int idx = 0;
            for (int i = 0; i < wn; i++) if (waves[i] == drone_get_wave()) { idx = i; break; }
            idx = (idx + dir + wn) % wn;
            drone_set_wave(waves[idx]);
            break;
        }
        case DROW_CHORD: {
            int c = SEQ_CLAMP_INT((int)drone_get_chord() + dir,
                                  0, DRONE_CHORD_COUNT - 1);
            drone_set_chord((drone_chord_t)c);
            break;
        }
        case DROW_RES:
            drone_set_resonance(drone_get_resonance() + (float)dir * 0.05f);
            break;
        case DROW_CONST:
            drone_set_amp_const(drone_get_amp_const() + (float)dir * 0.1f);
            break;
        case DROW_MOD:
            drone_set_amp_mod(drone_get_amp_mod() + (float)dir * 0.1f);
            break;
        case DROW_RATE: {
            int v = SEQ_CLAMP_INT((int)drone_get_rate() + dir, 0, DRONE_RATE_COUNT - 1);
            drone_set_rate((drone_rate_t)v);
            break;
        }
        case DROW_SWEEP_LO:
            drone_set_sweep_lo(drone_get_sweep_lo() + (float)dir * 25.0f);
            break;
        case DROW_SWEEP_HI:
            drone_set_sweep_hi(drone_get_sweep_hi() + (float)dir * 25.0f);
            break;
        case DROW_SWEEP_BARS:
            drone_set_sweep_bars((uint8_t)SEQ_CLAMP_INT(
                (int)drone_get_sweep_bars() + dir, 1, 16));
            break;
        case DROW_SUB:
            if (dir != 0) drone_set_sub_enabled(!drone_get_sub_enabled());
            break;
        case DROW_SUB_INTVL:
            drone_set_sub_interval((int8_t)SEQ_CLAMP_INT(
                (int)drone_get_sub_interval() + dir, -36, 0));
            break;
        case DROW_PATCH:
            synth_ui_drone_cycle_patch(dir);
            break;
        default:
            break;
    }
}

bool synth_ui_drone_is_active(void)
{
    return seq_state.ui_mode == UI_MODE_DRONE
        && !seq_state.menu_open
        && !synth_ui_graph_is_active();
}

void synth_ui_drone_handle_encoder(long delta)
{
    if (delta == 0) return;
    drone_logical_row_t vis[DROW_ALL_COUNT];
    uint8_t n = drone_visible_rows(vis);
    if (n == 0) return;
    if (s_drone_cursor >= n) s_drone_cursor = (uint8_t)(n - 1);

    if (s_drone_editing) {
        drone_edit_row(vis[s_drone_cursor], (int)delta);
    } else {
        int c = (int)s_drone_cursor + (int)delta;
        c = SEQ_CLAMP_INT(c, 0, (int)n - 1);
        s_drone_cursor = (uint8_t)c;
    }
    s_force_redraw = true;
}

void synth_ui_drone_handle_button(void)
{
    s_drone_editing = !s_drone_editing;
    s_force_redraw = true;
}

/* Signature of the drone screen (everything the renderer reads). */
static uint32_t drone_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    drone_view_t v;
    drone_build_view(&v);
    h = fnv1a_bytes(h, &v.cursor, sizeof(v.cursor));
    h = fnv1a_bytes(h, &v.editing, sizeof(v.editing));
    h = fnv1a_bytes(h, &v.count, sizeof(v.count));
    for (uint8_t i = 0; i < v.count; i++) {
        h = fnv1a_bytes(h, v.rows[i].label, sizeof(v.rows[i].label));
        h = fnv1a_bytes(h, v.rows[i].value, sizeof(v.rows[i].value));
    }
    return h;
}


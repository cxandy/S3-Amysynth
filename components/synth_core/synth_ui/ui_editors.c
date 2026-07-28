#include "synth_ui/synth_ui_internal.h"
#include "synth_ui.h"
#include "sequencer_core.h"
#include "arp_core.h"
#include "custompatches/drone_core.h"
#include "custompatches/drone_std_core.h"
#if CONFIG_SYNTH_WIRELESS
#include "live_play.h"     /* BLE MIDI live-play voice: GRAPH_TGT_LIVE target */
#endif
#include "graph_popup.h"
#include "filter_graph.h"
#include "display_lfo.h"
#include "seq_defaults.h"
#include "amy_helpers.h"
#include "filter_scope.h"
#if CONFIG_FILTER_SCOPE
#include "amy.h"              /* instrument_get_num_voices, amy_voice_base_osc */
#include "seq_core_config.h"  /* SEQ_ARP_SYNTH */
#endif
#include "seq_clamp.h"
#include "voice_config.h"  /* shared voice constants incl. envelope bounds */
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
    GRAPH_TGT_MELODIC   = 0,
    GRAPH_TGT_DRONE     = 1,
    GRAPH_TGT_ARP       = 2,
    GRAPH_TGT_DRONE_STD = 3,
    /* BLE MIDI live-play voice (slot 10). Structurally the arp's twin - one
     * voice, no layer/track scope - so it follows the arp branch everywhere,
     * including the LFO split: native carrier on wave patches, 20 Hz
     * software stepper on patch strings (live_play_lfo_service). */
    GRAPH_TGT_LIVE      = 4,
} graph_target_t;
static graph_target_t s_graph_target = GRAPH_TGT_MELODIC;

/* Which of the target's two independent AMY breakpoint generators the open
 * editor is showing/editing. 0 = EG0 (amp, the historical default), 1 = EG1
 * (typically the filter sweep — see sequencer_core_push_envelope_eg1()).
 * Reset to 0 on every editor open; the editor cycle (MY_BUTTON_3 click)
 * visits EG0 and EG1 as two consecutive screens before the filter editor. */
static uint8_t s_graph_eg_index = 0;

/* Display cache of the currently-shown EG's curve type (AMY ENVELOPE_*: 0=Normal,
 * 1=Linear, 2=DX7, 3=True-exponential). Mirrors the bound target's stored eg_type;
 * refreshed on open, on EG toggle, and on each cycle. Cycled by MY_BUTTON_1 in the
 * envelope editor (synth_ui_graph_cycle_eg_type); folded into the view signature
 * so the top-bar readout redraws. */
static uint8_t s_graph_eg_type_disp = 0;

/* Short top-bar code for an AMY envelope curve type (eg_type 0..3). */
static const char *graph_eg_type_code(uint8_t eg_type)
{
    switch (eg_type & 3u) {
        case 1:  return "LIN";   /* ENVELOPE_LINEAR          */
        case 2:  return "DX7";   /* ENVELOPE_DX7             */
        case 3:  return "EXP";   /* ENVELOPE_TRUE_EXPONENTIAL*/
        default: return "NRM";   /* ENVELOPE_NORMAL          */
    }
}

/* Full name for the transient top-bar readout shown right after a type cycle. */
static const char *graph_eg_type_name(uint8_t eg_type)
{
    switch (eg_type & 3u) {
        case 1:  return "LINEAR";
        case 2:  return "DX7";
        case 3:  return "TRUE EXP";
        default: return "NORMAL";
    }
}

/* Type-cycle flash: after MY_BUTTON_1 changes the curve type, the top bar shows
 * the full type name for a short window (the right slot is otherwise owned by
 * the point readout). Expiry is tick-based; the derived active flag is folded
 * into the view signature so the bar redraws when the window closes. */
#define GRAPH_TYPE_FLASH_MS 1200u
static TickType_t s_graph_type_flash_until = 0;

static bool graph_type_flash_active(void)
{
    return (int32_t)(s_graph_type_flash_until - xTaskGetTickCount()) > 0;
}

/* ── Curve-type preview shaping ──────────────────────────────────────────────
 * Per-segment shape callback for the ADSR plot: a float mirror of AMY's
 * compute_breakpoint_scale() segment math (envelope.c), evaluated in the
 * widget's normalised 0..1 level space, so the drawn curve matches what the
 * selected eg_type will actually sound like. Runs only while the editor is
 * open, on the 50 ms UI tick — nowhere near the render path. */
#define GRAPH_ENV_EPS 0.0002f   /* BREAKPOINT_EPS: floor for the log-domain types */

static float graph_env_shape(float v0, float v1, float t)
{
    uint8_t eg_type = (uint8_t)(s_graph_eg_type_disp & 3u);

    if (eg_type == 1) {         /* ENVELOPE_LINEAR */
        return v0 + (v1 - v0) * t;
    }

    if (eg_type == 2 || eg_type == 3) {   /* DX7 / TRUE_EXPONENTIAL */
        float a = (v0 > GRAPH_ENV_EPS) ? v0 : GRAPH_ENV_EPS;
        float b = (v1 > GRAPH_ENV_EPS) ? v1 : GRAPH_ENV_EPS;
        if (eg_type == 2 && b > a) {
            /* DX7 attack law. Levels map linear->DX7 (log2 + 12.375), then
             * through the attack-range curve; segment time is normalised so
             * only the ratio matters. Degenerate spans fall through to the
             * true-exp branch below. */
            float l0 = log2f(a) + 12.375f;
            float l1 = log2f(b) + 12.375f;
            float m0 = 1.0f - ((l0 > 4.25f) ? (l0 - 4.25f) : 0.0f) / 9.375f;
            float m1 = 1.0f - ((l1 > 4.25f) ? (l1 - 4.25f) : 0.0f) / 9.375f;
            float dl = log2f(m0) - log2f(m1);
            if (dl > 1e-6f) {
                float t_const = 1.0f / dl;
                float my_t0   = -t_const * log2f(m0);
                float level   = 4.25f
                    + 9.375f * (1.0f - exp2f(-(my_t0 + t) / t_const));
                return exp2f(level - 12.375f);
            }
        }
        /* TRUE_EXPONENTIAL, and DX7 decay/release (also plain true-exp). */
        float la = log2f(a), lb = log2f(b);
        return exp2f(la + (lb - la) * t);
    }

    /* ENVELOPE_NORMAL: overshoot-compensated "false exponential". */
    const float rate      = -4.328085f;   /* EXP_RATE_VAL */
    const float overshoot = 1.0f / (1.0f - exp2f(rate));
    float y = v0 + (v1 - v0) * overshoot * (1.0f - exp2f(rate * t));
    if (y < 0.0f) y = 0.0f;
    return y;
}

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

/* EG1 filter-env depth (melodic only): scratch octaves for the EG1 page's trim
 * mode. Seeded from the row's seq_filter_t.filter_env_amount on editor open;
 * committed on confirm ONLY when edited (s_graph_fenv_dirty) so an untouched
 * editor session never authors the row's filter. */
static float s_graph_fenv_edit  = 0.0f;
static bool  s_graph_fenv_dirty = false;

/* Layer-apply scope: when true, effect-editor commits write to all SEQ_TRACKS
 * in the active layer instead of only the selected track.  Toggled by
 * MY_BUTTON_1 while the ADSR graph or LFO editor is open.  The filter editor
 * repurposes MY_BUTTON_1 for enable/disable, so scope is set there or in LFO. */
static bool s_editor_apply_all = false;

/* Live-preview bookkeeping (see the "Live preview while editing" block below):
 * which parts of the open editor session have pushed scratch state to AMY, and
 * the amp value/throttle needed to restore or pace those pushes. */
static bool       s_graph_live_env  = false; /* env points/type live-pushed   */
static bool       s_graph_live_fenv = false; /* EG1 sweep depth live-pushed   */
static bool       s_amp_live_applied = false;/* a live amp apply landed       */
static bool       s_amp_live_pending = false;
static float      s_graph_amp_open  = 1.0f;  /* amp trim at open (cancel)     */
static TickType_t s_amp_live_last_apply = 0;

/* log(1 + GRAPH_LONG_SQUASH), the constant normaliser of the long-range
 * squash curve — hoisted so the mapping helpers below don't recompute it on
 * every call. */
static float graph_log1p_squash(void)
{
    static float s_val = 0.0f;
    if (s_val == 0.0f) s_val = logf(1.0f + GRAPH_LONG_SQUASH);
    return s_val;
}

/* Normalised X (0..1) -> milliseconds, range/curve aware. */
static uint32_t graph_x_to_ms(float x)
{
    x = SEQ_CLAMP_F32(x, 0.0f, 1.0f);
    if (!s_graph_long_range) {
        return (uint32_t)(x * (float)GRAPH_RANGE_SHORT_MS + 0.5f);
    }
    /* Long view: expand the squashed display X back to a linear time fraction.
     * Display compresses with log1p(k*t)/log1p(k); invert it here. */
    float k = GRAPH_LONG_SQUASH;
    float t = (expf(x * graph_log1p_squash()) - 1.0f) / k;   /* 0..1 linear time */
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
    t = SEQ_CLAMP_F32(t, 0.0f, 1.0f);
    /* Compress: more pixels to small t, fewer to the long tail. */
    return logf(1.0f + k * t) / graph_log1p_squash();
}

/* Push the engine's attack floor into the widget as its minimum point spacing.
 * The widget works in normalised X and cannot know what a millisecond is worth,
 * so without this it would enforce a legibility-derived gap that is stricter
 * than VOICE_ENV_ATTACK_MIN_MS — and by a different amount per range, since the
 * long axis is log-compressed. Re-sync whenever the range changes. */
static void graph_sync_min_gap(void)
{
    graph_popup_set_min_x_gap(&s_graph_popup, graph_ms_to_x(VOICE_ENV_ATTACK_MIN_MS));
}

/* Audio-taper encoder stride for the envelope points' X axis (installed as the
 * popup's xstep hook). Envelope time knobs are conventionally exponential:
 * each detent scales the SEGMENT duration (time since the previous point, i.e.
 * the A/D/R value itself — not the point's cumulative axis position) by ~8%,
 * with a minimum stride so the bottom of the range is reachable at fine
 * resolution. A fixed normalised step (2% of a linear 2 s axis = 40 ms) made
 * the smallest attack move 2→40 ms, which under the log-domain curve types
 * (DX7/EXP, back-loaded attacks) silences short notes outright.
 * Per-role feel: the attack is where a few ms decide pluck vs pad, so it gets
 * a finer ratio and stride than decay, and release coarser still — sweeping a
 * long tail shouldn't take a hundred detents. */
static const float grx_ratio[4]  = { 1.08f, 1.04f, 1.08f, 1.10f }; /* -,A,D,R */
static const float grx_stride[4] = { 1.0f,  1.0f,  1.0f,  2.0f  }; /* min ms  */

static float graph_xstep(uint8_t idx, float x, long delta)
{
    uint32_t prev_ms = 0;
    if (idx > 0 && idx <= s_graph_popup.num_points) {
        prev_ms = graph_x_to_ms(s_graph_popup.points[idx - 1].x);
    }
    uint32_t cum_ms = graph_x_to_ms(x);
    float seg = (cum_ms > prev_ms) ? (float)(cum_ms - prev_ms) : 0.0f;

    float ratio  = grx_ratio[idx & 3u];
    float stride = grx_stride[idx & 3u];
    for (long n = delta; n > 0; --n) {
        float g = seg * ratio;
        seg = (g > seg + stride) ? g : seg + stride;
    }
    for (long n = delta; n < 0; ++n) {
        float g = seg / ratio;
        seg = (g < seg - stride) ? g : seg - stride;
    }
    if (seg < 0.0f) seg = 0.0f;

    float range = s_graph_long_range ? (float)GRAPH_RANGE_LONG_MS
                                     : (float)GRAPH_RANGE_SHORT_MS;
    float cum = (float)prev_ms + seg;
    if (cum > range) cum = range;
    return graph_ms_to_x((uint32_t)(cum + 0.5f));
}

/* Yellow top-bar height on the dual-colour panel: rows 0..15 render yellow and
 * are used as the editor's context bar, the plot fills rows 16..63. */
#define GRAPH_TOPBAR_H 16

/* ── Auto-decay rule (derived-decay mode only) ───────────────────────────────
 * When CONFIG_SEQ_ADSR_EXPLICIT_DECAY is OFF the decay TIME is derived, not
 * user-dragged: the sustain point's X is locked (Y-only) in the widget and
 * recomputed here from attack time + sustain level. Lower sustain -> longer,
 * more audible fall; decay also scales gently with attack.
 * When the Kconfig toggle is ON (default, industry norm) the sustain point's X
 * is user-owned and graph_recompute_decay() is a no-op, so these constants and
 * the whole rule are inert. */
#if !CONFIG_SEQ_ADSR_EXPLICIT_DECAY
#define DECAY_BASE_MS          120u //note 06-20 testing some params moving up from 40
#define DECAY_ATTACK_K         0.5f
#define DECAY_SUSTAIN_SPAN_MS  400.0f
#define DECAY_MIN_MS           20u
#define DECAY_MAX_MS           2000u

static uint32_t graph_decay_ms(uint32_t attack_ms, float sustain_frac)
{
    sustain_frac = SEQ_CLAMP_F32(sustain_frac, 0.0f, 1.0f);
    float d = (float)DECAY_BASE_MS
            + (float)attack_ms * DECAY_ATTACK_K
            + (1.0f - sustain_frac) * DECAY_SUSTAIN_SPAN_MS;
    d = SEQ_CLAMP_F32(d, (float)DECAY_MIN_MS, (float)DECAY_MAX_MS);
    return (uint32_t)(d + 0.5f);
}
#endif /* !CONFIG_SEQ_ADSR_EXPLICIT_DECAY */

static void graph_popup_ensure_init(void)
{
    if (s_graph_popup_inited) return;
    /* Full-screen plot under the yellow context bar (rows 16..63). */
    graph_popup_init(&s_graph_popup, 0, GRAPH_TOPBAR_H, 128,
                     (uint8_t)(64 - GRAPH_TOPBAR_H));
    graph_popup_set_style(&s_graph_popup, GPOPUP_STYLE_ADSR);
    graph_sync_min_gap();
    /* Draw the curve with the bound EG's real per-type shape (reads
     * s_graph_eg_type_disp at draw time, so type cycles retint instantly). */
    graph_popup_set_shape(&s_graph_popup, graph_env_shape);
    /* Audio-taper time stride for A/D/R edits (see graph_xstep). */
    graph_popup_set_xstep(&s_graph_popup, graph_xstep);
#if CONFIG_SEQ_ADSR_EXPLICIT_DECAY
    /* Explicit decay (industry norm): the sustain point is draggable on both
     * axes - X sets the decay time (ms), Y sets the sustain level. */
    graph_popup_set_adsr_lock_sx(&s_graph_popup, false);
#else
    /* Derived decay: sustain point is Y-only; its X (decay time) is auto-derived
     * from attack + sustain via graph_recompute_decay(). */
    graph_popup_set_adsr_lock_sx(&s_graph_popup, true);
#endif
    s_graph_popup_inited = true;
}

/* Recompute the sustain point's X (decay time) from the current attack time and
 * sustain level, then write it back. Keeps all ms math host-side so the widget
 * stays AMY-agnostic. Expects the standard 4-point ADSR layout. */
static void graph_recompute_decay(void)
{
#if CONFIG_SEQ_ADSR_EXPLICIT_DECAY
    /* Explicit decay: the user owns the sustain point's X (decay time), so never
     * re-derive it. Keeps this a single guard covering every call site (seed +
     * every encoder move). */
    return;
#else
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
#endif /* !CONFIG_SEQ_ADSR_EXPLICIT_DECAY */
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
    /* Derived-decay mode: snap the sustain point's X to the auto-decay rule so
     * the opening curve already obeys it. Explicit-decay mode: no-op, so the
     * stored decay_ms (already mapped into pts[2].x above) is shown as-is. */
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
            case GRAPH_TGT_DRONE_STD:
                drone_std_get_envelope2(env);
                return true;
            case GRAPH_TGT_ARP:
                arp_get_envelope2(env);
                return true;
#if CONFIG_SYNTH_WIRELESS
            case GRAPH_TGT_LIVE:
                live_play_get_envelope2(env);
                return true;
#endif
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
        case GRAPH_TGT_DRONE_STD:
            drone_std_get_envelope(env);
            return true;
        case GRAPH_TGT_ARP:
            arp_get_envelope(env);
            return true;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:
            live_play_get_envelope(env);
            return true;
#endif
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
            case GRAPH_TGT_DRONE_STD:
                drone_std_set_envelope2(env);
                break;
            case GRAPH_TGT_ARP:
                arp_set_envelope2(env);
                break;
#if CONFIG_SYNTH_WIRELESS
            case GRAPH_TGT_LIVE:
                live_play_set_envelope2(env);
                break;
#endif
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
        case GRAPH_TGT_DRONE_STD:
            drone_std_set_envelope(env);
            break;
        case GRAPH_TGT_ARP:
            arp_set_envelope(env);
            break;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:
            live_play_set_envelope(env);
            break;
#endif
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
 * the arp screen the arp env, otherwise the selected melodic row.
 *
 * The Wireless page is the exception: it is a page of the menu overlay rather
 * than a ui_mode, so it cannot be read off seq_state.ui_mode - it is tested
 * first, before the mode ladder, since the mode underneath the overlay is
 * whatever screen the user came from. */
void synth_ui_graph_open_envelope(void)
{
    graph_popup_ensure_init();

#if CONFIG_SYNTH_WIRELESS
    if (synth_ui_wireless_page_is_open()) {
        s_graph_target = GRAPH_TGT_LIVE;
    } else
#endif
    if (seq_state.ui_mode == UI_MODE_DRONE) {
        s_graph_target = GRAPH_TGT_DRONE;
    } else if (seq_state.ui_mode == UI_MODE_DRONE_STD) {
        s_graph_target = GRAPH_TGT_DRONE_STD;
    } else if (seq_state.ui_mode == UI_MODE_ARP) {
        s_graph_target = GRAPH_TGT_ARP;
    } else {
        s_graph_target = GRAPH_TGT_MELODIC;
    }

    /* Melodic write-back targets a specific row; capture it at open time. */
    s_graph_layer = seq_state.active_layer_idx;
    s_graph_track = seq_state.selected_track;
    /* Always open on EG0 (amp); MY_BUTTON_3 click advances to the EG1 page. */
    s_graph_eg_index = 0;

    seq_env_t env;
    if (!graph_read_target_env(&env)) {
        env = seq_default_melodic_env();
    }
    s_graph_eg_type_disp = env.eg_type;
    s_graph_type_flash_until = 0;   /* no stale type flash from a prior session */
    /* Fresh live-preview session: nothing pushed yet. */
    s_graph_live_env   = false;
    s_graph_live_fenv  = false;
    s_amp_live_applied = false;
    s_amp_live_pending = false;

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
        case GRAPH_TGT_DRONE_STD:
            s_graph_amp_edit = drone_std_get_amp_trim();
            break;
        case GRAPH_TGT_ARP:
            s_graph_amp_edit = arp_get_amp_scale();
            break;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:
            s_graph_amp_edit = live_play_get_amp_scale();
            break;
#endif
        case GRAPH_TGT_MELODIC:
        default:
            s_graph_amp_edit = sequencer_core_get_melodic_amp_scale(
                s_graph_layer, s_graph_track);
            break;
    }
    s_graph_amp_open = s_graph_amp_edit;   /* cancel restores this value */

    graph_seed_from_env(&env);

    /* Seed the EG1 filter-env depth scratch (melodic rows + arp). */
    s_graph_fenv_dirty = false;
    s_graph_fenv_edit  = 0.0f;
    if (s_graph_target == GRAPH_TGT_MELODIC) {
        seq_filter_t f;
        if (sequencer_core_get_melodic_filter(s_graph_layer, s_graph_track, &f)) {
            s_graph_fenv_edit = f.filter_env_amount;
        }
    } else if (s_graph_target == GRAPH_TGT_ARP) {
        seq_filter_t f;
        arp_get_filter(&f);
        s_graph_fenv_edit = f.filter_env_amount;
#if CONFIG_SYNTH_WIRELESS
    } else if (s_graph_target == GRAPH_TGT_LIVE) {
        seq_filter_t f;
        live_play_get_filter(&f);
        s_graph_fenv_edit = f.filter_env_amount;
#endif
    }

    graph_update_ticks();
    graph_popup_open(&s_graph_popup, GPOPUP_MODE_EDIT, NULL);
    graph_popup_set_style(&s_graph_popup, GPOPUP_STYLE_ADSR);
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph editor open: target=%d L%u T%u range=%s amp=%.2f",
             (int)s_graph_target, s_graph_layer + 1u, s_graph_track + 1u,
             s_graph_long_range ? "LONG" : "SHORT", (double)s_graph_amp_edit);
}

/* Convert the popup's current points to a seq_env_t and write it to the given
 * eg_index's store on the bound target. Shared by graph_commit_to_env() (the
 * currently-shown eg_index) and graph_toggle_eg_index() (writes the
 * DEPARTING eg_index through before switching the view). */
/* Convert the current popup points into `env`'s ADSR fields, plus the shown
 * curve type from s_graph_eg_type_disp (the editor's scratch — the type is
 * previewed live and persisted on commit, like the points). `env` should be
 * pre-seeded from the target so any non-ADSR fields carry through. Returns
 * false when the popup has no full ADSR point set. */
static bool graph_points_to_env(seq_env_t *env)
{
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    if (n < 4) return false;   /* expect origin + A + D + R */

    uint32_t cum_a = graph_x_to_ms(pts[1].x);
    uint32_t cum_d = graph_x_to_ms(pts[2].x);
    uint32_t cum_r = graph_x_to_ms(pts[3].x);

    /* Convert cumulative times back to per-segment durations (clamp monotonic). */
    uint32_t a = SEQ_CLAMP_U32(cum_a, VOICE_ENV_ATTACK_MIN_MS, VOICE_ENV_TIME_MAX_MS);
    env->attack_ms   = a;
    env->decay_ms    = (cum_d > cum_a) ? (cum_d - cum_a) : 0;
    env->release_ms  = (cum_r > cum_d) ? (cum_r - cum_d) : 0;
    env->sustain_pct = (uint8_t)(pts[2].y * 100.0f + 0.5f);
    env->eg_type     = s_graph_eg_type_disp;
    return true;
}

static void graph_write_points_to_env(uint8_t eg_index)
{
    seq_env_t env;
    if (!graph_read_target_env_idx(&env, eg_index)) return;
    if (!graph_points_to_env(&env)) return;
    graph_write_target_env_idx(&env, eg_index);
}

/* ── Live preview while editing ──────────────────────────────────────────────
 * Every edit is auditioned immediately: scratch values are pushed to AMY only
 * (preview calls — the committed store never changes until confirm), so cancel
 * restores by re-pushing the store, or — for melodic rows that were never
 * authored, whose live state came from the patch string itself — by reloading
 * the layer's patch. Amp trim is the one exception: it lives in the step-emit
 * path, so its live apply goes through the real setter (throttled, since each
 * melodic apply re-emits the track's scheduled steps) and cancel restores the
 * value captured at editor open. State lives with the other editor statics
 * near the top of the file. */
#define GRAPH_AMP_LIVE_MS 200u               /* min spacing of amp re-emits   */

static void graph_live_push_env(void)
{
    seq_env_t env;
    if (!graph_read_target_env_idx(&env, s_graph_eg_index)) return;
    if (!graph_points_to_env(&env)) return;
    switch (s_graph_target) {
        case GRAPH_TGT_DRONE:
            if (s_graph_eg_index == 1) drone_preview_envelope2(&env);
            else                       drone_preview_envelope(&env);
            break;
        case GRAPH_TGT_DRONE_STD:
            if (s_graph_eg_index == 1) drone_std_preview_envelope2(&env);
            else                       drone_std_preview_envelope(&env);
            break;
        case GRAPH_TGT_ARP:
            if (s_graph_eg_index == 1) arp_preview_envelope2(&env);
            else                       arp_preview_envelope(&env);
            break;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:
            if (s_graph_eg_index == 1) live_play_preview_envelope2(&env);
            else                       live_play_preview_envelope(&env);
            break;
#endif
        case GRAPH_TGT_MELODIC:
        default: {
            uint8_t t0 = s_editor_apply_all ? 0 : s_graph_track;
            uint8_t t1 = s_editor_apply_all ? (uint8_t)(SEQ_TRACKS - 1)
                                            : s_graph_track;
            for (uint8_t t = t0; t <= t1; ++t) {
                if (s_graph_eg_index == 1)
                    sequencer_core_preview_melodic_envelope2(s_graph_layer, t, &env);
                else
                    sequencer_core_preview_melodic_envelope(s_graph_layer, t, &env);
            }
            break;
        }
    }
    s_graph_live_env = true;
}

/* EG1 sweep depth: stored filter + the scratch depth, previewed per row. */
static void graph_live_push_fenv(void)
{
    if (s_graph_target == GRAPH_TGT_ARP) {
        seq_filter_t f;
        arp_get_filter(&f);
        f.filter_env_amount = s_graph_fenv_edit;
        arp_preview_filter(&f);
        s_graph_live_fenv = true;
        return;
    }
#if CONFIG_SYNTH_WIRELESS
    if (s_graph_target == GRAPH_TGT_LIVE) {
        seq_filter_t f;
        live_play_get_filter(&f);
        f.filter_env_amount = s_graph_fenv_edit;
        live_play_preview_filter(&f);
        s_graph_live_fenv = true;
        return;
    }
#endif
    if (s_graph_target != GRAPH_TGT_MELODIC) return;
    uint8_t t0 = s_editor_apply_all ? 0 : s_graph_track;
    uint8_t t1 = s_editor_apply_all ? (uint8_t)(SEQ_TRACKS - 1) : s_graph_track;
    for (uint8_t t = t0; t <= t1; ++t) {
        seq_filter_t f;
        if (!sequencer_core_get_melodic_filter(s_graph_layer, t, &f)) continue;
        f.filter_env_amount = s_graph_fenv_edit;
        sequencer_core_preview_melodic_filter(s_graph_layer, t, &f);
    }
    s_graph_live_fenv = true;
}

static void graph_amp_live_set(float v)
{
    switch (s_graph_target) {
        case GRAPH_TGT_DRONE:     drone_set_amp_trim(v);     break;
        case GRAPH_TGT_DRONE_STD: drone_std_set_amp_trim(v); break;
        case GRAPH_TGT_ARP:       arp_set_amp_scale(v);      break;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:      live_play_set_amp_scale(v); break;
#endif
        case GRAPH_TGT_MELODIC:
        default: {
            uint8_t t0 = s_editor_apply_all ? 0 : s_graph_track;
            uint8_t t1 = s_editor_apply_all ? (uint8_t)(SEQ_TRACKS - 1)
                                            : s_graph_track;
            for (uint8_t t = t0; t <= t1; ++t)
                sequencer_core_set_melodic_amp_scale(s_graph_layer, t, v);
            break;
        }
    }
}

/* Apply a pending amp edit if the throttle window has passed. Called on each
 * detent (leading edge) and from the UI task's tick (trailing flush), so the
 * final value always lands within ~GRAPH_AMP_LIVE_MS of the last detent. */
static void graph_amp_live_flush(bool force)
{
    if (!s_amp_live_pending) return;
    if (!graph_popup_is_active(&s_graph_popup)) {
        s_amp_live_pending = false;
        return;
    }
    if (!force && (int32_t)(xTaskGetTickCount() - s_amp_live_last_apply)
                      < (int32_t)pdMS_TO_TICKS(GRAPH_AMP_LIVE_MS)) {
        return;
    }
    s_amp_live_last_apply = xTaskGetTickCount();
    s_amp_live_pending = false;
    s_amp_live_applied = true;
    graph_amp_live_set(s_graph_amp_edit);
}

/* Undo every live push on cancel: amp back to its open value, and the
 * previewed envelope/depth back to the store — or a full layer reload when a
 * touched melodic row was never authored (only the patch knows its state). */
static void graph_live_cancel_restore(void)
{
    s_amp_live_pending = false;
    if (s_amp_live_applied) {
        graph_amp_live_set(s_graph_amp_open);
        s_amp_live_applied = false;
    }
    if (!s_graph_live_env && !s_graph_live_fenv) return;

    if (s_graph_target == GRAPH_TGT_MELODIC) {
        uint8_t t0 = s_editor_apply_all ? 0 : s_graph_track;
        uint8_t t1 = s_editor_apply_all ? (uint8_t)(SEQ_TRACKS - 1) : s_graph_track;
        bool need_reload = false;
        for (uint8_t t = t0; t <= t1; ++t) {
            if (s_graph_live_env &&
                !sequencer_core_melodic_env_authored(s_graph_layer, t, s_graph_eg_index))
                need_reload = true;
            if (s_graph_live_fenv &&
                !sequencer_core_melodic_filter_authored(s_graph_layer, t))
                need_reload = true;
        }
        if (need_reload) {
            sequencer_core_reload_layer_synth(s_graph_layer);
        } else {
            seq_env_t env;
            if (s_graph_live_env && graph_read_target_env_idx(&env, s_graph_eg_index)) {
                for (uint8_t t = t0; t <= t1; ++t) {
                    if (s_graph_eg_index == 1)
                        sequencer_core_preview_melodic_envelope2(s_graph_layer, t, &env);
                    else
                        sequencer_core_preview_melodic_envelope(s_graph_layer, t, &env);
                }
            }
            if (s_graph_live_fenv) {
                for (uint8_t t = t0; t <= t1; ++t) {
                    seq_filter_t f;
                    if (sequencer_core_get_melodic_filter(s_graph_layer, t, &f))
                        sequencer_core_preview_melodic_filter(s_graph_layer, t, &f);
                }
            }
        }
    } else {
        if (s_graph_live_fenv && s_graph_target == GRAPH_TGT_ARP) {
            /* Re-push the stored filter so the previewed sweep depth reverts. */
            seq_filter_t f;
            arp_get_filter(&f);
            arp_preview_filter(&f);
        }
#if CONFIG_SYNTH_WIRELESS
        if (s_graph_live_fenv && s_graph_target == GRAPH_TGT_LIVE) {
            seq_filter_t f;
            live_play_get_filter(&f);
            live_play_preview_filter(&f);
        }
#endif
        seq_env_t env;
        if (s_graph_live_env && graph_read_target_env_idx(&env, s_graph_eg_index)) {
            if (s_graph_target == GRAPH_TGT_ARP) {
                if (s_graph_eg_index == 1) arp_preview_envelope2(&env);
                else                       arp_preview_envelope(&env);
#if CONFIG_SYNTH_WIRELESS
            } else if (s_graph_target == GRAPH_TGT_LIVE) {
                if (s_graph_eg_index == 1) live_play_preview_envelope2(&env);
                else                       live_play_preview_envelope(&env);
#endif
            } else if (s_graph_target == GRAPH_TGT_DRONE_STD) {
                if (s_graph_eg_index == 1) drone_std_preview_envelope2(&env);
                else                       drone_std_preview_envelope(&env);
            } else {
                if (s_graph_eg_index == 1) drone_preview_envelope2(&env);
                else                       drone_preview_envelope(&env);
            }
        }
    }
    s_graph_live_env  = false;
    s_graph_live_fenv = false;
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
        case GRAPH_TGT_DRONE_STD:
            drone_std_set_amp_trim(s_graph_amp_edit);
            break;
        case GRAPH_TGT_ARP:
            arp_set_amp_scale(s_graph_amp_edit);
            break;
#if CONFIG_SYNTH_WIRELESS
        case GRAPH_TGT_LIVE:
            live_play_set_amp_scale(s_graph_amp_edit);
            break;
#endif
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

    /* Commit the EG1 filter-env depth (melodic only, only if edited). Read-
     * modify-write through the public filter API so the COEF_EG1 push, the
     * guaranteed EG1 breakpoints, the 0..8 clamp, and filter_authored all
     * stay in the engine. Honors the layer/track scope. */
    if (s_graph_fenv_dirty && s_graph_target == GRAPH_TGT_ARP) {
        seq_filter_t f;
        arp_get_filter(&f);
        f.filter_env_amount = s_graph_fenv_edit;
        arp_set_filter(&f);
        s_graph_fenv_dirty = false;
    }
#if CONFIG_SYNTH_WIRELESS
    if (s_graph_fenv_dirty && s_graph_target == GRAPH_TGT_LIVE) {
        seq_filter_t f;
        live_play_get_filter(&f);
        f.filter_env_amount = s_graph_fenv_edit;
        live_play_set_filter(&f);
        s_graph_fenv_dirty = false;
    }
#endif
    if (s_graph_fenv_dirty && s_graph_target == GRAPH_TGT_MELODIC) {
        uint8_t t0 = s_editor_apply_all ? 0 : s_graph_track;
        uint8_t t1 = s_editor_apply_all ? (uint8_t)(SEQ_TRACKS - 1) : s_graph_track;
        for (uint8_t t = t0; t <= t1; ++t) {
            seq_filter_t f;
            if (!sequencer_core_get_melodic_filter(s_graph_layer, t, &f)) continue;
            f.filter_env_amount = s_graph_fenv_edit;
            sequencer_core_set_melodic_filter(s_graph_layer, t, &f);
        }
        s_graph_fenv_dirty = false;
    }
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
    graph_sync_min_gap();   /* the floor's normalised width moves with the axis */

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
/* Does the bound target expose an EG1 page? The free-running drone never sees
 * another note-on after its enable gate, so EG1 would fire once and park at its
 * sustain level forever — a static offset masquerading as an envelope. Hide the
 * whole EG1 page for it (the stutter drone keeps it: its stutter gates are real
 * note-ons). The editor cycle consults this too, so the hidden page is skipped
 * rather than dead-ending the cycle on EG0. */
static bool graph_target_has_eg1(void)
{
    return s_graph_target != GRAPH_TGT_DRONE_STD;
}

/* Targets carrying an EG1->cutoff sweep DEPTH field (seq_filter_t's
 * filter_env_amount): the melodic rows, the arp and the live voice. The drones
 * have no such field - their EG1 page edits the envelope only. Gates the depth
 * readout, the encoder's depth adjust and the polarity flip. */
static bool graph_target_has_eg1_depth(void)
{
#if CONFIG_SYNTH_WIRELESS
    if (s_graph_target == GRAPH_TGT_LIVE) return true;
#endif
    return s_graph_target == GRAPH_TGT_MELODIC || s_graph_target == GRAPH_TGT_ARP;
}

static void graph_toggle_eg_index(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return;
    if (!graph_target_has_eg1()) return;

    if (s_graph_env_dirty) {
        graph_write_points_to_env(s_graph_eg_index);
    }

    s_graph_eg_index = (s_graph_eg_index == 0) ? 1 : 0;

    seq_env_t env;
    if (!graph_read_target_env(&env)) {
        env = (s_graph_eg_index == 1) ? seq_default_melodic_env1()
                                       : seq_default_melodic_env();
    }
    s_graph_eg_type_disp = env.eg_type;
    uint32_t total_env_ms = env.attack_ms + env.decay_ms + env.release_ms;
    s_graph_long_range = (total_env_ms >= GRAPH_RANGE_SHORT_MS);

    graph_seed_from_env(&env);
    graph_update_ticks();
    s_graph_env_dirty = false;
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph eg index -> EG%u", s_graph_eg_index);
}

/* ── Perceived-duration equivalence across curve types ───────────────────────
 * Fraction of a segment's nominal time at which each type has covered its
 * first/last 40 dB — where a falling segment becomes effectively silent, or a
 * rising one becomes audible. Derived from envelope.c: EXP/DX7 move linearly
 * in dB down to the -74 dB BREAKPOINT_EPS floor, so -40 dB lands at ~40/74 of
 * T; NORMAL's RC shape crosses it at ~0.94 T; LINEAR at 0.99 T; the DX7
 * attack law is RC-like by design. Cycling types rescales each segment by
 * g_old/g_new so the AUDIBLE length is preserved even though the stored ms
 * (and the drawn point positions) change — without this, LIN→EXP at fixed ms
 * turns a long ringing release into a fast pluck. */
static const float graph_g_fall[4] = { 0.94f, 0.99f, 0.54f, 0.54f }; /* NRM LIN DX7 EXP */
static const float graph_g_rise[4] = { 0.94f, 0.99f, 0.94f, 0.54f };

static void graph_rescale_points_for_type(uint8_t old_type, uint8_t new_type)
{
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    if (n < 4) return;

    float cum_a = (float)graph_x_to_ms(pts[1].x);
    float cum_d = (float)graph_x_to_ms(pts[2].x);
    float cum_r = (float)graph_x_to_ms(pts[3].x);
    float a = cum_a;
    float d = (cum_d > cum_a) ? (cum_d - cum_a) : 0.0f;
    float r = (cum_r > cum_d) ? (cum_r - cum_d) : 0.0f;

    a *= graph_g_rise[old_type & 3u] / graph_g_rise[new_type & 3u];
    float kf = graph_g_fall[old_type & 3u] / graph_g_fall[new_type & 3u];
    d *= kf;
    r *= kf;
    if (a < (float)VOICE_ENV_ATTACK_MIN_MS) a = (float)VOICE_ENV_ATTACK_MIN_MS;

    /* Grow into the LONG range up front so a lengthening rescale isn't clipped
     * by the SHORT axis; the auto-range check below shrinks back when a
     * shortening rescale allows it. */
    if (!s_graph_long_range && (a + d + r) >= (float)GRAPH_RANGE_SHORT_MS) {
        graph_set_range(true);
    }
    float range = s_graph_long_range ? (float)GRAPH_RANGE_LONG_MS
                                     : (float)GRAPH_RANGE_SHORT_MS;
    float ca = (a > range) ? range : a;
    float cd = (ca + d > range) ? range : (ca + d);
    float cr = (cd + r > range) ? range : (cd + r);
    pts[1].x = graph_ms_to_x((uint32_t)(ca + 0.5f));
    pts[2].x = graph_ms_to_x((uint32_t)(cd + 0.5f));
    pts[3].x = graph_ms_to_x((uint32_t)(cr + 0.5f));

    uint8_t saved_cursor  = s_graph_popup.cursor;
    bool    saved_editing = s_graph_popup.editing_value;
    bool    saved_axis_y  = s_graph_popup.adjust_axis_y;
    graph_popup_set_points(&s_graph_popup, pts, n);
    s_graph_popup.cursor        = saved_cursor;
    s_graph_popup.editing_value = saved_editing;
    s_graph_popup.adjust_axis_y = saved_axis_y;

    graph_recompute_decay();
    graph_update_ticks();
    graph_auto_range_check();
}

/* Cycle the shown EG's curve type Normal->Linear->DX7->TrueExp->Normal (0..3).
 * Bound to MY_BUTTON_1 in the envelope editor (the slot vacated by the apply-
 * scope toggle, which moved to SHIFT+3). The new type is auditioned as a live
 * preview (AMY only) like every other edit — the store keeps the old type
 * until commit, and cancel restores it. Segment times are rescaled to keep the
 * perceived envelope length (see graph_rescale_points_for_type above). No-op
 * unless the envelope editor is open. */
void synth_ui_graph_cycle_eg_type(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return;

    uint8_t old_type = s_graph_eg_type_disp;
    s_graph_eg_type_disp = (uint8_t)((s_graph_eg_type_disp + 1u) & 3u);
    graph_rescale_points_for_type(old_type, s_graph_eg_type_disp);
    s_graph_env_dirty = true;   /* commit persists the type with the points */
    graph_live_push_env();
    s_graph_type_flash_until =
        xTaskGetTickCount() + pdMS_TO_TICKS(GRAPH_TYPE_FLASH_MS);
    s_force_redraw = true;
    ESP_LOGI(TAG, "graph EG%u type -> %s(%u) (preview)", s_graph_eg_index,
             graph_eg_type_code(s_graph_eg_type_disp),
             (unsigned)s_graph_eg_type_disp);
}

/* Hint-strip b2 label for the envelope editor: MY_BUTTON_2's trim mode edits
 * amplitude on the EG0 page but the EG1->cutoff sweep depth on the melodic
 * EG1 page. */
const char *synth_ui_graph_hint_b2(void)
{
    /* Deliberately narrower than graph_target_has_eg1_depth(): the arp keeps
     * its historical "Amp" label here, unchanged by the live-voice port. */
    bool env_slot = (s_graph_target == GRAPH_TGT_MELODIC);
#if CONFIG_SYNTH_WIRELESS
    env_slot = env_slot || (s_graph_target == GRAPH_TGT_LIVE);
#endif
    return (s_graph_eg_index == 1 && env_slot) ? "Env" : "Amp";
}

/* Flip the sign of the EG1->cutoff sweep (melodic rows + arp — the drones
 * have no EG1 depth field). Bound to MY_BUTTON_SHOULDER while the EG1 page is
 * showing. No-op at 0.0 depth: there is
 * nothing to invert and it keeps -0.0 out of the readout. */
void synth_ui_graph_flip_eg1_polarity(void)
{
    if (!graph_popup_is_active(&s_graph_popup)) return;
    if (s_graph_eg_index != 1) return;
    if (!graph_target_has_eg1_depth()) return;
    if (s_graph_fenv_edit == 0.0f) return;
    s_graph_fenv_edit  = -s_graph_fenv_edit;
    s_graph_fenv_dirty = true;
    s_force_redraw = true;
    ESP_LOGI(TAG, "EG1 polarity -> %+.2f oct", (double)s_graph_fenv_edit);
}

/* Route an encoder delta to the pop-up. Returns true if the pop-up consumed it
 * (host should then skip its normal sequencer routing). */
bool synth_ui_graph_handle_encoder(long delta)
{
    if (!graph_popup_is_active(&s_graph_popup)) return false;

    if (s_graph_amp_mode) {
        if (s_graph_eg_index == 1 && graph_target_has_eg1_depth()) {
            /* EG1 page: encoder adjusts EG1->cutoff depth, 0.25 oct/detent.
             * Bipolar -8..+8; negative = inverted/downward sweep (same range
             * as the filter editor's EG cursor — one shared field). */
            float v = s_graph_fenv_edit + (float)delta * 0.25f;
            v = SEQ_CLAMP_F32(v, -8.0f, 8.0f);
            s_graph_fenv_edit  = v;
            s_graph_fenv_dirty = true;
            graph_live_push_fenv();
            s_force_redraw = true;
            return true;
        }
        /* Amp mode: encoder adjusts per-target amplitude trim in 5% steps.
         * Applied live but throttled (melodic applies re-emit the track). */
        float v = s_graph_amp_edit + (float)delta * 0.05f;
        v = SEQ_CLAMP_F32(v, 0.0f, 1.0f);
        s_graph_amp_edit = v;
        s_amp_live_pending = true;
        graph_amp_live_flush(false);
        s_force_redraw = true;
        return true;
    }

    s_graph_env_dirty = true;   /* user moved an ADSR control point */
    bool adjusting = s_graph_popup.editing_value;
    graph_popup_handle_encoder(&s_graph_popup, delta);
    /* Derived-decay mode: moving A (time) or the S level changes the derived
     * decay, so re-snap S.x. No-op in explicit-decay mode (user owns S.x). */
    graph_recompute_decay();
    /* Auto-switch range if total envelope time crosses the threshold. */
    graph_auto_range_check();
    /* Point actually moved (not just cursor selection): audition it. */
    if (adjusting) graph_live_push_env();
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
        ESP_LOGI(TAG, "graph popup cancelled (restoring stored state)");
        graph_live_cancel_restore();
        graph_popup_close(&s_graph_popup);
    }
    return true;
}

/* Commit the current edits and close the editor. Bound to a MY_BUTTON_0 short
 * tap: closing keeps your work. The separate discard path is
 * synth_ui_graph_handle_button(true) (cancel), on a MY_BUTTON_0 long press. */
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
#if CONFIG_SYNTH_WIRELESS
/* Captured at open: the LFO editor targets the live voice (the Wireless page
 * is a menu overlay, not a ui_mode, so commit cannot route off seq_state). */
static bool s_lfo_live_target = false;
#endif

/* Normalisation helpers shared between open and commit. */
static float filter_hz_to_norm(float hz)
{
    hz = SEQ_CLAMP_F32(hz, FGRAPH_CUTOFF_HZ_MIN, FGRAPH_CUTOFF_HZ_MAX);
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

/* True when the filter editor's target plays a feedback wave (KS): the editor
 * then exposes the extra FB cursor (slot 2) editing KS string feedback 0..1.
 * Q stays fully editable — AMY runs the biquad on KS oscs like any other wave.
 * Covers the melodic KS patch and both arp routes to KS (WAVE-mode wave and
 * PATCH-mode virtual patch 263); the drones exclude KS from their cycles. */
/* Which backend the FILTER editor is bound to. The graph editor captures its
 * target in s_graph_target at open time, but the filter editor derives it on
 * every call - historically straight off seq_state.ui_mode. That breaks for the
 * live voice: the Wireless page is an overlay, so the mode underneath is
 * whichever screen the user opened the menu from, and a live filter opened over
 * the drone screen would otherwise take the drone's fixed-LPF24 cursor map and
 * push to the drone's sweep. These predicates put LIVE first so it can never
 * inherit another target's behaviour.
 *
 * REFACTOR TARGET: these bools are the seam, not the destination. Deriving the
 * edit target from sequencer UI state couples every editor to which SCREEN is
 * showing, when what they actually need is which VOICE they were opened on -
 * two things that only coincide for targets that happen to own a top-level
 * screen. The live voice is the first that does not, and adding a second such
 * target (a second live slot, a per-drum-track editor, MIDI-learn on an
 * arbitrary slot) means another predicate here and another ladder arm at every
 * call site.
 *
 * The decoupled shape already exists one function up: capture the target once
 * at open time, the way s_graph_target does, and give it a backend vtable
 * (get/set/preview env, env1, filter, amp, plus flags for "has EG1 depth",
 * "has an LFO page", "has track scope"). Then the editors read one struct and
 * seq_state is only consulted when BINDING a target, not while editing one -
 * which also retires the s_graph_layer/s_graph_track statics for every target
 * that has no track scope. Worth doing when the third non-screen target lands;
 * not worth churning the drone/arp/melodic paths for on its own. */
static bool filter_tgt_is_live(void)
{
    return synth_ui_wireless_page_is_open();
}
static bool filter_tgt_is_drone(void)
{
    return !filter_tgt_is_live() && seq_state.ui_mode == UI_MODE_DRONE;
}
static bool filter_tgt_is_drone_std(void)
{
    return !filter_tgt_is_live() && seq_state.ui_mode == UI_MODE_DRONE_STD;
}
static bool filter_tgt_is_arp(void)
{
    return !filter_tgt_is_live() && seq_state.ui_mode == UI_MODE_ARP;
}

static bool filter_target_is_feedback(void)
{
    /* Live voice is always a patch; KS feedback is not one of its cursors. */
    if (filter_tgt_is_live()) return false;
    if (filter_tgt_is_arp()) {
        return (arp_get_source() == ARP_SRC_WAVE  && arp_get_wave()  == KS)
            || (arp_get_source() == ARP_SRC_PATCH && arp_get_patch() == SEQ_PATCH_KS);
    }
    if (filter_tgt_is_drone() || filter_tgt_is_drone_std()) {
        return false;
    }
    return sequencer_core_get_layer_patch(seq_state.active_layer_idx) == SEQ_PATCH_KS;
}

#if CONFIG_FILTER_SCOPE
/* Bind the live overlay to the oscillators of whatever target the popup is
 * editing. Called on open and on any event that can rebuild the target's
 * voices, because a cached oscillator index survives a patch change while the
 * slot behind it may not - and the consumer of that index is the render task.
 *
 * The drone targets are deliberately not armed: the stutter drone's editor
 * shows a sweep MIDPOINT rather than a cutoff (see filter_load_from_target),
 * so a live band would not correspond to the curve being drawn. */
/* Last list handed to filter_scope_arm(), so the per-frame rebind below is a
 * comparison rather than a re-arm. 0xFF means "cache invalid, force a rebind". */
static uint16_t s_scope_oscs[FILTER_SCOPE_MAX_OSCS];
static uint8_t  s_scope_n = 0xFF;

/* Disarm and invalidate the cache together. Keeping these paired matters: a
 * bare disarm would leave the cache describing a list that is no longer armed,
 * and the next rebind would compare equal and decline to re-arm it. */
static void filter_scope_drop(void)
{
    filter_scope_disarm();
    s_scope_n = 0xFF;
}

static void filter_scope_bind_target(void)
{
    uint8_t slot = 0;

    if (filter_tgt_is_live()) {
#if CONFIG_SYNTH_WIRELESS
        slot = live_play_synth_slot();
#endif
    } else if (filter_tgt_is_arp()) {
        slot = SEQ_ARP_SYNTH;
    } else if (filter_tgt_is_drone() || filter_tgt_is_drone_std()) {
        filter_scope_drop();
        return;
    } else {
        slot = sequencer_core_get_track_synth(seq_state.active_layer_idx,
                                              seq_state.selected_track);
    }

    if (slot == 0) {
        filter_scope_drop();
        return;
    }

    uint16_t voices[MAX_VOICES_PER_INSTRUMENT];
    int nv = instrument_get_num_voices(slot, voices);
    if (nv <= 0) {
        filter_scope_drop();
        return;
    }

    /* Oscillator 0 of each voice is the audible carrier that owns the filter;
     * the voice's other oscillators are mod/algo sources. A voice's base osc IS
     * its oscillator 0, since event osc numbers are relative to the base. */
    uint16_t oscs[FILTER_SCOPE_MAX_OSCS];
    uint8_t  n = 0;
    for (int i = 0; i < nv && n < FILTER_SCOPE_MAX_OSCS; i++) {
        uint16_t base;
        if (amy_voice_base_osc(voices[i], &base)) {
            oscs[n++] = base;
        }
    }

    /* Re-arm only on an actual change. This runs every UI frame so that a voice
     * rebuild under the open popup (project load, chord-preset reallocation)
     * cannot leave the render tap reading another target's oscillators - but
     * re-arming unconditionally would clear the armed flag every frame and cost
     * the tap a block each time. */
    if (n == s_scope_n && memcmp(oscs, s_scope_oscs, n * sizeof(oscs[0])) == 0) {
        return;
    }
    memcpy(s_scope_oscs, oscs, n * sizeof(oscs[0]));
    s_scope_n = n;
    filter_scope_arm(oscs, n);
}

/* Drain the published band into s_fgraph, quantised to plot columns.
 *
 * Quantisation is not cosmetic: filter_view_signature() hashes the whole
 * struct, so unrounded floats would hash differently every frame and redraw the
 * screen continuously while nothing visibly moved. */
static void filter_sync_live_band(void)
{
    /* Condition 1 from the spec: while cutoff or resonance is actually being
     * adjusted, the authored value is the truth. A band moving under the
     * encoder would fight the very edit being made. */
    if (s_fgraph.editing && (s_fgraph.cursor == 0 || s_fgraph.cursor == 1)) {
        /* Keep draining while frozen: an unbumped epoch would let the tap
         * widen min/max across the whole edit, flashing a band spanning the
         * entire sweep on the first frame after the encoder is released. */
        (void)filter_scope_read(NULL, NULL);
        s_fgraph.live_valid = false;
        return;
    }

    float lo_lf, hi_lf;
    if (!filter_scope_read(&lo_lf, &hi_lf)) {
        s_fgraph.live_valid = false;   /* nothing armed, or nothing sounding */
        return;
    }

    /* AMY log-frequency -> Hz -> the graph's own 0..1 log axis. */
    const float lo_norm = filter_hz_to_norm(freq_of_logfreq(lo_lf));
    const float hi_norm = filter_hz_to_norm(freq_of_logfreq(hi_lf));

    /* Round to plot columns (the renderer maps norm * (FG_PLOT_W - 1)). */
    const float cols = 127.0f;
    s_fgraph.live_lo_norm = (float)(int)(lo_norm * cols + 0.5f) / cols;
    s_fgraph.live_hi_norm = (float)(int)(hi_norm * cols + 0.5f) / cols;
    s_fgraph.live_valid   = true;
}
#endif /* CONFIG_FILTER_SCOPE */

/* Populate s_fgraph from s_filter_edit and the current graph target. */
static void filter_sync_fgraph(void)
{
    s_fgraph.filter_type    = s_filter_edit.filter_type;
    s_fgraph.cutoff_norm    = filter_hz_to_norm(s_filter_edit.cutoff_hz);
    s_fgraph.has_feedback   = filter_target_is_feedback();
    s_fgraph.resonance_norm = filter_q_to_norm(s_filter_edit.resonance);
    s_fgraph.feedback_norm  = SEQ_CLAMP_F32(s_filter_edit.feedback, 0.0f, 1.0f);
    s_fgraph.enabled        = s_filter_edit.enabled;
    /* cursor and editing stay unchanged */
}

/* Read the current filter state from the active target into s_filter_edit. */
static void filter_load_from_target(void)
{
    seq_filter_t f = {0};
#if CONFIG_SYNTH_WIRELESS
    if (synth_ui_wireless_page_is_open()) {
        /* Live voice: same never-authored sentinel as the arp/melodic rows.
         * Tested before the ui_mode ladder because the Wireless page is an
         * overlay - the mode underneath is whatever screen the user came from. */
        live_play_get_filter(&f);
        if (f.cutoff_hz <= 0.0f) {
            f.filter_type = SEQ_FILTER_LPF;
            f.cutoff_hz   = 800.0f;
            f.resonance   = 1.0f;
            f.enabled     = false;
        }
        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "LIVE");
    } else
#endif
    if (filter_tgt_is_drone()) {
        /* Drone: always LPF24, cutoff = sweep midpoint, Q from drone_get_resonance. */
        float lo = drone_get_sweep_lo();
        float hi = drone_get_sweep_hi();
        f.filter_type = 4;   /* SEQ_FILTER_LPF24 */
        f.cutoff_hz   = SEQ_CLAMP_F32((lo + hi) * 0.5f, FGRAPH_CUTOFF_HZ_MIN, FGRAPH_CUTOFF_HZ_MAX);
        f.resonance   = drone_get_resonance();
        f.enabled     = true;

        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "STUTR");
    } else if (filter_tgt_is_drone_std()) {
        drone_std_get_filter(&f);
        /* Same never-authored sentinel handling as the arp: seed sensible
         * starting values but honestly read "OFF" until the user enables. */
        if (f.cutoff_hz <= 0.0f) {
            f.filter_type = SEQ_FILTER_LPF;
            f.cutoff_hz   = 1200.0f;
            f.resonance   = 1.0f;
            f.enabled     = false;
        }
        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "DRONE");
    } else if (filter_tgt_is_arp()) {
        arp_get_filter(&f);
        /* Only apply display defaults when the filter was never authored
         * (cutoff_hz == 0 from zero-init); preserve authored values even
         * when disabled so enabled=false round-trips correctly. Default to
         * disabled (enabled=false) so the graph honestly reads "OFF" for a
         * never-authored track — the type/cutoff/resonance below are only a
         * sensible starting point for when the user toggles the filter on. */
        if (f.cutoff_hz <= 0.0f) {
            f.filter_type = SEQ_FILTER_LPF;
            f.cutoff_hz   = 800.0f;
            f.resonance   = 1.0f;
            f.enabled     = false;
        }
        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "ARP");
    } else {
        uint8_t li = seq_state.active_layer_idx;
        uint8_t tr = seq_state.selected_track;
        sequencer_core_get_melodic_filter(li, tr, &f);
        /* Same sentinel: zero cutoff means never-authored; disabled-but-authored
         * keeps its authored values for correct round-trip display. Default to
         * disabled so the graph reads "OFF" when the track has no filter — the
         * type/cutoff/resonance are just the starting point for enabling one. */
        if (f.cutoff_hz <= 0.0f) {
            f.filter_type = SEQ_FILTER_LPF;
            f.cutoff_hz   = 800.0f;
            f.resonance   = 1.0f;
            f.enabled     = false;
        }
        snprintf(s_fgraph.label, sizeof(s_fgraph.label), "L%u T%u%s",
                 (unsigned)(li + 1), (unsigned)(tr + 1),
                 s_editor_apply_all ? ">L" : ">T");
    }
    /* Drone filter is a fixed LPF24 that is always on (cursor tops out at 1), so
     * it hides the type/enable header controls; melodic + arp expose both. */
    s_fgraph.show_toggles = (!filter_tgt_is_drone());
    /* Feedback waves (KS): a zero feedback means "never authored" — seed the
     * editor at the engine's build-time default so the FB readout opens honest
     * (0.9) instead of at a silent-string 0%. */
    if (filter_target_is_feedback() && f.feedback <= 0.0f) {
        f.feedback = 0.9f;
    }
    s_filter_edit = f;
}

/* ── Filter live preview ─────────────────────────────────────────────────────
 * Same model as the envelope editor: each edit pushes the scratch filter to
 * AMY only; cancel re-pushes the store (or reloads the layer when a touched
 * melodic row was never authored). The drone has no preview path — its sweep
 * window/resonance are live drone state — so its values are snapshotted at
 * open and re-set on cancel. */
static bool  s_filter_live = false;      /* any live push this session       */
static float s_fdrone_open_lo, s_fdrone_open_hi, s_fdrone_open_res;

static void filter_live_push(void)
{
    if (filter_tgt_is_drone()) {
        /* Same midpoint math as commit: move the sweep window, keep width. */
        float lo   = drone_get_sweep_lo();
        float hi   = drone_get_sweep_hi();
        float half = (hi - lo) * 0.5f;
        float mid  = filter_norm_to_hz(s_fgraph.cutoff_norm);
        drone_set_sweep_lo(SEQ_CLAMP_F32(mid - half, 65.0f, 7900.0f));
        drone_set_sweep_hi(SEQ_CLAMP_F32(mid + half, 100.0f, 8000.0f));
        drone_set_resonance(s_filter_edit.resonance);
    } else if (filter_tgt_is_drone_std()) {
        drone_std_preview_filter(&s_filter_edit);
    } else if (filter_tgt_is_arp()) {
        arp_preview_filter(&s_filter_edit);
#if CONFIG_SYNTH_WIRELESS
    } else if (filter_tgt_is_live()) {
        live_play_preview_filter(&s_filter_edit);
#endif
    } else {
        uint8_t li = seq_state.active_layer_idx;
        if (s_editor_apply_all) {
            for (uint8_t t = 0; t < SEQ_TRACKS; ++t)
                sequencer_core_preview_melodic_filter(li, t, &s_filter_edit);
        } else {
            sequencer_core_preview_melodic_filter(li, seq_state.selected_track,
                                                  &s_filter_edit);
        }
    }
    s_filter_live = true;
}

static void filter_live_cancel_restore(void)
{
    if (!s_filter_live) return;
    s_filter_live = false;

    if (filter_tgt_is_drone()) {
        drone_set_sweep_lo(s_fdrone_open_lo);
        drone_set_sweep_hi(s_fdrone_open_hi);
        drone_set_resonance(s_fdrone_open_res);
        return;
    }
    if (filter_tgt_is_drone_std()) {
        /* Re-push the stored filter (bypass when it was never enabled). */
        seq_filter_t f;
        drone_std_get_filter(&f);
        drone_std_preview_filter(&f);
        return;
    }
    if (filter_tgt_is_arp()) {
        /* Best effort: re-push the stored filter. A never-authored PATCH-mode
         * arp keeps the previewed bypass until its next patch reload. */
        seq_filter_t f;
        arp_get_filter(&f);
        arp_preview_filter(&f);
        return;
    }
#if CONFIG_SYNTH_WIRELESS
    if (filter_tgt_is_live()) {
        /* Same best-effort as the arp: the live voice is always a patch, so a
         * never-authored filter keeps the previewed bypass until the next
         * patch load rebuilds the slot. */
        seq_filter_t f;
        live_play_get_filter(&f);
        live_play_preview_filter(&f);
        return;
    }
#endif
    uint8_t li = seq_state.active_layer_idx;
    uint8_t t0 = s_editor_apply_all ? 0 : seq_state.selected_track;
    uint8_t t1 = s_editor_apply_all ? (uint8_t)(SEQ_TRACKS - 1)
                                    : seq_state.selected_track;
    bool need_reload = false;
    for (uint8_t t = t0; t <= t1; ++t) {
        if (!sequencer_core_melodic_filter_authored(li, t)) need_reload = true;
    }
    if (need_reload) {
        sequencer_core_reload_layer_synth(li);
        return;
    }
    for (uint8_t t = t0; t <= t1; ++t) {
        seq_filter_t f;
        if (sequencer_core_get_melodic_filter(li, t, &f))
            sequencer_core_preview_melodic_filter(li, t, &f);
    }
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
    s_filter_live = false;
    if (filter_tgt_is_drone()) {
        s_fdrone_open_lo  = drone_get_sweep_lo();
        s_fdrone_open_hi  = drone_get_sweep_hi();
        s_fdrone_open_res = drone_get_resonance();
    }
    s_filter_active = true;
    s_force_redraw  = true;
#if CONFIG_FILTER_SCOPE
    s_fgraph.live_valid = false;
    filter_scope_drop();            /* force a fresh bind for the new target */
    filter_scope_bind_target();
#endif
    ESP_LOGI(TAG, "filter editor open: %s type%u %.0fHz Q%.2f",
             s_fgraph.label, s_filter_edit.filter_type,
             (double)s_filter_edit.cutoff_hz, (double)s_filter_edit.resonance);
}

bool synth_ui_filter_handle_encoder(long delta)
{
    if (!s_filter_active) return false;
    bool drone = (filter_tgt_is_drone());

    /* Arp currently takes the same cursor map and the same edit branches as
     * melodic (see case 2/3 below), so it needs no separate predicate; the
     * arp-specific EG1 sweep depth is the fixed ARP_FILTER_EG1_DEPTH_OCT rather
     * than an editable field. Kept commented as the hook to restore if arp ever
     * regains its own filter-edit behaviour. */
    /* bool arp = (filter_tgt_is_arp()); */

    if (!s_fgraph.editing) {
        /* Not editing: scroll cursor position.
         * Drone: 0=cutoff 1=resonance (type fixed, no EN cursor).
         * Arp/melodic: 0=cutoff 1=resonance 2=feedback 3=type 4=enable, where
         * slot 2 only exists on KS targets and is skipped otherwise. The
         * melodic EG1 sweep depth/polarity live on the envelope editor's EG1
         * page (arp uses the fixed ARP_FILTER_EG1_DEPTH_OCT). */
        uint8_t max_cursor = drone ? 1 : 4;
        int dir = (delta > 0) ? 1 : ((delta < 0) ? -1 : 0);
        if (dir != 0) {
            uint8_t c = s_fgraph.cursor;
            do {
                c = (uint8_t)((c + (uint8_t)(max_cursor + 1) + dir) % (max_cursor + 1));
            } while (c == 2 && !s_fgraph.has_feedback);
            s_fgraph.cursor = c;
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
        case 2: {   /* KS string feedback (feedback targets only) */
            float step = 0.02f * (float)delta;
            s_fgraph.feedback_norm = SEQ_CLAMP_F32(s_fgraph.feedback_norm + step, 0.0f, 1.0f);
            s_filter_edit.feedback = s_fgraph.feedback_norm;
            break;
        }
        case 3: {   /* type (melodic/arp only) */
            if (!drone) {
                int t = (int)s_filter_edit.filter_type + (int)delta;
                /* Wrap within 1..COUNT-1 (NONE is toggled via MY_BUTTON_1, not the type cursor). */
                if (t < 1) t = SEQ_FILTER_COUNT - 1;
                if (t >= SEQ_FILTER_COUNT) t = 1;
                s_filter_edit.filter_type = (uint8_t)t;
                s_fgraph.filter_type      = (uint8_t)t;
            }
            break;
        }
        case 4: {   /* enable toggle (melodic/arp only; drone filter is always on) */
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
    filter_live_push();
    s_force_redraw = true;
    return true;
}

bool synth_ui_filter_handle_button(bool is_long)
{
    if (!s_filter_active) return false;
    if (is_long) {
        /* Long press = cancel: restore the target's state, reload display. */
        filter_live_cancel_restore();
        filter_load_from_target();
        filter_sync_fgraph();
        s_filter_active = false;
        s_force_redraw  = true;
#if CONFIG_FILTER_SCOPE
        filter_scope_drop();
        s_fgraph.live_valid = false;
#endif
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
    filter_live_push();
    s_force_redraw = true;
    ESP_LOGI(TAG, "filter enabled -> %d", (int)s_filter_edit.enabled);
}

bool synth_ui_filter_close_commit(void)
{
    if (!s_filter_active) return false;

#if CONFIG_FILTER_SCOPE
    /* Stop the render-side tap before anything else: from here the target's
     * voices may be rebuilt by the commit below, which would leave the armed
     * oscillator list pointing at slots that no longer belong to this target. */
    filter_scope_drop();
    s_fgraph.live_valid = false;
#endif

#if CONFIG_SYNTH_WIRELESS
    if (synth_ui_wireless_page_is_open()) {
        live_play_set_filter(&s_filter_edit);
        ESP_LOGI(TAG, "filter commit live: type%u %.0fHz Q%.2f en=%d",
                 s_filter_edit.filter_type,
                 (double)s_filter_edit.cutoff_hz, (double)s_filter_edit.resonance,
                 (int)s_filter_edit.enabled);
    } else
#endif
    if (filter_tgt_is_drone()) {
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
    } else if (filter_tgt_is_drone_std()) {
        drone_std_set_filter(&s_filter_edit);
        ESP_LOGI(TAG, "filter commit drone_std: type%u %.0fHz Q%.2f en=%d",
                 s_filter_edit.filter_type,
                 (double)s_filter_edit.cutoff_hz, (double)s_filter_edit.resonance,
                 (int)s_filter_edit.enabled);
    } else if (filter_tgt_is_arp()) {
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

void synth_ui_editors_live_service(void)
{
    graph_amp_live_flush(false);

#if CONFIG_FILTER_SCOPE
    /* Drain the live band once per UI frame, before the view signature is
     * taken. It cannot live inside filter_view_signature(): signature
     * functions are side-effect-free by contract (synth_ui_internal.h), and
     * an epoch bump hidden in one would couple the scope's read cadence to
     * how often the redraw gate happens to hash this view. */
    if (s_filter_active) {
        filter_scope_bind_target();   /* cheap no-op unless the voices changed */
        filter_sync_live_band();
    }
#endif
}

/* ── Filter editor: end ──────────────────────────────────────────────────── */

/* ── LFO editor ──────────────────────────────────────────────────────────── */

uint32_t lfo_view_signature(void)
{
    const seq_lfo_t *l = &s_lfo_view.lfo;
    return (uint32_t)l->enabled
         | ((uint32_t)l->wave    <<  1)   /* 3 bits */
         | ((uint32_t)l->rate    <<  4)   /* 3 bits */
         | ((uint32_t)l->depth   <<  7)   /* 7 bits (0..100) */
         | ((uint32_t)l->targets << 14)   /* 5 bits */
         | ((uint32_t)s_lfo_view.cursor  << 19)   /* 4 bits (0..8) */
         | ((uint32_t)s_lfo_view.editing << 23);
}

bool synth_ui_lfo_is_active(void) { return s_lfo_active; }

void synth_ui_lfo_open(void)
{
#if CONFIG_SYNTH_WIRELESS
    /* Wireless page first, before the mode ladder - it is a menu overlay, so
     * the ui_mode underneath is whatever screen the user came from. The live
     * voice always takes the editor: wave patches drive the native carrier,
     * patch strings the 20 Hz software stepper (live_play_lfo_service). */
    s_lfo_live_target = synth_ui_wireless_page_is_open();
    if (!s_lfo_live_target)
#endif
    {
        // TODO: drone LFO = BPM-multiplier stutter only; implement constrained editor when needed
        if (seq_state.ui_mode == UI_MODE_DRONE) return; // drone has no free LFO editor
    }
    uint8_t li = seq_state.active_layer_idx;
    uint8_t tr = seq_state.selected_track;
    seq_lfo_t existing = {
        .enabled = false,
        .mode    = LFO_MODE_FREE,
        .wave    = LFO_WAVE_SINE,
        .rate    = LFO_RATE_1BAR,
        .depth   = 50,
        .targets = LFO_TGT_BIT(LFO_TARGET_FILTER),
    };
#if CONFIG_SYNTH_WIRELESS
    if (s_lfo_live_target)
        live_play_get_lfo(&existing);
    else
#endif
    if (seq_state.ui_mode == UI_MODE_ARP)
        arp_get_lfo(&existing);
    else if (seq_state.ui_mode == UI_MODE_DRONE_STD)
        drone_std_get_lfo(&existing);
    else
        sequencer_core_get_melodic_lfo(li, tr, &existing);
    s_lfo_view.lfo          = existing;
    s_lfo_view.cursor       = 0;
    s_lfo_view.editing      = false;
    s_lfo_view.layer_idx    = li;
    s_lfo_view.track_idx    = tr;
    s_lfo_view.apply_all    = s_editor_apply_all;
    s_lfo_view.target_label = (seq_state.ui_mode == UI_MODE_ARP)       ? "ARP"
                            : (seq_state.ui_mode == UI_MODE_DRONE_STD) ? "DRONE"
                            : NULL;
#if CONFIG_SYNTH_WIRELESS
    if (s_lfo_live_target) s_lfo_view.target_label = "LIVE";
#endif
    s_lfo_active   = true;
    s_force_redraw = true;
    ESP_LOGI(TAG, "LFO editor open L%u T%u", li + 1u, tr + 1u);
}

bool synth_ui_lfo_handle_encoder(long delta)
{
    if (!s_lfo_active) return false;
    /* Fields: 5 target checkboxes (0..LFO_TARGET_COUNT-1), then WAVE/RATE/DEPTH/EN. */
    const uint8_t N = LFO_FLD_COUNT;
    if (!s_lfo_view.editing) {
        if (delta > 0)      s_lfo_view.cursor = (s_lfo_view.cursor + 1) % N;
        else if (delta < 0) s_lfo_view.cursor = (s_lfo_view.cursor + N - 1) % N;
        s_force_redraw = true;
        return true;
    }
    /* Only the multi-value fields (WAVE/RATE/DEPTH) use adjust mode; checkbox
     * and EN fields toggle on press and never set `editing`. */
    seq_lfo_t *l = &s_lfo_view.lfo;
    int d = (delta > 0) ? 1 : -1;
    switch (s_lfo_view.cursor) {
        case LFO_FLD_WAVE:
            l->wave = (lfo_wave_t)((l->wave + LFO_WAVE_COUNT + d) % LFO_WAVE_COUNT);
            break;
        case LFO_FLD_RATE:
            l->rate = (lfo_rate_t)((l->rate + LFO_RATE_COUNT + d) % LFO_RATE_COUNT);
            break;
        case LFO_FLD_DEPTH: /* ±5%, clamped 0..100 */
            if (d > 0) l->depth = (l->depth < 95) ? l->depth + 5 : 100;
            else       l->depth = (l->depth >  5) ? l->depth - 5 : 0;
            break;
        case LFO_FLD_FLT_OCT: {
            /* Quarter-octave steps. A legacy (sentinel) value materializes at
             * its current effective swing first, so the initial click nudges
             * the sound instead of jumping it. */
            int q = l->flt_oct_q ? (int)l->flt_oct_q
                                 : (int)(((unsigned)l->depth * 12u + 50u) / 100u);
            q = SEQ_CLAMP_INT(q + d, 1, (int)VOICE_LFO_FLT_OCT_Q_MAX);
            l->flt_oct_q = (uint8_t)q;
            break;
        }
        case LFO_FLD_WOB_RATE:
            l->wob_rate = (uint8_t)((l->wob_rate + LFO_RATE_COUNT + d) % LFO_RATE_COUNT);
            break;
        case LFO_FLD_WOB_DEPTH: {
            /* Authored in whole dB of carrier swing (see voice_config.h);
             * 0 dB is the OFF step at the bottom of the range, not a wrap. */
            int db = (int)voice_wob_depth_to_db(l->wob_depth) + d;
            db = SEQ_CLAMP_INT(db, 0, (int)VOICE_WOB_DB_MAX);
            l->wob_depth = voice_wob_db_to_depth((uint8_t)db);
            break;
        }
        default: break;
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
    seq_lfo_t *l = &s_lfo_view.lfo;
    uint8_t c = s_lfo_view.cursor;
    if (c < LFO_TARGET_COUNT) {
        l->targets ^= LFO_TGT_BIT(c);          /* toggle this target in/out of the set */
        s_lfo_view.editing = false;
    } else if (c == LFO_FLD_EN) {
        l->enabled = !l->enabled;              /* boolean: toggle directly */
        s_lfo_view.editing = false;
    } else if (c == LFO_FLD_WOB_MODE) {
        l->wob_depth_only = !l->wob_depth_only; /* boolean: toggle directly */
        s_lfo_view.editing = false;
    } else {
        s_lfo_view.editing = !s_lfo_view.editing;  /* WAVE/RATE/DEPTH/WOB: adjust mode */
    }
    s_force_redraw = true;
    return true;
}

bool synth_ui_lfo_close_commit(void)
{
    if (!s_lfo_active) return false;
#if CONFIG_SYNTH_WIRELESS
    /* Checked before the mode ladder: the Wireless page is an overlay, so the
     * ui_mode underneath is whatever screen the user came from. */
    if (s_lfo_live_target) {
        live_play_set_lfo(&s_lfo_view.lfo);
        s_lfo_active   = false;
        s_force_redraw = true;
        ESP_LOGI(TAG, "LFO editor committed (live voice)");
        return true;
    }
#endif
    if (seq_state.ui_mode == UI_MODE_DRONE) return false; // stutter drone has no free LFO editor
    if (seq_state.ui_mode == UI_MODE_ARP) {
        arp_set_lfo(&s_lfo_view.lfo);
    } else if (seq_state.ui_mode == UI_MODE_DRONE_STD) {
        drone_std_set_lfo(&s_lfo_view.lfo);
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
    /* The live voice has no track scope either, and it must be tested before
     * the ui_mode ladder: its editor runs over the menu overlay, so the mode
     * underneath would otherwise let the chord flip the MELODIC scope while a
     * LIVE editor is showing. */
    if (synth_ui_wireless_page_is_open()) return false;
    if (seq_state.ui_mode == UI_MODE_ARP || seq_state.ui_mode == UI_MODE_DRONE ||
        seq_state.ui_mode == UI_MODE_DRONE_STD)
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
        if (s_graph_eg_index == 0 && graph_target_has_eg1()) {
            /* EG0 -> EG1: same widget, next page (writes the departing
             * envelope through if it was edited). Targets without an EG1 page
             * fall straight through to the filter tab. */
            graph_toggle_eg_index();
            return;
        }
        synth_ui_graph_close_commit();
        synth_ui_filter_open();
    } else if (s_filter_active) {
        synth_ui_filter_close_commit();
        bool no_lfo_tab = (seq_state.ui_mode == UI_MODE_DRONE);
#if CONFIG_SYNTH_WIRELESS
        /* Live voice: the wireless overlay overrides the mode underneath and
         * always has an LFO tab (native for wave patches, software stepper
         * for patch strings - see synth_ui_lfo_open). */
        if (synth_ui_wireless_page_is_open())
            no_lfo_tab = false;
#endif
        if (no_lfo_tab)
            synth_ui_graph_open_envelope(); // skip LFO tab, cycle back to ADSR
        else
            synth_ui_lfo_open();
    } else if (s_lfo_active) {
        synth_ui_lfo_close_commit();
        synth_ui_graph_open_envelope();
    }
}

/* ── LFO editor: end ─────────────────────────────────────────────────────── */

/* ── View signatures ─────────────────────────────────────────────────────── */

uint32_t filter_view_signature(void)
{
    uint32_t h = FNV1A_OFFSET;
    h = fnv1a_bytes(h, &s_fgraph, sizeof(s_fgraph));
    return h;
}

uint32_t graph_view_signature(void)
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
    h = fnv1a_bytes(h, &s_graph_fenv_edit, sizeof(s_graph_fenv_edit));
    h = fnv1a_bytes(h, &s_graph_eg_index, sizeof(s_graph_eg_index));
    h = fnv1a_bytes(h, &s_graph_eg_type_disp, sizeof(s_graph_eg_type_disp));
    /* Tick-derived: flips once when the type-cycle flash window closes, so the
     * top bar reverts without any other state change (polled at the UI rate). */
    uint8_t type_flash = graph_type_flash_active() ? 1u : 0u;
    h = fnv1a_bytes(h, &type_flash, sizeof(type_flash));
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
        snprintf(buf, sizeof(buf), "STUTR %s", eg_tag);
    } else if (s_graph_target == GRAPH_TGT_DRONE_STD) {
        snprintf(buf, sizeof(buf), "DRONE %s", eg_tag);
#if CONFIG_SYNTH_WIRELESS
    } else if (s_graph_target == GRAPH_TGT_LIVE) {
        snprintf(buf, sizeof(buf), "LIVE %s", eg_tag);
#endif
    } else {
        snprintf(buf, sizeof(buf), "L%u T%u %s%s",
                 s_graph_layer + 1, s_graph_track + 1, eg_tag,
                 s_editor_apply_all ? ">L" : ">T");
    }
    u8g2_DrawStr(u8g2, 2, 8, buf);

    /* The middle point readout and the right-side trim readout share the
     * 60..126 px band, so only one may draw per frame (both at once overlap
     * into unreadable doubled text). Point selection is resolved first; the
     * right readout yields to it and returns when the cursor leaves A/D/R. */
    gpopup_point_t pts[GPOPUP_MAX_POINTS];
    uint8_t n = graph_popup_get_points(&s_graph_popup, pts, GPOPUP_MAX_POINTS);
    uint8_t c = s_graph_popup.cursor;
    /* Right after a type cycle the full type name takes the shared band; the
     * point readout yields for the flash window so the change is unmissable. */
    bool type_flash = graph_type_flash_active();
    bool mid_shown = (!type_flash && !s_graph_amp_mode && n >= 4 && c >= 1 && c <= 3);

    /* Right: amp indicator when in amp mode (replaces the old "S/L" range flag
     * which is now set automatically and no longer meaningful to the user).
     * The melodic EG1 page shows the signed sweep depth instead whenever the
     * middle readout is idle, so the shoulder-button polarity flip has a
     * visible readout. */
    uint8_t rw = 0;
    bool eg1_fenv = (s_graph_eg_index == 1 && graph_target_has_eg1_depth());
    if (type_flash) {
        /* Inverted pad so the transient name reads as an event, not a label. */
        const char *tname = graph_eg_type_name(s_graph_eg_type_disp);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        rw = (uint8_t)u8g2_GetStrWidth(u8g2, tname);
        u8g2_DrawBox(u8g2, (uint8_t)(128 - rw - 4), 0, (uint8_t)(rw + 4), 11);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, (uint8_t)(128 - rw - 2), 8, tname);
        u8g2_SetDrawColor(u8g2, 1);
    } else if (s_graph_amp_mode || (eg1_fenv && !mid_shown)) {
        char amp_buf[10];
        if (eg1_fenv) {
            snprintf(amp_buf, sizeof(amp_buf), "ENV%+.2f", (double)s_graph_fenv_edit);
        } else {
            snprintf(amp_buf, sizeof(amp_buf), "AMP%d%%",
                     (int)(s_graph_amp_edit * 100.0f + 0.5f));
        }
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        rw = (uint8_t)u8g2_GetStrWidth(u8g2, amp_buf);
        u8g2_DrawStr(u8g2, (uint8_t)(128 - rw - 2), 8, amp_buf);
    }
    /* No idle-slot fallback: the persistent curve-type code lives in the plot
     * corner (see synth_ui_graph_view_draw), since the point readout occupies
     * this band whenever the ADSR cursor sits on a point — i.e. always. */

    /* Middle: live readout of the selected point's real value (ms / %). */
    if (mid_shown) {
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

    /* Persistent curve-type code, top-right of the plot area. The top bar's
     * right slot is owned by the point readout, so this corner is the one spot
     * where the type stays visible while editing. A cleared pad keeps it
     * legible on the rare frames the curve passes underneath. */
    const char *tcode = graph_eg_type_code(s_graph_eg_type_disp);
    u8g2_SetFont(u8g2, u8g2_font_4x6_tr);
    uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, tcode);
    uint8_t tx = (uint8_t)(128 - tw - 2);
    uint8_t ty = (uint8_t)(GRAPH_TOPBAR_H + 8);
    u8g2_SetDrawColor(u8g2, 0);
    u8g2_DrawBox(u8g2, (uint8_t)(tx - 1), (uint8_t)(ty - 6), (uint8_t)(tw + 3), 8);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_DrawStr(u8g2, tx, ty, tcode);
}

void synth_ui_filter_view_draw(u8g2_t *u8g2)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    filter_graph_draw(u8g2, &s_fgraph);
}

void synth_ui_lfo_view_draw(u8g2_t *u8g2)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    lfo_view_draw(u8g2, &s_lfo_view);
}

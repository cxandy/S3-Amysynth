/*
 * graph_popup — reusable pop-up graph box implementation.
 * See graph_popup.h for the API contract and design notes.
 */

#include "graph_popup.h"
#include "seq_clamp.h"
#include <string.h>
#include <stdio.h>

/* ── File-local helpers ─────────────────────────────────────────────────── */

/* Normalised-coordinate clamp. Via seq_clamp_f32 so NaN degrades to 0.0f
 * instead of propagating: these results feed (int) casts on the draw path,
 * and NaN->int is undefined behaviour. */
static inline float clampf01(float v) { return SEQ_CLAMP_F32(v, 0.0f, 1.0f); }

/* Inset (in px) of the plot region inside the box border. Leaves room for the
 * frame and an optional title strip at the top. */
#define GPOPUP_PAD_X    4
#define GPOPUP_PAD_TOP  4
#define GPOPUP_PAD_BOT  4
#define GPOPUP_TITLE_H  8   /* extra top inset when a title is shown */

/* Fallback minimum normalised X gap between points until a host installs its
 * own via graph_popup_set_min_x_gap(). Sized for marker legibility on a 128 px
 * plot; a host that knows the real floor in its own units should install that,
 * so the plot is never the stricter constraint. */
#define GPOPUP_DEFAULT_MIN_X_GAP 0.004f

/* Accepted range for an installed gap. Zero is legal (no spacing rule); the
 * ceiling stops a bad conversion from silently distorting real edits. */
#define GPOPUP_MIN_X_GAP_LO      0.0f
#define GPOPUP_MIN_X_GAP_HI      0.05f

/* Encoder step size for value adjustment (normalised units per detent). */
#define GPOPUP_ADJUST_STEP 0.02f

/* Compute the inner plot rectangle (in absolute screen pixels). */
static void plot_rect(const gpopup_t *p,
                      uint8_t *px, uint8_t *py, uint8_t *pw, uint8_t *ph)
{
    /* ADSR style has no internal title strip (host draws the top bar), uses
     * tighter insets, and reserves 3px at the bottom for tick marks. */
    bool adsr = (p->style == GPOPUP_STYLE_ADSR);
    uint8_t top_pad = adsr ? 2 : (GPOPUP_PAD_TOP + (p->title ? GPOPUP_TITLE_H : 0));
    uint8_t bot_pad = adsr ? (GPOPUP_PAD_BOT + 3) : GPOPUP_PAD_BOT;
    uint8_t side_pad = adsr ? 2 : GPOPUP_PAD_X;
    *px = (uint8_t)(p->x + side_pad);
    *py = (uint8_t)(p->y + top_pad);
    *pw = (uint8_t)(p->w - 2 * side_pad);
    *ph = (uint8_t)(p->h - top_pad - bot_pad);
}

/* Map a normalised point to absolute screen pixels within the plot rect.
 * Y is inverted (screen y grows downward). */
static void point_to_px(const gpopup_t *p, const gpopup_point_t *pt,
                        uint8_t plot_x, uint8_t plot_y,
                        uint8_t plot_w, uint8_t plot_h,
                        int *out_x, int *out_y)
{
    float xn = clampf01(pt->x);
    float yn = clampf01(pt->y);
    *out_x = (int)(plot_x + xn * (plot_w - 1));
    *out_y = (int)(plot_y + (1.0f - yn) * (plot_h - 1));
}

/* Forward declarations for the ADSR helpers (defined below). */
static void adsr_apply_constraints(gpopup_t *p);
static bool adsr_y_editable(const gpopup_t *p, uint8_t idx);
static bool adsr_x_editable(const gpopup_t *p, uint8_t idx);

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

void graph_popup_init(gpopup_t *p, uint8_t x, uint8_t y, uint8_t w, uint8_t h)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->x = x;
    p->y = y;
    p->w = w;
    p->h = h;
    p->mode = GPOPUP_MODE_VIEW;
    p->active = false;
    p->min_x_gap = GPOPUP_DEFAULT_MIN_X_GAP;
}

void graph_popup_set_min_x_gap(gpopup_t *p, float gap)
{
    if (!p) return;
    p->min_x_gap = SEQ_CLAMP_F32(gap, GPOPUP_MIN_X_GAP_LO, GPOPUP_MIN_X_GAP_HI);
}

void graph_popup_set_points(gpopup_t *p, const gpopup_point_t *pts, uint8_t n)
{
    if (!p) return;
    if (n > GPOPUP_MAX_POINTS) n = GPOPUP_MAX_POINTS;
    if (pts) {
        for (uint8_t i = 0; i < n; ++i) {
            p->points[i].x = clampf01(pts[i].x);
            p->points[i].y = clampf01(pts[i].y);
        }
    }
    p->num_points = n;
    if (p->cursor >= n) p->cursor = (n > 0) ? (uint8_t)(n - 1) : 0;
    /* Keep the cursor off the fixed origin in ADSR style. */
    if (p->style == GPOPUP_STYLE_ADSR && n >= 2 && p->cursor == 0) p->cursor = 1;
    adsr_apply_constraints(p);
}

void graph_popup_open(gpopup_t *p, gpopup_mode_t mode, const char *title)
{
    if (!p) return;
    p->mode = mode;
    p->title = title;
    p->cursor = 0;
    p->editing_value = false;
    p->adjust_axis_y = true;
    p->active = true;
}

void graph_popup_set_style(gpopup_t *p, gpopup_style_t style)
{
    if (!p) return;
    p->style = style;
}

void graph_popup_set_ticks(gpopup_t *p, const float *xs, uint8_t n)
{
    if (!p) return;
    if (n > GPOPUP_MAX_POINTS) n = GPOPUP_MAX_POINTS;
    if (xs) {
        for (uint8_t i = 0; i < n; ++i) p->ticks[i] = clampf01(xs[i]);
    } else {
        n = 0;
    }
    p->num_ticks = n;
}

void graph_popup_set_adsr_lock_sx(gpopup_t *p, bool lock)
{
    if (!p) return;
    p->adsr_lock_sx = lock;
}

void graph_popup_set_shape(gpopup_t *p, gpopup_shape_fn fn)
{
    if (!p) return;
    p->shape = fn;
}

void graph_popup_set_xstep(gpopup_t *p, gpopup_xstep_fn fn)
{
    if (!p) return;
    p->xstep = fn;
}

/* ── ADSR role-aware constraints ─────────────────────────────────────────────
 * Points are [origin, A(attack peak), D(decay end / sustain level), R(release
 * end)] - index roles assume the host's standard 4-point ADSR seed:
 *   - X monotonic with a minimum gap so points can't cross or collapse.
 *   - origin pinned to (0,0); A.y pinned to 1.0; R.y pinned to 0.0.
 *   - only D's Y (the sustain level) is freely movable. */
static void adsr_apply_constraints(gpopup_t *p)
{
    if (p->style != GPOPUP_STYLE_ADSR || p->num_points < 4) return;

    gpopup_point_t *pt = p->points;

    /* Role-fixed Y levels. */
    pt[0].x = 0.0f; pt[0].y = 0.0f;   /* origin       */
    pt[1].y = 1.0f;                   /* attack peak  */
    pt[3].y = 0.0f;                   /* release end  */
    /* pt[2].y is the sustain level — left as edited, just bounded 0..1. */
    pt[2].y = clampf01(pt[2].y);

    /* Monotonic X with a minimum gap: forward pass bounds each point below by
     * its predecessor + gap, backward pass keeps an earlier point from being
     * pushed past a later one that was dragged left. */
    for (uint8_t i = 1; i < p->num_points; ++i) {
        float lo = pt[i - 1].x + p->min_x_gap;
        if (pt[i].x < lo) pt[i].x = lo;
        if (pt[i].x > 1.0f) pt[i].x = 1.0f;
    }
    for (int i = (int)p->num_points - 2; i >= 1; --i) {
        float hi = pt[i + 1].x - p->min_x_gap;
        if (pt[i].x > hi) pt[i].x = hi;
        if (pt[i].x < 0.0f) pt[i].x = 0.0f;
    }
}

/* Whether the selected point's Y is editable in ADSR style. Only the sustain
 * point (index 2) has a free level; A/R levels are role-fixed. */
static bool adsr_y_editable(const gpopup_t *p, uint8_t idx)
{
    if (p->style != GPOPUP_STYLE_ADSR) return true;
    return (idx == 2);
}

/* Whether the selected point's X (time) is editable in ADSR style. With
 * adsr_lock_sx the sustain point (2) is Y-only - its X is the host's
 * auto-derived decay time. A and R stay X-editable; the origin never is. */
static bool adsr_x_editable(const gpopup_t *p, uint8_t idx)
{
    if (p->style != GPOPUP_STYLE_ADSR) return true;
    if (idx == 0) return false;                 /* origin is pinned */
    if (p->adsr_lock_sx && idx == 2) return false; /* S.x host-owned */
    return true;
}

/* True when a point moves on BOTH axes: the explicit-decay sustain point
 * (lock_sx off) carries decay time (X) and sustain level (Y). With lock_sx on
 * no point is dual-free and the two-axis handling below stays dormant. */
static bool adsr_point_dual_free(const gpopup_t *p, uint8_t idx)
{
    return p->style == GPOPUP_STYLE_ADSR
        && adsr_x_editable(p, idx) && adsr_y_editable(p, idx);
}

/* Returns true if the encoder should adjust Y, false for X. Single-axis ADSR
 * points (A/R = time/X, S = level/Y when its X is host-owned) auto-select, so
 * there is nothing to toggle; the dual-free sustain point and the generic
 * (non-ADSR) editor honour the manual adjust_axis_y flag. */
static bool adsr_effective_axis_is_y(const gpopup_t *p, uint8_t idx)
{
    if (p->style != GPOPUP_STYLE_ADSR) return p->adjust_axis_y;
    if (adsr_y_editable(p, idx) && !adsr_x_editable(p, idx)) return true;
    if (adsr_x_editable(p, idx) && !adsr_y_editable(p, idx)) return false;
    /* Both free (explicit-decay sustain): honour the axis flag. Neither free
     * (origin): value is irrelevant, default to Y. */
    return p->adjust_axis_y;
}

void graph_popup_close(gpopup_t *p)
{
    if (!p) return;
    p->active = false;
    p->editing_value = false;
}

bool graph_popup_is_active(const gpopup_t *p)
{
    return p && p->active;
}

uint8_t graph_popup_get_points(const gpopup_t *p, gpopup_point_t *out, uint8_t max_out)
{
    if (!p) return 0;
    uint8_t n = p->num_points;
    if (out) {
        if (n > max_out) n = max_out;
        memcpy(out, p->points, n * sizeof(gpopup_point_t));
    }
    return n;
}

/* ── Render ─────────────────────────────────────────────────────────────── */

/* Interpolate the curve's screen-Y at a given screen column, walking the
 * point segments. Returns the baseline if the column is outside the curve. */
static int curve_y_at_col(const gpopup_t *p, int col,
                          uint8_t plot_x, uint8_t plot_y,
                          uint8_t plot_w, uint8_t plot_h)
{
    int baseline = (int)(plot_y + plot_h - 1);
    int px0, py0, px1, py1;
    point_to_px(p, &p->points[0], plot_x, plot_y, plot_w, plot_h, &px0, &py0);
    if (col <= px0) return py0;
    /* Holds the first point's level when there is no segment to walk. */
    py1 = py0;
    for (uint8_t i = 1; i < p->num_points; ++i) {
        point_to_px(p, &p->points[i], plot_x, plot_y, plot_w, plot_h, &px1, &py1);
        if (col <= px1) {
            int span = px1 - px0;
            if (span <= 0) return py1;
            if (p->shape) {
                /* Evaluate in normalised level space so the callback sees the
                 * same v0/v1/t domain the synth engine uses. */
                float t  = (float)(col - px0) / (float)span;
                float yn = p->shape(clampf01(p->points[i - 1].y),
                                    clampf01(p->points[i].y), t);
                return (int)(plot_y + (1.0f - clampf01(yn)) * (plot_h - 1));
            }
            return py0 + ((py1 - py0) * (col - px0)) / span;
        }
        px0 = px1; py0 = py1;
    }
    (void)baseline;
    return py1; /* past the last point: hold its level */
}

void graph_popup_draw(u8g2_t *u8g2, const gpopup_t *p)
{
    if (!u8g2 || !p || !p->active) return;

    const bool adsr = (p->style == GPOPUP_STYLE_ADSR);

    /* ADSR style is full-screen and framed by the host's top bar, so it skips
     * the rounded border for max plot area. */
    u8g2_SetDrawColor(u8g2, 0);
    u8g2_DrawBox(u8g2, p->x, p->y, p->w, p->h);
    u8g2_SetDrawColor(u8g2, 1);
    if (!adsr) {
        u8g2_DrawRFrame(u8g2, p->x, p->y, p->w, p->h, 2);
    }

    /* Optional title (PLAIN style only; ADSR uses the host top bar). */
    if (p->title && !adsr) {
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        u8g2_DrawStr(u8g2, (uint8_t)(p->x + 3), (uint8_t)(p->y + 7), p->title);
    }

    uint8_t plot_x, plot_y, plot_w, plot_h;
    plot_rect(p, &plot_x, &plot_y, &plot_w, &plot_h);

    int baseline = (int)(plot_y + plot_h - 1);

    /* Axes: bottom + left of the plot region. */
    u8g2_DrawHLine(u8g2, plot_x, (uint8_t)baseline, plot_w);
    u8g2_DrawVLine(u8g2, plot_x, plot_y, plot_h);

    if (p->num_points == 0) return;

    /* ADSR: fill under the curve first (one VLine per column down to the
     * baseline) so the points and curve edge sit on top. */
    if (adsr) {
        int last_x;
        {
            int lx, ly;
            point_to_px(p, &p->points[p->num_points - 1], plot_x, plot_y,
                        plot_w, plot_h, &lx, &ly);
            last_x = lx;
        }
        for (int cx = (int)plot_x; cx <= last_x; ++cx) {
            int cy = curve_y_at_col(p, cx, plot_x, plot_y, plot_w, plot_h);
            cy = SEQ_CLAMP_INT(cy, (int)plot_y, baseline);
            if (baseline - cy > 0) {
                u8g2_DrawVLine(u8g2, (uint8_t)cx, (uint8_t)cy,
                               (uint8_t)(baseline - cy));
            }
        }
    }

    /* Curve, on top of any fill. With a shape callback the path is evaluated
     * per column so non-linear segments render their true profile, joining
     * adjacent columns with lines to keep steep runs solid; otherwise straight
     * segments between points. */
    if (p->shape && p->num_points > 1) {
        int fx, fy, lx, ly;
        point_to_px(p, &p->points[0], plot_x, plot_y, plot_w, plot_h, &fx, &fy);
        point_to_px(p, &p->points[p->num_points - 1], plot_x, plot_y,
                    plot_w, plot_h, &lx, &ly);
        int prev_cx = fx, prev_cy = fy;
        for (int cx = fx; cx <= lx; ++cx) {
            int cy = curve_y_at_col(p, cx, plot_x, plot_y, plot_w, plot_h);
            cy = SEQ_CLAMP_INT(cy, (int)plot_y, baseline);
            u8g2_DrawLine(u8g2, prev_cx, prev_cy, cx, cy);
            prev_cx = cx;
            prev_cy = cy;
        }
    } else {
        int prev_x = 0, prev_y = 0;
        for (uint8_t i = 0; i < p->num_points; ++i) {
            int cx, cy;
            point_to_px(p, &p->points[i], plot_x, plot_y, plot_w, plot_h, &cx, &cy);
            if (i > 0) {
                u8g2_DrawLine(u8g2, prev_x, prev_y, cx, cy);
            }
            prev_x = cx;
            prev_y = cy;
        }
    }

    /* ADSR: minimal time tick marks in the bottom margin (just under the axis).
     * Spacing reflects the host's (possibly non-linear) time->X mapping. */
    if (adsr && p->num_ticks > 0) {
        int ty = baseline + 1;
        for (uint8_t i = 0; i < p->num_ticks; ++i) {
            int tx = (int)(plot_x + clampf01(p->ticks[i]) * (plot_w - 1));
            u8g2_DrawVLine(u8g2, (uint8_t)tx, (uint8_t)ty, 2);
        }
    }

    /* ADSR: amplitude ticks at the quarter divisions in the 2px left margin,
     * so both edges carry a scale (0%/100% are the axis ends). */
    if (adsr && plot_x >= 2) {
        for (uint8_t i = 1; i < 4; ++i) {   /* 25%, 50%, 75% */
            int tyy = (int)plot_y + (int)((4u - i) * (plot_h - 1)) / 4;
            u8g2_DrawHLine(u8g2, (uint8_t)(plot_x - 2), (uint8_t)tyy, 2);
        }
    }

    /* Edit-mode markers + cursor highlight. */
    if (p->mode == GPOPUP_MODE_EDIT) {
        /* ADSR letter labels for the editable points (A, D/S, R). */
        static const char adsr_letters[4] = { 0, 'A', 'S', 'R' };

        for (uint8_t i = 0; i < p->num_points; ++i) {
            int cx, cy;
            point_to_px(p, &p->points[i], plot_x, plot_y, plot_w, plot_h, &cx, &cy);

            /* Label above-left of the point (ADSR style, skip the origin). The
             * sustain marker reads 'D' while its decay-time (X) axis is being
             * adjusted, 'S' otherwise, so the dual-axis point shows which
             * value the encoder moves. */
            char letter = adsr_letters[i];
            if (i == 2 && i == p->cursor && p->editing_value
                && adsr_point_dual_free(p, i) && !adsr_effective_axis_is_y(p, i)) {
                letter = 'D';
            }
            if (adsr && i > 0 && i < 4 && letter) {
                char lb[2] = { letter, 0 };
                u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
                int lx = cx - 2;
                int ly = cy - 4;            /* sit above the disc */
                if (ly < (int)plot_y + 6) ly = cy + 11; /* flip below if cramped */
                lx = SEQ_CLAMP_INT(lx, (int)plot_x, (int)(plot_x + plot_w - 5));
                if (i == p->cursor) {
                    /* Emphasise the selected label with an inverted pad. */
                    u8g2_DrawBox(u8g2, (uint8_t)(lx - 1), (uint8_t)(ly - 6), 7, 8);
                    u8g2_SetDrawColor(u8g2, 0);
                    u8g2_DrawStr(u8g2, (uint8_t)lx, (uint8_t)ly, lb);
                    u8g2_SetDrawColor(u8g2, 1);
                } else {
                    u8g2_DrawStr(u8g2, (uint8_t)lx, (uint8_t)ly, lb);
                }
            }

            if (i == p->cursor) {
                /* Selected point: filled disc; when adjusting, ring around it. */
                u8g2_DrawDisc(u8g2, cx, cy, 2, U8G2_DRAW_ALL);
                if (p->editing_value) {
                    u8g2_DrawCircle(u8g2, cx, cy, 4, U8G2_DRAW_ALL);
                }
            } else if (!adsr || i > 0) {
                /* Skip a marker on the fixed origin in ADSR style. */
                u8g2_DrawFrame(u8g2, cx - 1, cy - 1, 3, 3);
            }
        }

        /* Value readout while adjusting (PLAIN only; in ADSR style the host top
         * bar carries the real ms/percent readout). */
        if (!adsr && p->editing_value && p->cursor < p->num_points) {
            char buf[16];
            const gpopup_point_t *pt = &p->points[p->cursor];
            /* uint8_t so the percentages are provably three digits: the 0..100
             * float bound does not survive the float->int conversion, and
             * -Wformat-truncation then assumes a full-width int per %d. */
            uint8_t rx = SEQ_CLAMP_U8((int)(clampf01(pt->x) * 100.0f + 0.5f), 0, 100);
            uint8_t ry = SEQ_CLAMP_U8((int)(clampf01(pt->y) * 100.0f + 0.5f), 0, 100);
            snprintf(buf, sizeof(buf), "%c %d,%d",
                     p->adjust_axis_y ? 'V' : 'H', rx, ry);
            u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
            /* Bottom-right inside the box. */
            uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, buf);
            u8g2_DrawStr(u8g2,
                         (uint8_t)(p->x + p->w - tw - 3),
                         (uint8_t)(p->y + p->h - 3),
                         buf);
        }
    }
}

/* ── Input ──────────────────────────────────────────────────────────────── */

gpopup_result_t graph_popup_handle_encoder(gpopup_t *p, long delta)
{
    if (!p || !p->active || delta == 0) return GPOPUP_RESULT_NONE;
    if (p->mode != GPOPUP_MODE_EDIT || p->num_points == 0) {
        return GPOPUP_RESULT_NONE;
    }

    /* In ADSR style the origin (index 0) is fixed and not selectable. */
    long min_idx = (p->style == GPOPUP_STYLE_ADSR) ? 1 : 0;

    if (!p->editing_value) {
        /* Selecting: move the cursor between points, clamped to ends. */
        long idx = (long)p->cursor + delta;
        idx = SEQ_CLAMP_INT(idx, min_idx, (long)p->num_points - 1);
        p->cursor = (uint8_t)idx;
    } else {
        /* Adjusting along the point's editable axis (auto-selected in ADSR
         * mode, manual adjust_axis_y flag in the generic editor). */
        gpopup_point_t *pt = &p->points[p->cursor];
        float step = (float)delta * GPOPUP_ADJUST_STEP;
        if (adsr_effective_axis_is_y(p, p->cursor)) {
            /* Role-fixed levels (A peak, R end) ignore Y nudges in ADSR mode. */
            if (adsr_y_editable(p, p->cursor)) {
                pt->y = clampf01(pt->y + step);
            }
        } else {
            /* Locked points (origin, host-owned S.x) ignore X nudges. An
             * installed xstep (audio-taper time stride) wins over the fixed
             * normalised step. */
            if (adsr_x_editable(p, p->cursor)) {
                if (p->xstep) {
                    pt->x = clampf01(p->xstep(p->cursor, pt->x, delta));
                } else {
                    pt->x = clampf01(pt->x + step);
                }
            }
        }
        /* Re-impose ADSR validity (monotonic time, role-fixed levels). */
        adsr_apply_constraints(p);
    }
    return GPOPUP_RESULT_NONE;
}

gpopup_result_t graph_popup_handle_button(gpopup_t *p)
{
    if (!p || !p->active) return GPOPUP_RESULT_NONE;
    if (p->mode != GPOPUP_MODE_EDIT) {
        /* VIEW mode: short press confirms (dismiss the preview). */
        return GPOPUP_RESULT_CONFIRMED;
    }
    /* EDIT mode. A dual-axis point carries two values on one marker, so a
     * single toggle can't reach both: cycle select -> level (Y) -> decay time
     * (X) -> select. Single-axis points keep the plain select/adjust toggle. */
    if (adsr_point_dual_free(p, p->cursor)) {
        if (!p->editing_value) {
            p->editing_value = true;
            p->adjust_axis_y = true;    /* start on the sustain level */
        } else if (p->adjust_axis_y) {
            p->adjust_axis_y = false;   /* switch to the decay time */
        } else {
            p->editing_value = false;   /* back to point selection */
            p->adjust_axis_y = true;    /* reset default for the next entry */
        }
        return GPOPUP_RESULT_NONE;
    }
    p->editing_value = !p->editing_value;
    return GPOPUP_RESULT_NONE;
}

gpopup_result_t graph_popup_handle_button_long(gpopup_t *p)
{
    if (!p || !p->active) return GPOPUP_RESULT_NONE;
    /* Long press always cancels, from any mode/state. */
    return GPOPUP_RESULT_CANCELLED;
}


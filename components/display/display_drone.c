#include "display_drone.h"
#include <stdio.h>

/* Layout mirrors the menu overlay: a title bar then up to 5 visible rows,
 * scrolling to keep the cursor in view. 128x64 OLED. */
#define DRONE_TITLE_Y    8
#define DRONE_ROW_H      10
#define DRONE_FIRST_ROW  20
#define DRONE_VIS_ROWS   4

void display_drone_draw_frame(u8g2_t *u8g2, const drone_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);

    /* Title bar. */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2, 2, DRONE_TITLE_Y, "DRONE");
    u8g2_DrawHLine(u8g2, 0, 10, 128);

    if (view == NULL || view->count == 0) {
        u8g2_SendBuffer(u8g2);
        return;
    }

    /* Compute the scroll window so the cursor is always visible. */
    uint8_t first = 0;
    if (view->cursor >= DRONE_VIS_ROWS) {
        first = (uint8_t)(view->cursor - (DRONE_VIS_ROWS - 1));
    }
    uint8_t last = (uint8_t)(first + DRONE_VIS_ROWS);
    if (last > view->count) last = view->count;

    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    for (uint8_t i = first; i < last; i++) {
        const drone_row_view_t *r = &view->rows[i];
        uint8_t row = (uint8_t)(i - first);
        uint8_t y   = (uint8_t)(DRONE_FIRST_ROW + row * DRONE_ROW_H);
        bool selected = (i == view->cursor);

        if (selected) {
            u8g2_DrawBox(u8g2, 0, (uint8_t)(y - 9), 128, DRONE_ROW_H);
            u8g2_SetDrawColor(u8g2, 0);
        }

        u8g2_DrawStr(u8g2, 2, y, r->label);

        if (r->value[0] != '\0') {
            uint8_t vw = (uint8_t)u8g2_GetStrWidth(u8g2, r->value);
            uint8_t vx = (vw < 124) ? (uint8_t)(126 - vw) : 2;
            if (selected && view->editing) {
                u8g2_DrawFrame(u8g2, (uint8_t)(vx - 2), (uint8_t)(y - 9),
                               (uint8_t)(vw + 4), DRONE_ROW_H);
            }
            u8g2_DrawStr(u8g2, vx, y, r->value);
        }

        if (selected) {
            u8g2_SetDrawColor(u8g2, 1);
        }
    }

    /* Scroll affordances. */
    if (first > 0) {
        u8g2_DrawTriangle(u8g2, 124, 14, 120, 18, 128, 18);
    }
    if (last < view->count) {
        u8g2_DrawTriangle(u8g2, 120, 60, 128, 60, 124, 64);
    }

    u8g2_SendBuffer(u8g2);
}

/* ── Drone visualiser overlay ───────────────────────────────────────────────
 *
 * 128×64 layout:
 *
 *  Y  0- 8  Title "DRONE VIS"   +   rate + sweep speed right-aligned
 *  Y  9     Divider line
 *           [Q needle 0–5px above the sweep bar at its midpoint]
 *  Y 11-24  SWEEP  [lo────▓▓▓▓▓▓▓────hi]   filled=range, notch=mid
 *  Y 27-40  AMP    [░░░░|████████|░░░░░]   floor|active range|headroom
 *                         ↑DC centre tick;  "!" at right if clip
 *  Y 43-62  PAT    8 step squares; gate fill height per active step
 */

/* Clamp a float to [0,1] without pulling in math.h. */
static inline float vis_clamp01(float v)
{
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

/* Rate names mirroring drone_core.c s_rate_names (display-only copy). */
static const char * const s_vis_rate_names[] = { "1/4", "1/8", "1/16", "1/32" };
#define VIS_RATE_COUNT 4

void display_drone_vis_draw(u8g2_t *u8g2, const drone_vis_t *vis)
{
    if (!u8g2 || !vis) return;

    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);

    /* ── Title bar ── */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2, 2, 8, "DRONE VIS");

    /* Rate + sweep-speed label, right-aligned in the title bar. */
    {
        char info[12];
        const char *rn = (vis->rate_idx < VIS_RATE_COUNT)
                         ? s_vis_rate_names[vis->rate_idx] : "?";
        snprintf(info, sizeof(info), "%s %ub", rn, (unsigned)vis->sweep_bars);
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        uint8_t iw = (uint8_t)u8g2_GetStrWidth(u8g2, info);
        u8g2_DrawStr(u8g2, (uint8_t)(126 - iw), 8, info);
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    }

    u8g2_DrawHLine(u8g2, 0, 9, 128);

    /* ── Shared bar geometry ── */
    const uint8_t BAR_X = 38;
    const uint8_t BAR_W = 88;
    const uint8_t BAR_H = 7;

    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);

    /* ── SWEEP section ── */
    {
        const uint8_t BY = 13;   /* top of sweep bar */

        u8g2_DrawStr(u8g2, 0, 20, "SWEEP");
        u8g2_DrawFrame(u8g2, BAR_X, BY, BAR_W, BAR_H);

        float lo = vis_clamp01(vis->sweep_lo_norm);
        float hi = vis_clamp01(vis->sweep_hi_norm);

        /* Filled region = sweep range */
        if (hi > lo) {
            uint8_t fx = (uint8_t)(BAR_X + 1 + lo * (float)(BAR_W - 2));
            uint8_t fw = (uint8_t)((hi - lo) * (float)(BAR_W - 2));
            if (fw < 1) fw = 1;
            u8g2_DrawBox(u8g2, fx, (uint8_t)(BY + 1), fw, (uint8_t)(BAR_H - 2));
        }

        /* Inverted notch at sweep midpoint */
        float mid = (lo + hi) * 0.5f;
        uint8_t mx = (uint8_t)(BAR_X + 1 + mid * (float)(BAR_W - 2));
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawVLine(u8g2, mx, (uint8_t)(BY + 1), (uint8_t)(BAR_H - 2));
        u8g2_SetDrawColor(u8g2, 1);

        /* Resonance needle: solid box above the bar at the midpoint.
         * Height 0..5px proportional to Q (0.1..8.0). */
        uint8_t q_h = (uint8_t)(vis->resonance / 8.0f * 5.0f + 0.5f);
        if (q_h > 5) q_h = 5;
        if (q_h > 0) {
            uint8_t nx = (mx > 0) ? (uint8_t)(mx - 1) : 0;
            u8g2_DrawBox(u8g2, nx, (uint8_t)(BY - q_h), 3, q_h);
        }
    }

    /* ── AMP section ── */
    {
        const uint8_t BY = 26;   /* top of amp bar */

        if (vis->wave_mode) {
            u8g2_DrawStr(u8g2, 0, 33, "AMP");
            u8g2_DrawFrame(u8g2, BAR_X, BY, BAR_W, BAR_H);

            float fl = vis_clamp01(vis->amp_floor_norm);
            float cl = vis_clamp01(vis->amp_ceil_norm);
            float dc = vis_clamp01(vis->amp_const);

            /* Solid fill: floor → ceiling (the LFO sweeps through this range) */
            if (cl > fl) {
                uint8_t fx = (uint8_t)(BAR_X + 1 + fl * (float)(BAR_W - 2));
                uint8_t fw = (uint8_t)((cl - fl) * (float)(BAR_W - 2));
                if (fw < 1) fw = 1;
                u8g2_DrawBox(u8g2, fx, (uint8_t)(BY + 1), fw, (uint8_t)(BAR_H - 2));
            }

            /* DC centre tick (inverted column) at amp_const position */
            uint8_t dcx = (uint8_t)(BAR_X + 1 + dc * (float)(BAR_W - 2));
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawVLine(u8g2, dcx, (uint8_t)(BY + 1), (uint8_t)(BAR_H - 2));
            u8g2_SetDrawColor(u8g2, 1);

            /* Clip warning: "!" just right of the bar when ceiling hits 1.0 */
            if (vis->amp_ceil_norm >= 0.99f) {
                u8g2_DrawStr(u8g2, (uint8_t)(BAR_X + BAR_W + 2), 33, "!");
            }
        } else {
            u8g2_DrawStr(u8g2, 0, 33, "AMP  [PATCH]");
        }
    }

    /* ── PATTERN section ── */
    {
        u8g2_DrawStr(u8g2, 0, 43, "PAT");

        const uint8_t SQ_W  = 10;
        const uint8_t SQ_H  = 14;
        const uint8_t SQ_Y  = 47;
        const uint8_t SQ_X0 = BAR_X;

        for (uint8_t s = 0; s < 8; s++) {
            uint8_t sx = (uint8_t)(SQ_X0 + s * (SQ_W + 1));
            bool on = (vis->pattern_mask >> s) & 1u;

            u8g2_DrawFrame(u8g2, sx, SQ_Y, SQ_W, SQ_H);

            if (on) {
                float g = vis_clamp01(vis->gate_len);
                uint8_t fill_h = (uint8_t)(g * (float)(SQ_H - 2));
                if (fill_h < 1) fill_h = 1;
                uint8_t fill_y = (uint8_t)(SQ_Y + 1 + (SQ_H - 2) - fill_h);
                u8g2_DrawBox(u8g2, (uint8_t)(sx + 1), fill_y,
                             (uint8_t)(SQ_W - 2), fill_h);
            }
        }
    }

    u8g2_SendBuffer(u8g2);
}

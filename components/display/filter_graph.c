#include "filter_graph.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── Layout ── */
#define FG_TOPBAR_H  16          /* rows 0..15 */
#define FG_PLOT_W   128
#define FG_PLOT_H   (64 - FG_TOPBAR_H)   /* 48 rows */
#define FG_PLOT_Y0  FG_TOPBAR_H

/* log2(MAX/MIN) = log2(8000/20) ≈ 8.644, precomputed — unused but kept for reference. */
#define FG_LOG_SPAN  8.644f

/* Passband is displayed at 75% of plot height, leaving 25% headroom for
 * resonance spikes (which can reach up to 100% for Q > ~1.33). */
#define FG_PASSBAND_NORM  0.75f

/* Number of log-spaced frequency samples for the response curve. */
#define FG_NPTS  24

static const char * const s_type_names[FGRAPH_FILTER_COUNT] = {
    "NONE", "LPF", "BPF", "HPF", "LPF24"
};

/* ── Response functions (2nd-order biquad magnitude, unclipped) ── */

static float lpf2_mag(float r, float Q)
{
    float d1 = 1.0f - r * r;
    float d2 = r / Q;
    return 1.0f / sqrtf(d1 * d1 + d2 * d2 + 1e-12f);
}

static float hpf2_mag(float r, float Q)
{
    float r2 = r * r;
    float d1 = 1.0f - r2;
    float d2 = r / Q;
    return r2 / sqrtf(d1 * d1 + d2 * d2 + 1e-12f);
}

static float bpf2_mag(float r, float Q)
{
    float num = r / Q;
    float d1  = 1.0f - r * r;
    float d2  = r / Q;
    return num / sqrtf(d1 * d1 + d2 * d2 + 1e-12f);
}

/* Compute display magnitude (0..1) for a given frequency f (Hz),
 * cutoff fc (Hz), Q, and filter type.
 * Passband = FG_PASSBAND_NORM; resonance spike may push above that, capped at 1. */
static float filter_display_mag(uint8_t type, float f, float fc, float Q)
{
    if (fc < 1.0f) fc = 1.0f;
    float r = f / fc;
    if (r < 1e-4f) r = 1e-4f;
    float mag;
    switch (type) {
        case FGRAPH_FILTER_LPF: {
            float m = lpf2_mag(r, Q);
            mag = m * FG_PASSBAND_NORM;
            break;
        }
        case FGRAPH_FILTER_LPF24: {
            float m = lpf2_mag(r, Q);
            mag = m * m * FG_PASSBAND_NORM;   /* two cascaded biquads */
            break;
        }
        case FGRAPH_FILTER_HPF: {
            float m = hpf2_mag(r, Q);
            mag = m * FG_PASSBAND_NORM;
            break;
        }
        case FGRAPH_FILTER_BPF: {
            float m = bpf2_mag(r, Q);
            /* BPF unity gain at center = 1.0; scale to passband level. */
            mag = m * FG_PASSBAND_NORM;
            break;
        }
        default:   /* FGRAPH_FILTER_NONE */
            mag = FG_PASSBAND_NORM;
            break;
    }
    return mag > 1.0f ? 1.0f : (mag < 0.0f ? 0.0f : mag);
}

/* Convert normalised cutoff 0..1 → Hz. */
static float norm_to_hz(float norm)
{
    return FGRAPH_CUTOFF_HZ_MIN *
           powf(FGRAPH_CUTOFF_HZ_MAX / FGRAPH_CUTOFF_HZ_MIN, norm);
}

/* Convert normalised resonance 0..1 → Q. */
static float norm_to_q(float norm)
{
    return FGRAPH_RES_MIN + norm * (FGRAPH_RES_MAX - FGRAPH_RES_MIN);
}

void filter_graph_draw(u8g2_t *u8g2, const filter_graph_t *fg)
{
    if (!u8g2 || !fg) return;

    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);

    /* ── Top bar ── */
    const char *type_name = (fg->filter_type < FGRAPH_FILTER_COUNT)
                            ? s_type_names[fg->filter_type] : "?";

    /* Left: voice label (e.g. "L1 T2" or "ARP"). */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2, 2, 8, fg->label);

    /* Right: value readout for the selected cursor parameter. */
    {
        char val_buf[12];
        float hz = norm_to_hz(fg->cutoff_norm);
        float q  = norm_to_q(fg->resonance_norm);
        if (fg->cursor == 1) {
            snprintf(val_buf, sizeof(val_buf), "Q:%.1f", (double)q);
        } else {
            if (hz >= 1000.0f) {
                snprintf(val_buf, sizeof(val_buf), "%.1fk", (double)(hz / 1000.0f));
            } else {
                snprintf(val_buf, sizeof(val_buf), "%uHz", (unsigned)hz);
            }
        }
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        uint8_t vw = (uint8_t)u8g2_GetStrWidth(u8g2, val_buf);
        uint8_t vx = (uint8_t)(126 - vw);
        /* Frame the readout when the matching cursor is editing. */
        if (fg->editing && (fg->cursor == 0 || fg->cursor == 1)) {
            u8g2_DrawRFrame(u8g2, (uint8_t)(vx - 2), 0, (uint8_t)(vw + 4), 11, 1);
        }
        u8g2_DrawStr(u8g2, vx, 8, val_buf);
    }

    /* Centre: filter type name (cursor=2) or EN toggle (cursor=3). */
    {
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        if (fg->cursor == 3) {
            /* EN cursor: show enable state centred in the top bar. */
            const char *en_str = fg->enabled ? "EN:ON" : "EN:OFF";
            uint8_t ew = (uint8_t)u8g2_GetStrWidth(u8g2, en_str);
            uint8_t ex = (uint8_t)((128 - ew) / 2);
            if (fg->editing) {
                u8g2_DrawBox(u8g2, (uint8_t)(ex - 2), 0, (uint8_t)(ew + 4), 11);
                u8g2_SetDrawColor(u8g2, 0);
                u8g2_DrawStr(u8g2, ex, 8, en_str);
                u8g2_SetDrawColor(u8g2, 1);
            } else {
                u8g2_DrawRFrame(u8g2, (uint8_t)(ex - 2), 0, (uint8_t)(ew + 4), 11, 1);
                u8g2_DrawStr(u8g2, ex, 8, en_str);
            }
        } else {
            uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, type_name);
            uint8_t tx = (uint8_t)((128 - tw) / 2);
            if (fg->cursor == 2) {
                if (fg->editing) {
                    /* Inverted box = value is live. */
                    u8g2_DrawBox(u8g2, (uint8_t)(tx - 2), 0, (uint8_t)(tw + 4), 11);
                    u8g2_SetDrawColor(u8g2, 0);
                    u8g2_DrawStr(u8g2, tx, 8, type_name);
                    u8g2_SetDrawColor(u8g2, 1);
                } else {
                    u8g2_DrawRFrame(u8g2, (uint8_t)(tx - 2), 0, (uint8_t)(tw + 4), 11, 1);
                    u8g2_DrawStr(u8g2, tx, 8, type_name);
                }
            } else {
                u8g2_DrawStr(u8g2, tx, 8, type_name);
            }
        }
    }

    /* Divider. */
    u8g2_DrawHLine(u8g2, 0, FG_TOPBAR_H - 1, 128);

    /* ── Disabled state: flat line + "OFF" ── */
    if (!fg->enabled) {
        uint8_t y_mid = (uint8_t)(FG_PLOT_Y0 + FG_PLOT_H / 2);
        u8g2_DrawHLine(u8g2, 0, y_mid, FG_PLOT_W);
        u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
        uint8_t ow = (uint8_t)u8g2_GetStrWidth(u8g2, "OFF");
        u8g2_DrawStr(u8g2, (uint8_t)((FG_PLOT_W - ow) / 2), (uint8_t)(y_mid - 2), "OFF");
        u8g2_SendBuffer(u8g2);
        return;
    }

    /* ── Compute response curve ── */
    float fc = norm_to_hz(fg->cutoff_norm);
    float Q  = norm_to_q(fg->resonance_norm);
    const uint8_t baseline_y = (uint8_t)(FG_PLOT_Y0 + FG_PLOT_H - 1);

    /* py[i]: Y pixel for each log-spaced frequency sample. */
    uint8_t px[FG_NPTS];
    uint8_t py[FG_NPTS];
    for (int i = 0; i < FG_NPTS; i++) {
        float t = (float)i / (float)(FG_NPTS - 1);
        float f = FGRAPH_CUTOFF_HZ_MIN *
                  powf(FGRAPH_CUTOFF_HZ_MAX / FGRAPH_CUTOFF_HZ_MIN, t);
        float mag = filter_display_mag(fg->filter_type, f, fc, Q);
        px[i] = (uint8_t)(t * (float)(FG_PLOT_W - 1));
        /* Y: top of plot = high magnitude; bottom = silence. */
        uint8_t amp_px = (uint8_t)(mag * (float)(FG_PLOT_H - 2) + 0.5f);
        py[i] = (uint8_t)(FG_PLOT_Y0 + (FG_PLOT_H - 2) - amp_px);
    }

    /* Fill area under the curve (vertical lines from curve to baseline). */
    int seg = 0;
    for (uint8_t x = 0; x < FG_PLOT_W; x++) {
        while (seg < FG_NPTS - 2 && px[seg + 1] <= x) seg++;
        int y;
        int dX = (int)px[seg + 1] - (int)px[seg];
        if (dX < 1) {
            y = py[seg];
        } else {
            y = (int)py[seg] + ((int)py[seg + 1] - (int)py[seg]) * (int)(x - px[seg]) / dX;
        }
        if (y < FG_PLOT_Y0)    y = FG_PLOT_Y0;
        if (y > (int)baseline_y) y = baseline_y;
        u8g2_DrawVLine(u8g2, x, (uint8_t)y, (uint8_t)(baseline_y - y + 1));
    }

    /* ── Cutoff cursor: XOR the column at cutoff_norm position ── */
    uint8_t cx = (uint8_t)(fg->cutoff_norm * (float)(FG_PLOT_W - 1));
    u8g2_SetDrawColor(u8g2, 2);   /* XOR: inverts whatever is there */
    u8g2_DrawVLine(u8g2, cx, FG_PLOT_Y0, FG_PLOT_H - 1);
    u8g2_SetDrawColor(u8g2, 1);

    u8g2_SendBuffer(u8g2);
}

#include "display_lfo.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Column x-positions and width for the 5 parameter fields (128px total).
 * MODE was removed — RETRIG is not yet implemented, so showing a dead toggle
 * would mislead users. */
static const uint8_t COL_X[5] = { 2, 28, 54, 80, 106 };
static const uint8_t COL_W    = 24;

/* Precomputed y-offsets (from icon center) for 16-pixel-wide wave icons.
 * Positive = below center on screen (screen y increases downward). */
static const int8_t s_wave_icon[LFO_WAVE_COUNT][16] = {
    /* SINE */
    { 0,-3,-5,-6,-7,-6,-5,-3, 0, 3, 5, 6, 7, 6, 5, 3},
    /* TRIANGLE */
    { 7, 5, 4, 2, 0,-2,-4,-6,-7,-6,-4,-2, 0, 2, 4, 6},
    /* SAW_UP  */
    { 7, 6, 5, 4, 3, 2, 1, 0,-1,-2,-3,-4,-5,-6,-7,-7},
    /* SAW_DOWN */
    {-7,-6,-5,-4,-3,-2,-1, 0, 1, 2, 3, 4, 5, 6, 7, 7},
    /* SQUARE */
    {-7,-7,-7,-7,-7,-7,-7,-7, 7, 7, 7, 7, 7, 7, 7, 7},
    /* RANDOM */
    { 4, 4, 4, 4,-5,-5,-5,-5, 2, 2, 2, 2,-4,-4,-4,-4},
};

static const char *target_label(lfo_target_t t)
{
    switch (t) {
        case LFO_TARGET_FILTER: return "FLT";
        case LFO_TARGET_AMP:    return "AMP";
        case LFO_TARGET_PITCH:  return "PCH";
        case LFO_TARGET_PAN:    return "PAN";
        case LFO_TARGET_SCAN:   return "SCN";
        default:                return "???";
    }
}

static const char *wave_label(lfo_wave_t w)
{
    switch (w) {
        case LFO_WAVE_SINE:     return "SIN";
        case LFO_WAVE_TRIANGLE: return "TRI";
        case LFO_WAVE_SAW_UP:   return "S+ ";
        case LFO_WAVE_SAW_DOWN: return "S- ";
        case LFO_WAVE_SQUARE:   return "SQR";
        case LFO_WAVE_RANDOM:   return "RND";
        default:                return "???";
    }
}

static const char *rate_label(lfo_rate_t r)
{
    switch (r) {
        case LFO_RATE_1_8:  return "1/8";
        case LFO_RATE_1_4:  return "1/4";
        case LFO_RATE_1_2:  return "1/2";
        case LFO_RATE_1BAR: return "1BR";
        case LFO_RATE_2BAR: return "2BR";
        case LFO_RATE_4BAR: return "4BR";
        default:            return "???";
    }
}

static void draw_wave_icon(u8g2_t *u8g2, uint8_t x0, uint8_t y_center,
                           lfo_wave_t wave, bool invert)
{
    if ((uint8_t)wave >= LFO_WAVE_COUNT) return;
    u8g2_SetDrawColor(u8g2, invert ? 0 : 1);
    for (uint8_t i = 0; i < 16; i++) {
        int16_t y = (int16_t)y_center + s_wave_icon[wave][i];
        if (y >= 0 && y < 64)
            u8g2_DrawPixel(u8g2, x0 + i, (uint8_t)y);
    }
    /* For saw waves, draw the reset edge. */
    if (wave == LFO_WAVE_SAW_UP || wave == LFO_WAVE_SAW_DOWN) {
        u8g2_DrawVLine(u8g2, x0 + 15, y_center - 7, 14);
    }
    u8g2_SetDrawColor(u8g2, 1);
}

void lfo_view_draw(u8g2_t *u8g2, const lfo_view_t *v)
{
    const seq_lfo_t *l = &v->lfo;

    /* ── Header bar ── */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    char hdr[20];
    if (v->target_label && v->target_label[0]) {
        snprintf(hdr, sizeof(hdr), "LFO  %s", v->target_label);
    } else {
        snprintf(hdr, sizeof(hdr), "LFO  L%u T%u%s",
                 v->layer_idx + 1u, v->track_idx + 1u, v->apply_all ? ">L" : ">T");
    }
    u8g2_DrawStr(u8g2, 1, 9, hdr);
    u8g2_DrawStr(u8g2, 100, 9, l->enabled ? "ON" : "--");
    u8g2_DrawHLine(u8g2, 0, 15, 128);

    /* ── Parameter column labels ── */
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
    static const char *labels[5] = {"TGT","WAV","RTE","DEP","EN"};
    for (int i = 0; i < 5; i++) {
        u8g2_DrawStr(u8g2, COL_X[i], 22, labels[i]);
    }

    /* ── Parameter values with cursor highlight ── */
    char depth_str[5];
    snprintf(depth_str, sizeof(depth_str), "%3u%%", (unsigned)l->depth);
    const char *vals[5] = {
        target_label(l->target),
        wave_label(l->wave),
        rate_label(l->rate),
        depth_str,
        l->enabled ? "ON" : "--",
    };

    for (int i = 0; i < 5; i++) {
        bool selected = (v->cursor == (uint8_t)i);
        if (selected) {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawBox(u8g2, COL_X[i] - 1, 23, COL_W, 9);
            u8g2_SetDrawColor(u8g2, 0);
        }
        u8g2_DrawStr(u8g2, COL_X[i], 31, vals[i]);
        if (selected) {
            if (v->editing) {
                /* Editing indicator: small filled triangle above selected column. */
                u8g2_SetDrawColor(u8g2, 1);
                u8g2_DrawBox(u8g2, COL_X[i] + COL_W/2 - 1, 23, 3, 2);
            }
            u8g2_SetDrawColor(u8g2, 1);
        }
    }

    u8g2_DrawHLine(u8g2, 0, 34, 128);

    /* ── Waveform icon preview ── */
    draw_wave_icon(u8g2, 4, 50, l->wave, false);

    /* ── Rate / depth summary ── */
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);
    char desc[20];
    snprintf(desc, sizeof(desc), "%s  d:%u%%",
             rate_label(l->rate), (unsigned)l->depth);
    u8g2_DrawStr(u8g2, 26, 56, desc);
}

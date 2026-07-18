#include "display_lfo.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Two-panel layout (128x64): a left checklist of the 5 modulation targets
 * (each an independent checkbox — the one LFO carrier drives every checked
 * one) and a right column with the shared WAVE/RATE/DEPTH/EN parameters.
 * Both panels use the 5x7 font at a tight pitch so every row (incl. the
 * last target and the En row) ends above the hint strip at y=57. */
#define LFO_DIV_X      60      /* vertical divider between the two panels */
#define LFO_ROW0_Y     21      /* first checklist row baseline            */
#define LFO_ROW_DY      8      /* checklist row pitch                      */
#define LFO_RCOL_X     64      /* right-column text x                      */

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

static const char *target_name(int t)
{
    switch (t) {
        case LFO_TARGET_FILTER: return "Filter";
        case LFO_TARGET_AMP:    return "Amp";
        case LFO_TARGET_PITCH:  return "Pitch";
        case LFO_TARGET_PAN:    return "Pan";
        case LFO_TARGET_SCAN:   return "Scan";
        default:                return "?";
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

/* Draw a right-column parameter row ("Label  value"), inverted when selected,
 * with a small caret to the far right while it is being adjusted. */
static void draw_right_row(u8g2_t *u8g2, uint8_t baseline, const char *text,
                           bool selected, bool editing)
{
    if (selected) {
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawBox(u8g2, LFO_DIV_X + 1, baseline - 7, 127 - LFO_DIV_X, 9);
        u8g2_SetDrawColor(u8g2, 0);
    }
    u8g2_DrawStr(u8g2, LFO_RCOL_X, baseline, text);
    if (selected && editing) {
        /* filled caret at the row's right edge = "turn to adjust" */
        u8g2_DrawBox(u8g2, 123, baseline - 6, 3, 6);
    }
    if (selected) u8g2_SetDrawColor(u8g2, 1);
}

void lfo_view_draw(u8g2_t *u8g2, const lfo_view_t *v)
{
    const seq_lfo_t *l = &v->lfo;

    /* ── Header bar ── */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    char hdr[20];
    if (v->target_label && v->target_label[0]) {
        snprintf(hdr, sizeof(hdr), "LFO %s", v->target_label);
    } else {
        snprintf(hdr, sizeof(hdr), "LFO L%u T%u%s",
                 v->layer_idx + 1u, v->track_idx + 1u, v->apply_all ? ">L" : ">T");
    }
    u8g2_DrawStr(u8g2, 1, 10, hdr);
    u8g2_DrawHLine(u8g2, 0, 13, 128);
    u8g2_DrawVLine(u8g2, LFO_DIV_X, 15, 42);

    /* Panels use the compact font so all rows clear the hint strip. */
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);

    /* ── Left panel: target checklist ── */
    for (int t = 0; t < LFO_TARGET_COUNT; t++) {
        uint8_t base = LFO_ROW0_Y + (uint8_t)t * LFO_ROW_DY;
        bool selected = (v->cursor == (uint8_t)t);
        bool on = (l->targets & LFO_TGT_BIT(t)) != 0;

        if (selected) {
            u8g2_SetDrawColor(u8g2, 1);
            u8g2_DrawBox(u8g2, 0, base - 7, LFO_DIV_X, 9);
            u8g2_SetDrawColor(u8g2, 0);
        }
        /* checkbox: filled when the target is active, framed when not */
        if (on) u8g2_DrawBox(u8g2, 2, base - 6, 6, 6);
        else    u8g2_DrawFrame(u8g2, 2, base - 6, 6, 6);
        u8g2_DrawStr(u8g2, 11, base, target_name(t));
        if (selected) u8g2_SetDrawColor(u8g2, 1);
    }

    /* ── Right panel: shared LFO parameters ── */
    char buf[12];

    /* WAVE row: label + live waveform icon */
    bool wav_sel = (v->cursor == LFO_FLD_WAVE);
    if (wav_sel) {
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawBox(u8g2, LFO_DIV_X + 1, 15, 127 - LFO_DIV_X, 12);
        u8g2_SetDrawColor(u8g2, 0);
    }
    u8g2_DrawStr(u8g2, LFO_RCOL_X, 24, "Wav");
    if (wav_sel && v->editing) u8g2_DrawBox(u8g2, 123, 17, 3, 6);  /* color 0 while selected */
    draw_wave_icon(u8g2, 92, 20, l->wave, wav_sel);                /* restores color 1 */
    if (wav_sel) u8g2_SetDrawColor(u8g2, 1);

    snprintf(buf, sizeof(buf), "Rte %s", rate_label(l->rate));
    draw_right_row(u8g2, 36, buf, v->cursor == LFO_FLD_RATE, v->editing);

    snprintf(buf, sizeof(buf), "Dep %u%%", (unsigned)l->depth);
    draw_right_row(u8g2, 45, buf, v->cursor == LFO_FLD_DEPTH, v->editing);

    snprintf(buf, sizeof(buf), "En  %s", l->enabled ? "ON" : "--");
    draw_right_row(u8g2, 54, buf, v->cursor == LFO_FLD_EN, false);
}

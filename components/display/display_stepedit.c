#include "display_stepedit.h"
#include <stdio.h>

/* Yellow header (rows 0..15) carries the STEP title; the up-to-4 fixed fields
 * fill the blue region starting at y=24 so no row crosses the 16px seam. */
#define SE_TITLE_Y   8
#define SE_ROW_H     10
#define SE_FIRST_ROW 24

static const char *se_cond_name(uint8_t cond_type)
{
    switch (cond_type) {
        case 1: return "FILL";
        case 2: return "PREV";
        default: return "---";
    }
}

/* Mirrors display_trackopts.c's to_draw_row: box+invert when selected, plain
 * otherwise. This editor has no separate select/adjust phase (the encoder
 * always adjusts the currently-cursored field), so "selected" alone is
 * enough context for the user. */
static void se_draw_row(u8g2_t *u8g2, uint8_t y, const char *label,
                        const char *value, bool selected)
{
    char buf[26];
    snprintf(buf, sizeof(buf), "%-8s: %s", label, value);
    if (selected) {
        uint8_t w = (uint8_t)(u8g2_GetStrWidth(u8g2, buf) + 4);
        u8g2_DrawBox(u8g2, 0, (uint8_t)(y - 8), w, SE_ROW_H);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, 2, y, buf);
        u8g2_SetDrawColor(u8g2, 1);
    } else {
        u8g2_DrawStr(u8g2, 2, y, buf);
    }
}

void display_stepedit_draw_frame(u8g2_t *u8g2, const stepedit_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);

    if (view == NULL) { return; }

    char title[24];
    snprintf(title, sizeof(title), "STEP L%u T%u S%02u",
             (unsigned)(view->layer_idx + 1), (unsigned)(view->track_idx + 1),
             (unsigned)(view->step_idx + 1));
    u8g2_DrawStr(u8g2, 2, SE_TITLE_Y, title);
    u8g2_DrawHLine(u8g2, 0, 15, 128);

    uint8_t y = SE_FIRST_ROW;
    char val[8];

    snprintf(val, sizeof(val), "%u%%", (unsigned)view->prob);
    se_draw_row(u8g2, y, "Prob", val, view->field_cursor == SE_FIELD_PROB);
    y = (uint8_t)(y + SE_ROW_H);

    snprintf(val, sizeof(val), "%u", (unsigned)view->ratchet);
    se_draw_row(u8g2, y, "Ratchet", val, view->field_cursor == SE_FIELD_RATCHET);
    y = (uint8_t)(y + SE_ROW_H);

    se_draw_row(u8g2, y, "Cond", se_cond_name(view->cond_type),
                view->field_cursor == SE_FIELD_COND);
    y = (uint8_t)(y + SE_ROW_H);

    /* Param only means anything for FILL; still show it (dimmed by dashes via
     * se_cond_name-style "-" would need its own helper — keep it simple and
     * just show the raw value, since the encoder loop below skips this row
     * entirely for any other condition type). */
    if (view->cond_type == 1 /* SEQ_STEP_COND_FILL */) {
        snprintf(val, sizeof(val), "%u", (unsigned)view->cond_param);
        se_draw_row(u8g2, y, " Every", val, view->field_cursor == SE_FIELD_PARAM);
    }
}

#include "display_trackopts.h"
#include <stdio.h>

#define TO_TITLE_Y    8
#define TO_ROW_H      11
#define TO_FIRST_ROW  22

/* Draw one "label : value" row, inverting the value when this row is the
 * selected+editing target, or boxing the whole row when merely selected. */
static void to_draw_row(u8g2_t *u8g2, uint8_t y, const char *label,
                        const char *value, bool selected, bool editing)
{
    char buf[26];
    snprintf(buf, sizeof(buf), "%-11s: %s", label, value);
    if (selected && editing) {
        uint8_t w = (uint8_t)(u8g2_GetStrWidth(u8g2, buf) + 4);
        u8g2_DrawBox(u8g2, 0, (uint8_t)(y - 8), w, TO_ROW_H);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, 2, y, buf);
        u8g2_SetDrawColor(u8g2, 1);
    } else if (selected) {
        u8g2_DrawStr(u8g2, 8, y, buf);
        u8g2_DrawTriangle(u8g2, 0, (int16_t)(y - 6),
                                0, (int16_t)(y - 1),
                                3, (int16_t)(y - 3));
    } else {
        u8g2_DrawStr(u8g2, 8, y, buf);
    }
}

/* Draw a value string in the title bar with inversion when selected. */
static void to_draw_title_val(u8g2_t *u8g2, uint8_t x, const char *s, bool selected)
{
    if (selected) {
        uint8_t w = (uint8_t)(u8g2_GetStrWidth(u8g2, s) + 4);
        u8g2_DrawBox(u8g2, (uint8_t)(x - 2), 0, w, 10);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, x, TO_TITLE_Y, s);
        u8g2_SetDrawColor(u8g2, 1);
    } else {
        u8g2_DrawStr(u8g2, x, TO_TITLE_Y, s);
    }
}

void display_trackopts_draw_frame(u8g2_t *u8g2, const trackopts_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);

    /* Title: "TRACK OPTS  " then Lx and Tx as interactive values. */
    static const char *prefix = "TRACK OPTS  ";
    u8g2_DrawStr(u8g2, 2, TO_TITLE_Y, prefix);
    uint8_t lx = (uint8_t)(2 + u8g2_GetStrWidth(u8g2, prefix));
    char lbuf[6], tbuf[6];
    snprintf(lbuf, sizeof(lbuf), "L%u", (unsigned)((view ? view->layer_idx : 0) + 1));
    snprintf(tbuf, sizeof(tbuf), "T%u", (unsigned)((view ? view->track_idx : 0) + 1));
    to_draw_title_val(u8g2, lx, lbuf, view && view->cursor == TO_ROW_LAYER);
    uint8_t tx = (uint8_t)(lx + u8g2_GetStrWidth(u8g2, lbuf) + 4);
    to_draw_title_val(u8g2, tx, tbuf, view && view->cursor == TO_ROW_TRACK);
    u8g2_DrawHLine(u8g2, 0, 10, 128);

    if (view == NULL) { u8g2_SendBuffer(u8g2); return; }

    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    uint8_t y = TO_FIRST_ROW;
    char val[12];

    /* Repeat Rate (always present). */
    snprintf(val, sizeof(val), "%u", (unsigned)view->repeat_rate);
    to_draw_row(u8g2, y, "Repeat Rate", val,
                view->cursor == TO_ROW_REPEAT, view->editing);
    y = (uint8_t)(y + TO_ROW_H);

    if (view->melodic) {
        /* When the global progression is driving harmony, the chord rows are
         * read-only — show "(prog)" so the user knows why edits don't stick. */
        to_draw_row(u8g2, y, "Chord Mode",
                    view->chord_locked ? "(prog)" : (view->chord_mode ? "ON" : "OFF"),
                    view->cursor == TO_ROW_CHORD, view->editing && !view->chord_locked);
        y = (uint8_t)(y + TO_ROW_H);

        to_draw_row(u8g2, y, " Root", chord_root_name(view->chord_root),
                    view->cursor == TO_ROW_ROOT, view->editing);
        y = (uint8_t)(y + TO_ROW_H);

        to_draw_row(u8g2, y, " Type", chord_type_name(view->chord_type),
                    view->cursor == TO_ROW_TYPE, view->editing);
    } else {
        u8g2_SetFont(u8g2, u8g2_font_5x7_tf);
        u8g2_DrawStr(u8g2, 8, (uint8_t)(y + 4), "(drum track)");
    }

    u8g2_SendBuffer(u8g2);
}

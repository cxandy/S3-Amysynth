#include "display_trackopts.h"
#include <stdio.h>

#define TO_TITLE_Y    8
#define TO_ROW_H      11
#define TO_FIRST_ROW  22
#define TO_VIS_ROWS   4    /* content rows visible at once (rows 22/33/44/55) */

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

    if (view == NULL) { return; }

    /* Content rows, indexed relative to TO_ROW_REPEAT: 0=Repeat 1=Mute 2=Solo
     * 3=Chord 4=Root 5=Type. Drum layers stop at Solo (3 rows); melodic
     * layers have all 6 and need the same scroll-window technique as the
     * drone screen to fit the 4 rows visible below the title bar. */
    uint8_t content_count = view->melodic ? 6 : 3;
    uint8_t content_cursor = (view->cursor >= TO_ROW_REPEAT)
                             ? (uint8_t)(view->cursor - TO_ROW_REPEAT) : 0;
    uint8_t first = 0;
    if (content_cursor >= TO_VIS_ROWS) {
        first = (uint8_t)(content_cursor - (TO_VIS_ROWS - 1));
    }
    uint8_t last = (uint8_t)(first + TO_VIS_ROWS);
    if (last > content_count) last = content_count;

    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    char val[12];

    for (uint8_t i = first; i < last; i++) {
        uint8_t y = (uint8_t)(TO_FIRST_ROW + (i - first) * TO_ROW_H);
        bool selected = (view->cursor == (uint8_t)(TO_ROW_REPEAT + i));

        switch (i) {
        case 0:
            snprintf(val, sizeof(val), "%u", (unsigned)view->repeat_rate);
            to_draw_row(u8g2, y, "Repeat Rate", val, selected, view->editing);
            break;
        case 1:
            to_draw_row(u8g2, y, "Mute", view->track_mute ? "ON" : "OFF",
                        selected, view->editing);
            break;
        case 2:
            to_draw_row(u8g2, y, "Solo", view->track_solo ? "ON" : "OFF",
                        selected, view->editing);
            break;
        case 3:
            /* When the global progression is driving harmony, the chord rows
             * are read-only — show "(prog)" so edits don't silently no-op. */
            to_draw_row(u8g2, y, "Chord Mode",
                        view->chord_locked ? "(prog)" : (view->chord_mode ? "ON" : "OFF"),
                        selected, view->editing && !view->chord_locked);
            break;
        case 4:
            to_draw_row(u8g2, y, " Root", chord_root_name(view->chord_root),
                        selected, view->editing);
            break;
        case 5:
            to_draw_row(u8g2, y, " Type", chord_type_name(view->chord_type),
                        selected, view->editing);
            break;
        default:
            break;
        }
    }

    /* Scroll affordances, same shape as the drone screen. */
    if (first > 0) {
        u8g2_DrawTriangle(u8g2, 124, 14, 120, 18, 128, 18);
    }
    if (last < content_count) {
        u8g2_DrawTriangle(u8g2, 120, 60, 128, 60, 124, 64);
    }
}

#include "display_prog.h"
#include <stdio.h>

/* Yellow header (rows 0..15) holds the title + ON/OFF toggle; entries fill the
 * blue region below the 16px seam. 3 roomy rows (25/37/49); the bottom status
 * line at y=63 is safe because PROG has no bottom hint strip. */
#define PROG_TITLE_Y    8
#define PROG_ROW_H      12
#define PROG_FIRST_ROW  25
#define PROG_VIS_ROWS   3

void display_prog_draw_frame(u8g2_t *u8g2, const prog_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);

    /* Title + enabled state. */
    u8g2_DrawStr(u8g2, 2, PROG_TITLE_Y, "PROG");
    const char *en_str = (view && view->enabled) ? "ON" : "OFF";
    uint8_t en_x = (uint8_t)(128 - u8g2_GetStrWidth(u8g2, en_str) - 2);
    if (view && view->cursor == 0) {
        /* Invert the ON/OFF label when cursor is on the toggle. */
        uint8_t w = (uint8_t)(u8g2_GetStrWidth(u8g2, en_str) + 4);
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawBox(u8g2, (uint8_t)(en_x - 2), 0, w, 10);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawStr(u8g2, en_x, PROG_TITLE_Y, en_str);
        u8g2_SetDrawColor(u8g2, 1);
    } else {
        u8g2_DrawStr(u8g2, en_x, PROG_TITLE_Y, en_str);
    }
    u8g2_DrawHLine(u8g2, 0, 15, 128);

    if (view == NULL || view->count == 0) {
        u8g2_SetFont(u8g2, u8g2_font_5x7_tf);
        u8g2_DrawStr(u8g2, 8, 38, "No entries");
        return;
    }

    /* Scroll so the cursor entry is visible. cursor 0 = toggle (title bar),
     * cursor 1..count = entry 0..count-1. */
    uint8_t entry_cursor = (view->cursor > 0) ? (uint8_t)(view->cursor - 1) : 0;
    uint8_t first = 0;
    if (entry_cursor >= PROG_VIS_ROWS) {
        first = (uint8_t)(entry_cursor - (PROG_VIS_ROWS - 1));
    }

    for (uint8_t i = 0; i < PROG_VIS_ROWS && (first + i) < view->count; i++) {
        uint8_t ei  = (uint8_t)(first + i);
        uint8_t y   = (uint8_t)(PROG_FIRST_ROW + i * PROG_ROW_H);
        bool is_playing  = (ei == view->current_entry && view->enabled);
        bool is_selected = (view->cursor == (uint8_t)(ei + 1));

        /* When this row is selected AND being edited, bracket the focused field
         * (root / type / duration) so the user sees what the encoder will change. */
        bool editing_here = is_selected && view->editing;
        const char *r_l = (editing_here && view->edit_field == 0) ? "<" : " ";
        const char *r_r = (editing_here && view->edit_field == 0) ? ">" : " ";
        const char *t_l = (editing_here && view->edit_field == 1) ? "<" : " ";
        const char *t_r = (editing_here && view->edit_field == 1) ? ">" : " ";
        const char *d_l = (editing_here && view->edit_field == 2) ? "<" : " ";
        const char *d_r = (editing_here && view->edit_field == 2) ? ">" : "b";

        char buf[32];
        snprintf(buf, sizeof(buf), "%u.%s%s%s%s%s%s%s%u%s",
                 (unsigned)(ei + 1),
                 r_l, chord_root_name(view->entries[ei].root), r_r,
                 t_l, chord_type_name(view->entries[ei].chord_type), t_r,
                 d_l, (unsigned)view->entries[ei].duration_bars, d_r);

        if (is_selected) {
            uint8_t w = (uint8_t)(u8g2_GetStrWidth(u8g2, buf) + 4);
            u8g2_DrawBox(u8g2, 0, (uint8_t)(y - 8), w, PROG_ROW_H);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, 2, y, buf);
            u8g2_SetDrawColor(u8g2, 1);
        } else if (is_playing) {
            /* Active entry: draw a small triangle marker. */
            u8g2_DrawStr(u8g2, 8, y, buf);
            u8g2_DrawTriangle(u8g2, 0, (int16_t)(y - 6),
                                    0, (int16_t)(y - 1),
                                    3, (int16_t)(y - 3));
        } else {
            u8g2_DrawStr(u8g2, 8, y, buf);
        }
    }

    /* Status bar: bar position within current entry (left) + button hints (right). */
    u8g2_SetFont(u8g2, u8g2_font_5x7_tf);
    if (view->enabled && view->count > 0) {
        uint8_t dur = view->entries[view->current_entry].duration_bars;
        char status[16];
        snprintf(status, sizeof(status), "bar %u/%u",
                 (unsigned)(view->bars_in_current + 1), (unsigned)dur);
        u8g2_DrawStr(u8g2, 2, 63, status);
    }
    /* Right-aligned soft-button legend: B2=+entry, B1=delete. */
    const char *hint = "+:B2 -:B1";
    uint8_t hint_x = (uint8_t)(128 - u8g2_GetStrWidth(u8g2, hint) - 2);
    u8g2_DrawStr(u8g2, hint_x, 63, hint);
}

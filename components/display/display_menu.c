#include "display_menu.h"
#include <string.h>

/* Layout: a title bar then up to 5 visible rows, scrolling to keep the cursor
 * in view. 128x64 OLED. */
#define MENU_TITLE_Y    8
#define MENU_ROW_H      11
#define MENU_FIRST_ROW  16
#define MENU_VIS_ROWS   4

void display_menu_draw_frame(u8g2_t *u8g2, const menu_view_t *view)
{
    display_menu_draw_frame_titled(u8g2, "MENU", view);
}

void display_menu_draw_frame_titled(u8g2_t *u8g2, const char *title,
                                    const menu_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);

    /* Title bar. */
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    u8g2_DrawStr(u8g2, 2, MENU_TITLE_Y, title ? title : "MENU");
    u8g2_DrawHLine(u8g2, 0, 10, 128);

    if (view == NULL || view->count == 0) {
        return;
    }

    /* Compute the scroll window so the cursor is always visible. */
    uint8_t first = 0;
    if (view->cursor >= MENU_VIS_ROWS) {
        first = (uint8_t)(view->cursor - (MENU_VIS_ROWS - 1));
    }
    uint8_t last = (uint8_t)(first + MENU_VIS_ROWS);
    if (last > view->count) last = view->count;

    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
    for (uint8_t i = first; i < last; i++) {
        const menu_item_view_t *it = &view->items[i];
        uint8_t row = (uint8_t)(i - first);
        uint8_t y   = (uint8_t)(MENU_FIRST_ROW + row * MENU_ROW_H);
        bool selected = (i == view->cursor);

        if (selected) {
            /* Highlight the whole row (filled bar, inverted text). */
            u8g2_DrawBox(u8g2, 0, (uint8_t)(y - 9), 128, MENU_ROW_H);
            u8g2_SetDrawColor(u8g2, 0);
        }

        u8g2_DrawStr(u8g2, 2, y, it->label);

        if (it->value[0] != '\0') {
            uint8_t vw = (uint8_t)u8g2_GetStrWidth(u8g2, it->value);
            uint8_t vx = (vw < 124) ? (uint8_t)(126 - vw) : 2;
            /* When editing the selected item, frame the value to signal "live".
             * Note: while the row is highlighted the draw color is 0 (black),
             * so the frame reads as a cut-out box around the value. */
            if (selected && view->editing) {
                u8g2_DrawFrame(u8g2, (uint8_t)(vx - 2), (uint8_t)(y - 9),
                               (uint8_t)(vw + 4), MENU_ROW_H);
            }
            u8g2_DrawStr(u8g2, vx, y, it->value);
        }

        if (selected) {
            u8g2_SetDrawColor(u8g2, 1);  /* restore */
        }
    }

    /* Scroll affordances: little arrows when there is more above/below. */
    if (first > 0) {
        u8g2_DrawTriangle(u8g2, 124, 14, 120, 18, 128, 18);
    }
    if (last < view->count) {
        u8g2_DrawTriangle(u8g2, 120, 60, 128, 60, 124, 64);
    }
}

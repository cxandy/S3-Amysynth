#include "display_drone.h"

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

#include "display_song.h"
#include <stdio.h>

/* Same geometry as the progression screen: yellow title bar (rows 0..15),
 * blue body rows on the 12px grid from the 16px seam down, status line at
 * y=63 (SONG has no bottom hint strip). */
#define SONG_TITLE_Y    8
#define SONG_ROW_H      12
#define SONG_FIRST_ROW  25
#define SONG_VIS_ROWS   3

/* Render a scene's layer mask as up to 4 bits ('1' = audible, '.' = muted)
 * into buf, bracketing with '<' '>' the bit being edited (edit_field 1..4 =
 * bit3..bit0, leftmost char = bit3). Returns the string length. */
static int song_draw_mask(char *buf, uint8_t mask, bool editing_here, uint8_t edit_field)
{
    int pos = 0;
    for (int bit = 3; bit >= 0; bit--) {
        bool focused = editing_here && edit_field >= 1 && edit_field <= 4 &&
                       (uint8_t)(4 - bit) == edit_field;
        if (focused) buf[pos++] = '<';
        buf[pos++] = (mask & (1u << (uint8_t)bit)) ? '1' : '.';
        if (focused) buf[pos++] = '>';
    }
    buf[pos] = '\0';
    return pos;
}

void display_song_draw_frame(u8g2_t *u8g2, const song_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);
    u8g2_SetFont(u8g2, u8g2_font_6x10_tf);

    /* Header row: SONG title, then the loop + enabled toggles right-aligned. */
    u8g2_DrawStr(u8g2, 2, SONG_TITLE_Y, "SONG");
    if (view) {
        /* Enabled toggle, inverted when cursor == 0. */
        const char *en_str = view->enabled ? "ON" : "OFF";
        uint8_t en_x = (uint8_t)(128 - u8g2_GetStrWidth(u8g2, en_str) - 2);
        if (view->cursor == 0) {
            uint8_t w = (uint8_t)(u8g2_GetStrWidth(u8g2, en_str) + 4);
            u8g2_DrawBox(u8g2, (uint8_t)(en_x - 2), 0, w, 10);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, en_x, SONG_TITLE_Y, en_str);
            u8g2_SetDrawColor(u8g2, 1);
        } else {
            u8g2_DrawStr(u8g2, en_x, SONG_TITLE_Y, en_str);
        }
        /* Loop toggle (cursor == 1), left of ON/OFF: "LOOP" or "ONE". */
        const char *lp_str = view->loop ? "LOOP" : "ONE";
        uint8_t lp_x = (uint8_t)(en_x - u8g2_GetStrWidth(u8g2, lp_str) - 8);
        if (view->cursor == 1) {
            uint8_t w = (uint8_t)(u8g2_GetStrWidth(u8g2, lp_str) + 4);
            u8g2_DrawBox(u8g2, (uint8_t)(lp_x - 2), 0, w, 10);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, lp_x, SONG_TITLE_Y, lp_str);
            u8g2_SetDrawColor(u8g2, 1);
        } else {
            u8g2_DrawStr(u8g2, lp_x, SONG_TITLE_Y, lp_str);
        }
    }
    u8g2_DrawHLine(u8g2, 0, 15, 128);

    if (view == NULL || view->count == 0) {
        u8g2_SetFont(u8g2, u8g2_font_5x7_tf);
        u8g2_DrawStr(u8g2, 8, 38, "No scenes");
        return;
    }

    /* Scroll so the cursor row is visible. Cursor 2..count+1 = scene 0..count-1. */
    uint8_t scene_cursor = (view->cursor > 1) ? (uint8_t)(view->cursor - 2) : 0;
    uint8_t first = 0;
    if (scene_cursor >= SONG_VIS_ROWS) {
        first = (uint8_t)(scene_cursor - (SONG_VIS_ROWS - 1));
    }

    for (uint8_t i = 0; i < SONG_VIS_ROWS && (first + i) < view->count; i++) {
        uint8_t si  = (uint8_t)(first + i);
        uint8_t y   = (uint8_t)(SONG_FIRST_ROW + i * SONG_ROW_H);
        bool is_playing  = (si == view->current_scene && view->enabled);
        bool is_selected = (view->cursor == (uint8_t)(si + 2));

        /* While editing, bracket the focused field: bars, then one char per
         * layer bit (field 1..4 = bit3..bit0 toggles). */
        bool editing_here = is_selected && view->editing;
        const char *b_l = (editing_here && view->edit_field == 0) ? "<" : " ";
        const char *b_r = (editing_here && view->edit_field == 0) ? ">" : " ";
        char mask[16];
        song_draw_mask(mask, view->scenes[si].layer_mask, editing_here,
                       view->edit_field);

        char buf[32];
        snprintf(buf, sizeof(buf), "%u.%s%ub%s %s",
                 (unsigned)(si + 1),
                 b_l, (unsigned)view->scenes[si].bars, b_r, mask);

        if (is_selected) {
            uint8_t w = (uint8_t)(u8g2_GetStrWidth(u8g2, buf) + 4);
            u8g2_DrawBox(u8g2, 0, (uint8_t)(y - 8), w, SONG_ROW_H);
            u8g2_SetDrawColor(u8g2, 0);
            u8g2_DrawStr(u8g2, 2, y, buf);
            u8g2_SetDrawColor(u8g2, 1);
        } else if (is_playing) {
            u8g2_DrawStr(u8g2, 8, y, buf);
            u8g2_DrawTriangle(u8g2, 0, (int16_t)(y - 6),
                                    0, (int16_t)(y - 1),
                                    3, (int16_t)(y - 3));
        } else {
            u8g2_DrawStr(u8g2, 8, y, buf);
        }
    }

    /* Status bar: bar position within the current scene (left). */
    u8g2_SetFont(u8g2, u8g2_font_5x7_tf);
    if (view->enabled && view->count > 0) {
        uint8_t dur = view->scenes[view->current_scene].bars;
        char status[16];
        snprintf(status, sizeof(status), "bar %u/%u",
                 (unsigned)(view->bars_in_current + 1), (unsigned)dur);
        u8g2_DrawStr(u8g2, 2, 63, status);
    }
    /* Right-aligned soft-button legend: B2=+scene, B1=delete. */
    const char *hint = "+:B2 -:B1";
    uint8_t hint_x = (uint8_t)(128 - u8g2_GetStrWidth(u8g2, hint) - 2);
    u8g2_DrawStr(u8g2, hint_x, 63, hint);
}
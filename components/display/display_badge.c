#include "display_badge.h"

/* Badge geometry. The 5px glyph plus a 1px plate surround owns BADGE_W columns
 * of the top tile row; BADGE_GAP keeps a blank column each side so the badge
 * never sits flush against header text. */
#define BADGE_W    7
#define BADGE_H    8
#define BADGE_GAP  1

/* A 5x8 Bluetooth rune. */
static const unsigned char s_bt_rune[] = {
    0x04, 0x0C, 0x15, 0x0E, 0x0E, 0x15, 0x0C, 0x04
};

/* True when the buffer's byte layout is directly readable: one byte per column
 * of the top tile row, bit n = pixel row n. Page-mode buffers hold only a slice
 * and non-R0 rotations permute the mapping, so the occupancy probe below would
 * read the wrong pixels - draw nothing rather than place from a bad answer. */
static bool buffer_is_probeable(u8g2_t *u8g2)
{
    return u8g2->cb == U8G2_R0 &&
           u8g2->tile_buf_height == u8g2->u8x8.display_info->tile_height &&
           u8g2_GetBufferPtr(u8g2) != NULL;
}

/* Is every pixel of the badge footprint at x (plus its blank gap) still dark?
 * The badge spans rows 0..7, which is exactly tile row 0, so a column is clear
 * precisely when its first buffer byte is zero. */
static bool slot_is_clear(u8g2_t *u8g2, int x)
{
    const int width = (int)u8g2_GetDisplayWidth(u8g2);
    if (x < 0 || x + BADGE_W > width) {
        return false;
    }

    const uint8_t *row0 = u8g2_GetBufferPtr(u8g2);
    int lo = x - BADGE_GAP;
    int hi = x + BADGE_W + BADGE_GAP;
    if (lo < 0) {
        lo = 0;
    }
    if (hi > width) {
        hi = width;
    }

    for (int col = lo; col < hi; col++) {
        if (row0[col] != 0) {
            return false;
        }
    }
    return true;
}

/* Nearest clear slot to the view's preferred x, or -1 when the top row is full.
 * Searching outward keeps the badge in its hand-tuned home when free, and moves
 * it the shortest distance - not to a fixed fallback corner - when not. */
static int find_slot(u8g2_t *u8g2, uint8_t preferred_x)
{
    const int width = (int)u8g2_GetDisplayWidth(u8g2);
    const int anchor = preferred_x;

    for (int delta = 0; delta < width; delta++) {
        if (slot_is_clear(u8g2, anchor - delta)) {
            return anchor - delta;
        }
        if (delta != 0 && slot_is_clear(u8g2, anchor + delta)) {
            return anchor + delta;
        }
    }
    return -1;
}

bool display_badge_draw(u8g2_t *u8g2, uint8_t preferred_x, bool connected)
{
    if (u8g2 == NULL || !buffer_is_probeable(u8g2)) {
        return false;
    }

    const int x = find_slot(u8g2, preferred_x);
    if (x < 0) {
        return false;   /* top row is full -- header content wins, badge yields */
    }

    if (connected) {
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawBox(u8g2, (u8g2_uint_t)x, 0, BADGE_W, BADGE_H);
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawXBM(u8g2, (u8g2_uint_t)(x + 1), 0, 5, BADGE_H, s_bt_rune);
        u8g2_SetDrawColor(u8g2, 1);
    } else {
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawXBM(u8g2, (u8g2_uint_t)(x + 1), 0, 5, BADGE_H, s_bt_rune);
    }
    return true;
}

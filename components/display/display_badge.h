#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Top-row status badge (BLE session indicator) ─────────────────────────
 * Pure rendering only, like every other display_*.c file: the caller supplies
 * the link state, this file owns the glyph and -- crucially -- where it lands.
 *
 * The badge is the one screen element with no layout of its own: it is
 * composited on top of whatever view already filled the buffer, so a fixed x
 * can only ever be right by hand-audit, and goes wrong the moment a header
 * string grows. Instead of trusting a constant, the placement here reads the
 * frame buffer back and refuses to draw over lit pixels: the badge takes the
 * caller's preferred slot when it is genuinely empty, slides to the nearest
 * empty slot when it is not, and stays off screen entirely when the top row
 * has no room at all. Header text always wins.
 *
 * Requires u8g2 in full-buffer mode (u8g2_Setup_*_f_*) at U8G2_R0, which is
 * how priv_i2c_u8g2.c sets the panel up; display_badge_draw() verifies both
 * at run time and degrades to drawing nothing rather than guessing. */

/* Draws the BLE badge and returns true if it was drawn. preferred_x is the
 * view's ideal left edge -- a hint, not a promise; pass 0 for "no preference,
 * put it wherever it fits". connected selects the inverted plate (a central is
 * attached) over the bare glyph (advertising only). */
bool display_badge_draw(u8g2_t *u8g2, uint8_t preferred_x, bool connected);

#ifdef __cplusplus
}
#endif

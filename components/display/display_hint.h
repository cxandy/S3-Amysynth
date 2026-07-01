#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Persistent button-hint strip ─────────────────────────────────────────
 * A one-line, bottom-of-screen legend shared by every synth_ui view. Pure
 * rendering only (like every other display_*.c file, this has no dependency
 * on synth_core/AMY): the caller supplies the already-formatted text.
 *
 * Reserves the bottom 7 rows (y=57..63 of the 128x64 panel) and always erases
 * that strip first, so it is safe to call after any other display_*_draw_frame
 * has already filled the buffer -- this file owns that strip exclusively. */

void display_hint_draw(u8g2_t *u8g2, const char *text);

#ifdef __cplusplus
}
#endif

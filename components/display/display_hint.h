#pragma once

#include "u8g2.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Persistent button-hint strip ─────────────────────────────────────────
 * One-line, bottom-of-screen legend shared by every synth_ui view. Pure
 * rendering, no synth_core/AMY dependency: the caller supplies the text.
 *
 * Owns the bottom 7 rows (y=57..63) exclusively and erases them first, so it
 * is safe to call after any other display_*_draw_frame filled the buffer. */

void display_hint_draw(u8g2_t *u8g2, const char *text);

#ifdef __cplusplus
}
#endif

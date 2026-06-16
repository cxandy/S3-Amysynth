#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Generic menu overlay renderer ───────────────────────────────────────
 * The menu model (item list, current values) lives in sequencer_ui.c. To keep
 * the display component decoupled from sequencer logic, the caller formats each
 * row into a label + value string and hands the renderer a flat array. The
 * renderer draws a scrollable highlighted list; the entered item's value is
 * framed/inverted while editing. */

#define MENU_MAX_ITEMS    8
#define MENU_LABEL_LEN    18
#define MENU_VALUE_LEN    14

typedef struct {
    char label[MENU_LABEL_LEN];
    char value[MENU_VALUE_LEN];   /* empty string => action item (no value) */
} menu_item_view_t;

typedef struct {
    const menu_item_view_t *items;
    uint8_t count;
    uint8_t cursor;     /* highlighted item                     */
    bool    editing;    /* true => value of cursor item is being edited */
} menu_view_t;

/* Draw the full menu overlay (clears + sends the buffer). */
void priv_u8g2_menu_draw_frame(u8g2_t *u8g2, const menu_view_t *view);

#ifdef __cplusplus
}
#endif

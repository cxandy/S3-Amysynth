#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Drone screen renderer (simple scrollable param list) ─────────────────
 * Like the menu, the drone logic lives in the synth_core component
 * (drone_core). synth_ui formats each parameter into a label + value
 * string and hands the renderer a flat array; the renderer draws a scrollable
 * highlighted list with the selected row's value framed while editing. */

#define DRONE_VIEW_MAX_ROWS  16
#define DRONE_LABEL_LEN      12
#define DRONE_VALUE_LEN      12

typedef struct {
    char label[DRONE_LABEL_LEN];
    char value[DRONE_VALUE_LEN];
} drone_row_view_t;

typedef struct {
    const drone_row_view_t *rows;
    uint8_t count;
    uint8_t cursor;     /* highlighted row                       */
    bool    editing;    /* true => value of cursor row is editing */
} drone_view_t;

/* Draw the full drone screen (clears + sends the buffer). */
void display_drone_draw_frame(u8g2_t *u8g2, const drone_view_t *view);

#ifdef __cplusplus
}
#endif

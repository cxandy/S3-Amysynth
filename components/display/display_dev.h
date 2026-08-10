#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── DEV screen renderer (CONFIG_SYNTH_DEV_MENU) ──────────────────────────
 * Fully generic scrolling "label : value" list: the screen side prebuilds
 * every row as strings, so adding a DEV control never touches the renderer.
 * Value conventions: ">" = submenu, "" = plain action/back row. */

#define DEV_VIEW_MAX_ROWS 10
#define DEV_LABEL_LEN     14
#define DEV_VALUE_LEN     12

typedef struct {
    char label[DEV_LABEL_LEN];
    char value[DEV_VALUE_LEN];
} dev_row_view_t;

typedef struct {
    const char    *title;   /* page title, shown as "DEV <title>" */
    dev_row_view_t rows[DEV_VIEW_MAX_ROWS];
    uint8_t        count;
    uint8_t        cursor;
    bool           editing;
} dev_view_t;

void display_dev_draw_frame(u8g2_t *u8g2, const dev_view_t *view);

#ifdef __cplusplus
}
#endif

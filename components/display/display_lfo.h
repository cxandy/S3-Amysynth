#pragma once
#include "u8g2.h"
#include "display_seq.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* View state for the LFO editor overlay.  Holds the working copy being
 * edited plus display metadata (cursor, editing flag, target track). */
typedef struct {
    seq_lfo_t   lfo;
    uint8_t     cursor;       /* 0=target 1=wave 2=rate 3=depth 4=enabled  */
    bool        editing;
    uint8_t     layer_idx;
    uint8_t     track_idx;
    bool        apply_all;    /* true = commit applies to all tracks        */
    const char *target_label; /* "ARP"/"DRONE" label override, or NULL      */
} lfo_view_t;

void lfo_view_draw(u8g2_t *u8g2, const lfo_view_t *v);

#ifdef __cplusplus
}
#endif

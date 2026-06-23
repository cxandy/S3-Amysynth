#pragma once

#include "u8g2.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Arp screen renderer ──────────────────────────────────────────────────
 * Like the menu, the arp logic lives in the synth_core component (arp_core).
 * synth_ui populates this flat view each frame; the renderer just draws it.
 *
 * Layout (matches the user's sketch):
 *   ARP: ON  | MODE: UP   | OCT: 2     <- macro row 1
 *   RATE: 1/16  | GATE: 75%            <- macro row 2
 *   [C3]  E3   G3   B3   --   --  ...  <- 8 note slots
 *
 * The cursor walks the macro fields then the 8 slots. cursor index space:
 *   0=ARP enable, 1=MODE, 2=OCT, 3=RATE, 4=GATE, 5..12 = slots 0..7. */

#define ARP_VIEW_SLOTS 8

#define ARP_CUR_ENABLE 0
#define ARP_CUR_MODE   1
#define ARP_CUR_OCT    2
#define ARP_CUR_RATE   3
#define ARP_CUR_GATE   4
#define ARP_CUR_SLOT0  5
#define ARP_CUR_COUNT  (ARP_CUR_SLOT0 + ARP_VIEW_SLOTS)

typedef struct {
    bool        enabled;
    const char *mode_str;     /* "UP" / "DOWN"        */
    uint8_t     octaves;
    const char *rate_str;     /* "1/16" etc           */
    uint8_t     gate_pct;
    /* Per-slot display: note name (snapped), "--" (empty), or " R " (rest).
     * slot_active: true = note present. slot_rest: true = ARP_REST sentinel.
     * Check slot_rest before slot_active to disambiguate the inactive case. */
    char        slot_name[ARP_VIEW_SLOTS][4];
    bool        slot_active[ARP_VIEW_SLOTS];
    bool        slot_rest[ARP_VIEW_SLOTS];
    uint8_t     cursor;       /* 0..ARP_CUR_COUNT-1   */
    bool        editing;      /* value being adjusted */
    /* Patch indicator (mirrors the sequencer view). The number is always shown
     * top-right; while the patch-select button is held, patch_name (if non-NULL)
     * is drawn as a centred banner so the full-range browser is legible. */
    uint16_t    patch;
    bool        patch_select; /* true while the patch hold+turn gesture is active */
    const char *patch_name;   /* human-readable name or NULL (table excluded)     */
} arp_view_t;

void display_arp_draw_frame(u8g2_t *u8g2, const arp_view_t *view);

#ifdef __cplusplus
}
#endif

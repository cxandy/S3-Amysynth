#include "display_hint.h"

/* Bottom strip geometry. GRAPH_TOPBAR_H-style ADSR editors already leave a
 * blank 5-6px margin below their bottom axis tick marks (graph_popup.c's
 * ADSR bot_pad reserves 7px, of which only the first 2 hold tick marks); the
 * grid-heavy screens (sequencer/arp/drone-vis) do not, so this strip
 * deliberately overwrites their last couple of content rows rather than
 * shrinking their layouts -- see docs/handoff/c1-hint-bar.md. */
#define HINT_Y0       57
#define HINT_H         7
#define HINT_BASELINE 63

void display_hint_draw(u8g2_t *u8g2, const char *text)
{
    if (u8g2 == NULL) {
        return;
    }

    u8g2_SetDrawColor(u8g2, 0);
    u8g2_DrawBox(u8g2, 0, HINT_Y0, 128, HINT_H);
    u8g2_SetDrawColor(u8g2, 1);

    if (text != NULL && text[0] != '\0') {
        u8g2_SetFont(u8g2, u8g2_font_4x6_tr);
        u8g2_DrawStr(u8g2, 1, HINT_BASELINE, text);
    }
}

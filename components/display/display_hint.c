#include "display_hint.h"

/* Bottom strip geometry. ADSR editors already leave a blank margin below their
 * tick marks (graph_popup.c's ADSR bot_pad); the grid-heavy screens
 * (sequencer/arp/drone-vis) do not, so this strip deliberately overwrites
 * their last content rows rather than shrinking their layouts. */
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

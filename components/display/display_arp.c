#include "display_arp.h"
#include <stdio.h>
#include <string.h>

/* Draw a small label+value field; frame it when it is the cursor.
 * When `editing` and selected, the frame is doubled (a 2px ring) to signal the
 * value is live-adjustable. */
static void draw_field(u8g2_t *u8g2, uint8_t x, uint8_t y, const char *text,
                       bool selected, bool editing)
{
    uint8_t w = (uint8_t)u8g2_GetStrWidth(u8g2, text);
    if (selected) {
        u8g2_DrawRFrame(u8g2, (uint8_t)(x - 2), (uint8_t)(y - 8),
                        (uint8_t)(w + 4), 11, 1);
        if (editing) {
            u8g2_DrawRFrame(u8g2, (uint8_t)(x - 3), (uint8_t)(y - 9),
                            (uint8_t)(w + 6), 13, 1);
        }
    }
    u8g2_DrawStr(u8g2, x, y, text);
}

void display_arp_draw_frame(u8g2_t *u8g2, const arp_view_t *view)
{
    u8g2_ClearBuffer(u8g2);
    u8g2_SetDrawColor(u8g2, 1);

    if (view == NULL) {
        u8g2_SendBuffer(u8g2);
        return;
    }

    char buf[16];
    u8g2_SetFont(u8g2, u8g2_font_5x7_tr);

    /* ── Macro row 1: ARP | MODE | OCT ── */
    snprintf(buf, sizeof(buf), "ARP:%s", view->enabled ? "ON" : "OFF");
    draw_field(u8g2, 2, 8, buf,
               view->cursor == ARP_CUR_ENABLE, view->editing);

    snprintf(buf, sizeof(buf), "%s", view->mode_str ? view->mode_str : "UP");
    draw_field(u8g2, 52, 8, buf,
               view->cursor == ARP_CUR_MODE, view->editing);

    snprintf(buf, sizeof(buf), "OCT:%u", (unsigned)view->octaves);
    draw_field(u8g2, 96, 8, buf,
               view->cursor == ARP_CUR_OCT, view->editing);

    /* ── Macro row 2: RATE | GATE ── */
    snprintf(buf, sizeof(buf), "RATE:%s", view->rate_str ? view->rate_str : "?");
    draw_field(u8g2, 2, 20, buf,
               view->cursor == ARP_CUR_RATE, view->editing);

    snprintf(buf, sizeof(buf), "GATE:%u%%", (unsigned)view->gate_pct);
    draw_field(u8g2, 56, 20, buf,
               view->cursor == ARP_CUR_GATE, view->editing);

    /* Right area of row 2: SOURCE cursor, WAVE cursor, GLIDE cursor, or the
     * patch/wave indicator. In WAVE mode the waveform name replaces the patch
     * number. GLIDE (portamento) has no spare row of its own, so it borrows
     * this same slot, matching the SOURCE/WAVE cursor-swap convention. */
    {
        bool sel_src   = (view->cursor == ARP_CUR_SOURCE);
        bool sel_wave  = (view->cursor == ARP_CUR_WAVE);
        bool sel_porta = (view->cursor == ARP_CUR_PORTA);
        char rbuf[12];
        if (sel_src) {
            snprintf(rbuf, sizeof(rbuf), "SRC:%s",
                     view->wave_mode ? "WAVE" : "PTCH");
        } else if (sel_wave) {
            snprintf(rbuf, sizeof(rbuf), "%s",
                     view->wave_str ? view->wave_str : "?");
        } else if (sel_porta) {
            snprintf(rbuf, sizeof(rbuf), "GLIDE:%u", (unsigned)view->portamento_ms);
        } else if (view->wave_mode) {
            snprintf(rbuf, sizeof(rbuf), "W:%s",
                     view->wave_str ? view->wave_str : "?");
        } else {
            snprintf(rbuf, sizeof(rbuf), "P%u", (unsigned)view->patch);
        }
        uint8_t rw = (uint8_t)u8g2_GetStrWidth(u8g2, rbuf);
        uint8_t rx = (rw < 126u) ? (uint8_t)(126u - rw) : 0u;
        if (sel_src || sel_wave || sel_porta) {
            draw_field(u8g2, rx, 20, rbuf, true, view->editing);
        } else if (!view->wave_mode && view->patch_select) {
            u8g2_DrawRFrame(u8g2, (uint8_t)(rx - 2), 12,
                            (uint8_t)(rw + 4), 11, 1);
            u8g2_DrawStr(u8g2, rx, 20, rbuf);
        } else {
            u8g2_DrawStr(u8g2, rx, 20, rbuf);
        }
    }

    u8g2_DrawHLine(u8g2, 0, 26, 128);

    /* ── Note slots: two rows of 4 cells ── */
    const uint8_t cell_w = 30;
    const uint8_t cell_h = 14;
    const uint8_t base_x = 2;
    const uint8_t base_y = 40;   /* baseline of first slot row text */
    for (uint8_t i = 0; i < ARP_VIEW_SLOTS; i++) {
        uint8_t col = i % 4;
        uint8_t rowi = i / 4;
        uint8_t cx = (uint8_t)(base_x + col * (cell_w + 1));
        uint8_t cy = (uint8_t)(base_y + rowi * (cell_h + 2));

        const char *txt = view->slot_active[i] ? view->slot_name[i]
                        : view->slot_rest[i]   ? " R "
                        :                        "--";
        uint8_t tw = (uint8_t)u8g2_GetStrWidth(u8g2, txt);
        uint8_t tx = (uint8_t)(cx + (cell_w - tw) / 2);

        bool selected = (view->cursor == (ARP_CUR_SLOT0 + i));
        if (selected) {
            u8g2_DrawRFrame(u8g2, cx, (uint8_t)(cy - 10), cell_w, cell_h, 1);
            if (view->editing) {
                u8g2_DrawRFrame(u8g2, (uint8_t)(cx - 1), (uint8_t)(cy - 11),
                                (uint8_t)(cell_w + 2), (uint8_t)(cell_h + 2), 1);
            }
        }
        u8g2_DrawStr(u8g2, tx, cy, txt);
    }

    /* Patch-name banner: while the patch hold+turn gesture is active, draw the
     * current patch's human name centred over the slot grid so the full-range
     * browser is legible — same affordance as the sequencer view. patch_name is
     * NULL when the name table is compiled out, so this costs nothing then. */
    if (!view->wave_mode && view->patch_select && view->patch_name) {
        u8g2_SetFont(u8g2, u8g2_font_6x10_tf);
        uint8_t nw = (uint8_t)u8g2_GetStrWidth(u8g2, view->patch_name);
        if (nw > 124) nw = 124;
        uint8_t bx = (uint8_t)((128 - (nw + 4)) / 2);
        uint8_t by = 34;            /* banner top, over the slot area */
        u8g2_SetDrawColor(u8g2, 0);
        u8g2_DrawBox(u8g2, bx, by, (uint8_t)(nw + 4), 13);
        u8g2_SetDrawColor(u8g2, 1);
        u8g2_DrawRFrame(u8g2, bx, by, (uint8_t)(nw + 4), 13, 2);
        u8g2_DrawStr(u8g2, (uint8_t)(bx + 2), (uint8_t)(by + 9), view->patch_name);
    }

    u8g2_SendBuffer(u8g2);
}

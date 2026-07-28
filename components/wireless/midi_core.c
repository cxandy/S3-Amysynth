#include "midi_core.h"

/* Standard MIDI 1.0 stream parser with running status. Only note-on/note-off
 * reach the sink; every other message is still consumed, since a
 * dropped-but-unparsed message would desync running status. No allocation,
 * locks or logging: runs on the transport's task inside the note path. */

static const midi_sink_t *s_sink = 0;

static uint8_t s_status   = 0;      /* running status (0 = none) */
static uint8_t s_d1       = 0;      /* first data byte of the pending message */
static bool    s_have_d1  = false;
static bool    s_in_sysex = false;

void midi_core_set_sink(const midi_sink_t *sink)
{
    s_sink = sink;
}

void midi_core_reset(void)
{
    s_status   = 0;
    s_have_d1  = false;
    s_in_sysex = false;
}

/* Data-byte count for a channel status byte. */
static uint8_t status_data_len(uint8_t status)
{
    switch (status & 0xF0u) {
        case 0xC0u:            /* program change   */
        case 0xD0u:            /* channel pressure */
            return 1;
        default:
            return 2;
    }
}

void midi_core_feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (b >= 0xF8u) continue;              /* realtime: transparent, keeps
                                                * running status intact */
        if (b & 0x80u) {
            if (b == 0xF0u) {                  /* sysex start */
                s_in_sysex = true;
                s_status   = 0;
                s_have_d1  = false;
            } else if (b == 0xF7u) {           /* sysex end */
                s_in_sysex = false;
            } else if (b >= 0xF0u) {           /* system common: cancels
                                                * running status per spec */
                s_status  = 0;
                s_have_d1 = false;
            } else {                           /* channel status */
                s_status  = b;
                s_have_d1 = false;
            }
            continue;
        }

        /* Data byte. */
        if (s_in_sysex || s_status == 0) continue;

        if (status_data_len(s_status) == 1) continue; /* 1-data messages: drop */

        if (!s_have_d1) {
            s_d1      = b;
            s_have_d1 = true;
            continue;
        }
        s_have_d1 = false;                     /* message complete */

        uint8_t hi = s_status & 0xF0u;
        uint8_t ch = s_status & 0x0Fu;
        if (!s_sink) continue;

        if (hi == 0x90u) {
            if (b == 0) {                      /* vel 0 = note-off per spec */
                if (s_sink->note_off) s_sink->note_off(ch, s_d1);
            } else {
                if (s_sink->note_on) s_sink->note_on(ch, s_d1, b);
            }
        } else if (hi == 0x80u) {
            if (s_sink->note_off) s_sink->note_off(ch, s_d1);
        }
        /* Poly pressure / CC / pitch bend: parsed and dropped (Phase 1). */
    }
}

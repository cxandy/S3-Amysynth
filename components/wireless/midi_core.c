#include "midi_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/* Standard MIDI 1.0 stream parser with running status. Note-on/note-off and
 * the expressive controls (CC, pitch bend) reach the sink; every other
 * message is still consumed, since a dropped-but-unparsed message would
 * desync running status. The parser state
 * is shared by every transport, so the whole feed runs inside a spinlock
 * (BLE-MIDI on the NimBLE host task and DIN-MIDI on its own task both pin to
 * core 0, where a critical section also shields against same-core preemption
 * mid-message). No allocation or logging: stays on the transport's task. */

static const midi_sink_t *s_sink = 0;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t s_status   = 0;      /* running status (0 = none) */
static uint8_t s_d1       = 0;      /* first data byte of the pending message */
static bool    s_have_d1  = false;
static bool    s_in_sysex = false;

void midi_core_set_sink(const midi_sink_t *sink)
{
    portENTER_CRITICAL(&s_mux);
    s_sink = sink;
    portEXIT_CRITICAL(&s_mux);
}

void midi_core_reset(void)
{
    portENTER_CRITICAL(&s_mux);
    s_status   = 0;
    s_have_d1  = false;
    s_in_sysex = false;
    portEXIT_CRITICAL(&s_mux);
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
    portENTER_CRITICAL(&s_mux);
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (b >= 0xF8u) {
            /* Realtime: transparent to running status. Surface the timing
             * clock + song commands so a slave sink can lock to the master. */
            if (s_sink) {
                if (b == 0xF8u && s_sink->clock_tick)         s_sink->clock_tick();
                else if (b == 0xFAu && s_sink->clock_start)   s_sink->clock_start();
                else if (b == 0xFBu && s_sink->clock_start)   s_sink->clock_start();
                else if (b == 0xFCu && s_sink->clock_stop)    s_sink->clock_stop();
            }
            continue;
        }
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
        } else if (hi == 0xE0u) {
            /* Pitch bend, 14-bit, 8192 centered. Passed raw; the sink applies
             * the engine's own conversion (±1/6 octave = ±2 semitones at full
             * travel). */
            if (s_sink->pitch_bend)
                s_sink->pitch_bend(ch, (uint16_t)(s_d1 | (b << 7)));
        } else if (hi == 0xB0u) {
            if (s_sink->cc) s_sink->cc(ch, s_d1, b);
        }
        /* Program / poly pressure: consumed for running status, not surfaced. */
    }
    portEXIT_CRITICAL(&s_mux);
}

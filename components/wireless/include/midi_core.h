#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Transport-agnostic MIDI byte-stream parser + router.
 *
 * Transports (ble_midi today, usb_midi later) strip their framing and feed
 * plain MIDI bytes here; complete note and control messages go to the
 * application's sink. Everything else (program change, poly pressure, sysex)
 * is parsed - keeping running status coherent - and dropped.
 *
 * Threading: the parser state is shared, but every feed is serialized by an
 * internal spinlock, so transports on their own tasks (BLE on the NimBLE host
 * task, DIN on its read task) may call midi_core_feed() freely. The sink runs
 * in the producer's context, so it must be cheap and cross-task safe (the
 * live_play sink goes through the bounded amy_helpers event mutex). */

typedef struct {
    void (*note_on)(uint8_t channel, uint8_t note, uint8_t velocity);
    void (*note_off)(uint8_t channel, uint8_t note);
    /* Optional expressive controls. Called with the raw MIDI values; the sink
     * is omni (channel ignored), matching the note entry points. */
    void (*cc)(uint8_t channel, uint8_t cc, uint8_t value);
    void (*pitch_bend)(uint8_t channel, uint16_t value);
    /* Optional real-time handlers: MIDI timing clock (0xF8), song start or
     * continue (0xFA / 0xFB) and stop (0xFC). NULL skips them. Fired while
     * the parser's spinlock is held, so they must be cheap - leave any heavy
     * work to a deferred task (see midi_clock_slave). */
    void (*clock_tick)(void);
    void (*clock_start)(void);
    void (*clock_stop)(void);
} midi_sink_t;

/* Register the sink. `sink` must have static lifetime; NULL detaches. */
void midi_core_set_sink(const midi_sink_t *sink);

/* Drop parser state (running status, pending data). Call at session start
 * and whenever a transport (re)connects so a torn packet can't misparse. */
void midi_core_reset(void);

/* Feed de-framed MIDI bytes (any split: per byte, per message, per packet). */
void midi_core_feed(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

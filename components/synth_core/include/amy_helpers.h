#pragma once

#include "amy.h"

#ifdef __cplusplus
extern "C" {
#endif

void amy_helpers_init(void);
amy_event *amy_helpers_event_begin(void);
void amy_helpers_event_send(amy_event *event);
void amy_helpers_event_cancel(amy_event *event);

/* Focused one-liners over the begin/send primitive for the two event shapes
 * that recur across the synth modules. Everything else sets its own field mix
 * via begin()/send() directly. */

/* Scheduled note-on/off. Carries the full AMY sequence[] tuple (tag/tick/period)
 * that drives the active-tag index; pass velocity 0.0f for a note-off. */
void amy_send_note_sched(uint8_t synth, float midi_note, float velocity,
                         uint32_t tag, uint32_t tick, uint32_t period);

/* Load a patch onto a synth, sizing its voice pool. synth_flags is passed
 * through (callers may use layer-specific instrument flags). */
void amy_send_patch(uint8_t synth, uint16_t patch_number, uint16_t num_voices,
                    uint32_t synth_flags);

#ifdef __cplusplus
}
#endif

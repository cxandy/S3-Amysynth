#pragma once

#include "amy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

void amy_helpers_init(void);
/* Register the AMY render task so debug builds can assert that no ingress
 * helper is ever called from the locked render body. Lock order is
 * s_event_mutex -> amy_queue_lock/SEQ_LOCK; the render body holds
 * amy_queue_lock and must not re-enter this seam. */
void amy_helpers_set_render_task(TaskHandle_t render_task);
amy_event *amy_helpers_event_begin(void);
void amy_helpers_event_send(amy_event *event);
void amy_helpers_event_cancel(amy_event *event);

/* Focused one-liners over the begin/send primitive for the two event shapes
 * that recur across the synth modules. Everything else sets its own field mix
 * via begin()/send() directly.
 *
 * Two ingress routes exist and the entry-point names state them:
 * - NOTE-send: sequence[] set -> tick-scheduled whole event via
 *   sequencer_add_event under AMY's SEQ_LOCK.
 * - CONFIG-send: no sequence[] -> applied now via add_delta_to_queue under
 *   amy_queue_lock. */

/* Scheduled note-on/off (NOTE route). Carries the full AMY sequence[] tuple
 * (tag/tick/period) that drives the active-tag index; pass velocity 0.0f for a
 * note-off. This signature is the attach seam for per-step parameter locks. */
void amy_helpers_note_send(uint8_t synth, float midi_note, float velocity,
                           uint32_t tag, uint32_t tick, uint32_t period);

/* Send a begin()-obtained event on the CONFIG route: debug-asserts it carries
 * no sequence[] tuple (which would silently reroute it to the tick scheduler),
 * then sends. Use for apply-now synth/FX configuration events. */
void amy_helpers_config_send(amy_event *event);

/* Load a patch onto a synth, sizing its voice pool (CONFIG route).
 * synth_flags is passed through (callers may use layer-specific instrument
 * flags). */
void amy_send_patch(uint8_t synth, uint16_t patch_number, uint16_t num_voices,
                    uint32_t synth_flags);

/* Global panic: release every sounding note across all synths via RESET_ALL_NOTES
 * delta. Safe to call from sequencer/UI core — runs on the render thread. Call on
 * pause to silence mid-gate notes whose scheduled note-offs were just cancelled. */
void amy_send_all_notes_off(void);

#ifdef __cplusplus
}
#endif

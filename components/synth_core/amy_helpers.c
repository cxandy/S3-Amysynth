#include "amy_helpers.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * amy_event is large enough to be risky on small task stacks. First-party synth
 * modules share one scratch event and serialize access around amy_add_event().
 * Callers must not hold the returned pointer after send/cancel.
 */
static amy_event s_event;
static SemaphoreHandle_t s_event_mutex = NULL;
static TaskHandle_t s_render_task = NULL;

/* Bounded wait so a leaked begin/send pair surfaces as a loud assert instead
 * of hanging every AMY sender. Sized well above the worst-case legitimate hold
 * (a patch-string load building four FX reassert events); if we ever wait this
 * long the mutex was leaked, not merely contended. */
#define AMY_HELPERS_EVENT_TIMEOUT_MS 250

void amy_helpers_init(void)
{
    if (s_event_mutex == NULL) {
        s_event_mutex = xSemaphoreCreateMutex();
        configASSERT(s_event_mutex != NULL);
    }
}

/* Lock hierarchy: s_event_mutex is the OUTERMOST first-party lock. It is held
 * ACROSS amy_add_event(), so the acquisition order is
 *     s_event_mutex -> (amy_queue_lock | SEQ_LOCK).
 * The render body (amy_render on Core 1) holds amy_queue_lock for its whole
 * duration, so it must NEVER call an ingress helper. Register its handle here
 * so a debug build can catch that inversion. Optional: the guard is skipped
 * until a handle is set. */
void amy_helpers_set_render_task(TaskHandle_t render_task)
{
    s_render_task = render_task;
}

amy_event *amy_helpers_event_begin(void)
{
    amy_helpers_init();
#if !defined(NDEBUG)
    /* Render must not re-enter the ingress seam while holding amy_queue_lock. */
    configASSERT(s_render_task == NULL ||
                 xTaskGetCurrentTaskHandle() != s_render_task);
#endif
    if (xSemaphoreTake(s_event_mutex,
                       pdMS_TO_TICKS(AMY_HELPERS_EVENT_TIMEOUT_MS)) != pdTRUE) {
        /* A prior begin/send pair leaked the mutex: fail loud, never hang. */
        configASSERT(0 && "amy_helpers: s_event_mutex timeout, leaked begin/send");
    }
    s_event = amy_default_event();
    return &s_event;
}

void amy_helpers_event_send(amy_event *event)
{
    configASSERT(event == &s_event);
    amy_add_event(event);
    xSemaphoreGive(s_event_mutex);
}

void amy_helpers_event_cancel(amy_event *event)
{
    configASSERT(event == &s_event);
    xSemaphoreGive(s_event_mutex);
}

/* ── Typed ingress entry points ─────────────────────────────────────────
 * amy_add_event() routes by shape: an event with sequence[] set is
 * tick-scheduled whole (sequencer_add_event / SEQ_LOCK); one without is
 * applied now (add_delta_to_queue / amy_queue_lock). The two entry points
 * below make that route visible at the call site. The note-send signature is
 * also the seam where per-step parameter-lock fields will attach. */

void amy_helpers_note_send(uint8_t synth, float midi_note, float velocity,
                           uint32_t tag, uint32_t tick, uint32_t period)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth                     = synth;
    e->midi_note                 = midi_note;
    e->velocity                  = velocity;
    e->sequence[SEQUENCE_TAG]    = tag;
    e->sequence[SEQUENCE_TICK]   = tick;
    e->sequence[SEQUENCE_PERIOD] = period;
    amy_helpers_event_send(e);
}

void amy_helpers_config_send(amy_event *event)
{
    /* Config sends must not carry a sequence[] tuple — that shape would be
     * silently rerouted to the tick scheduler instead of applying now. AMY's
     * "unset" state is the AMY_UNSET sentinel, NOT zero (amy_default_event
     * UNSETs all three fields); mirror amy_add_event's own routing test. */
    configASSERT(AMY_IS_UNSET(event->sequence[SEQUENCE_TAG]) &&
                 AMY_IS_UNSET(event->sequence[SEQUENCE_TICK]) &&
                 AMY_IS_UNSET(event->sequence[SEQUENCE_PERIOD]));
    amy_helpers_event_send(event);
}

void amy_send_patch(uint8_t synth, uint16_t patch_number, uint16_t num_voices,
                    uint32_t synth_flags)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth        = synth;
    e->patch_number = patch_number;
    e->num_voices   = num_voices;
    e->synth_flags  = synth_flags;
    amy_helpers_config_send(e);
}

void amy_send_all_notes_off(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->reset_osc = RESET_ALL_NOTES;
    amy_helpers_config_send(e);
}

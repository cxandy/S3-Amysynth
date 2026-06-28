#include "amy_helpers.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/*
 * amy_event is large enough to be risky on small task stacks. First-party synth
 * modules share one scratch event and serialize access around amy_add_event().
 * Callers must not hold the returned pointer after send/cancel.
 */
static amy_event s_event;
static SemaphoreHandle_t s_event_mutex = NULL;

void amy_helpers_init(void)
{
    if (s_event_mutex == NULL) {
        s_event_mutex = xSemaphoreCreateMutex();
        configASSERT(s_event_mutex != NULL);
    }
}

amy_event *amy_helpers_event_begin(void)
{
    amy_helpers_init();
    xSemaphoreTake(s_event_mutex, portMAX_DELAY);
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

void amy_send_note_sched(uint8_t synth, float midi_note, float velocity,
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

void amy_send_patch(uint8_t synth, uint16_t patch_number, uint16_t num_voices,
                    uint32_t synth_flags)
{
    amy_event *e = amy_helpers_event_begin();
    e->synth        = synth;
    e->patch_number = patch_number;
    e->num_voices   = num_voices;
    e->synth_flags  = synth_flags;
    amy_helpers_event_send(e);
}

void amy_send_all_notes_off(void)
{
    amy_event *e = amy_helpers_event_begin();
    e->reset_osc = RESET_ALL_NOTES;
    amy_helpers_event_send(e);
}

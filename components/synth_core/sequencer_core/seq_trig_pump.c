#include "sequencer_core/seq_core_internal.h"
#include "diag_report.h"
#include "freertos/queue.h"

/*
 * Decorated-step trig pump.
 *
 * sequencer_core_service_tick() always runs on amy_usb_render_task (it's
 * called from amy_execute_deltas() -> sequencer_check_and_fill(), inline in
 * amy_update()). trig_schedule_ratchets() ends in amy_helpers_note_send(),
 * whose amy_helpers_event_begin() asserts if the caller is the render task -
 * and would be wrong to allow even without the assert, since it can block for
 * up to AMY_HELPERS_EVENT_TIMEOUT_MS on a mutex/FIFO the render task must
 * never wait on.
 *
 * So the render task's tick hook only enqueues a tiny job descriptor
 * (non-blocking, never waits) and this dedicated task drains it, doing the
 * actual velocity/pitch/tag-emission work and calling amy_helpers_note_send()
 * exactly like any other non-render sender (arp, drone, live-play) already
 * does. Full plan: docs/plans/decorated-trig-render-task-fix-2026-07-29.md.
 *
 * Deadline: a ratchet's k==0 sub-hit is scheduled for now_ticks+1 - the very
 * next sequencer tick (single-digit ms at typical tempos) - and AMY silently
 * drops any event whose tick has already passed. That's why this task runs
 * at a priority above the UI tier and wakes on a blocking queue receive
 * rather than polling.
 */

#define SEQ_TRIG_QUEUE_DEPTH 32
#define SEQ_TRIG_TASK_STACK  4096
#define SEQ_TRIG_TASK_PRIO   6   /* above encoder/button/seq_ui (5), Core 0 */
#define SEQ_TRIG_TASK_CORE   0   /* Core 1 is DSP-budgeted */

typedef struct {
    uint8_t  layer_idx;
    uint8_t  track;
    uint8_t  step;
    uint32_t now_ticks;
} seq_trig_job_t;

static QueueHandle_t s_trig_queue = NULL;
static StaticQueue_t s_trig_queue_struct;
static uint8_t s_trig_queue_storage[SEQ_TRIG_QUEUE_DEPTH * sizeof(seq_trig_job_t)];

/* Diagnostics only - never gate correctness on these. A nonzero drops_full
 * means the queue is genuinely undersized for the pattern being played; a
 * nonzero drops_mutating means the consumer raced a layer add/delete, which
 * the engine already tolerates as an inaudible dropped tick (see the
 * s_layers_mutating check in sequencer_core_service_tick()). */
static uint32_t s_trig_drops_full     = 0;
static uint32_t s_trig_drops_mutating = 0;

static void sequencer_core_trig_pump_report(void);

static void seq_trig_pump_task(void *arg)
{
    (void)arg;
    seq_trig_job_t job;
    for (;;) {
        if (xQueueReceive(s_trig_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        /* Closes the race the deferral itself introduces: a delete_layer()
         * compaction landing between the render task's enqueue and this
         * dequeue would otherwise read a shifted or reused s_layers[] slot.
         * Re-check both the in-flight flag and the (possibly now smaller)
         * layer count before touching s_layers. */
        if (s_layers_mutating || job.layer_idx >= s_num_layers) {
            s_trig_drops_mutating++;
            continue;
        }
        trig_schedule_ratchets(job.layer_idx, &s_layers[job.layer_idx],
                               job.track, job.step, job.now_ticks);
    }
}

void sequencer_core_trig_pump_init(void)
{
    if (s_trig_queue != NULL) return;   /* idempotent, like amy_helpers_init() */

    s_trig_queue = xQueueCreateStatic(SEQ_TRIG_QUEUE_DEPTH, sizeof(seq_trig_job_t),
                                      s_trig_queue_storage, &s_trig_queue_struct);
    configASSERT(s_trig_queue != NULL);

    BaseType_t ok = xTaskCreatePinnedToCore(seq_trig_pump_task, "seq_trig_pump",
                                            SEQ_TRIG_TASK_STACK, NULL,
                                            SEQ_TRIG_TASK_PRIO, NULL,
                                            SEQ_TRIG_TASK_CORE);
    configASSERT(ok == pdPASS);

    diag_register_reporter("seq-trig", sequencer_core_trig_pump_report);
}

void sequencer_core_trig_enqueue(uint8_t layer_idx, uint8_t track, uint8_t step,
                                 uint32_t now_ticks)
{
    seq_trig_job_t job = { layer_idx, track, step, now_ticks };
    /* Zero timeout: the render task must never block here. A full queue means
     * the consumer has stalled or this tick produced more decorated fires
     * than the queue was sized for - drop and count, never wait. */
    if (xQueueSend(s_trig_queue, &job, 0) != pdTRUE) {
        s_trig_drops_full++;
    }
}

static void sequencer_core_trig_pump_report(void)
{
    if (s_trig_drops_full == 0 && s_trig_drops_mutating == 0) return;
    ESP_LOGI(TAG, "seq_trig: drops_full=%u drops_mutating=%u",
             (unsigned)s_trig_drops_full, (unsigned)s_trig_drops_mutating);
}

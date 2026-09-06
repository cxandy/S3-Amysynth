/*
 * midi_clock_slave: lock the sequencer tempo to an external MIDI Clock.
 *
 * Realtime bytes from midi_core (0xF8 tick, 0xFA/0xFB start/continue, 0xFC
 * stop) land on the transport task inside the parser's spinlock, so each
 * callback here is critical-section-safe: esp_timer_get_time() plus a float
 * EMA over the inter-tick interval. 24 ticks make one quarter note, so
 *
 *     bpm = 60 us-per-min / (24 * interval_us) = 2 500 000 / interval_us.
 *
 * The interval estimate is EMA-smoothed, range-gated to SEQ_MIN/MAX_BPM, and
 * surfaced as a "pending" value stashed under a spinlock; this component's
 * own low-priority task flushes it to sequencer_core_set_bpm() a few times a
 * second (that call is too heavy for the transport's critical section).
 * Start/stop follow the master only while a live clock stream has been seen,
 * so a keyboard that never sends clock can't hijack the local transport.
 *
 * Tempo lock only - song-position (0xF2 SPP) and sample-accurate re-phase of
 * the sequencer steps are out of scope for this pass.
 *
 * No-op when CONFIG_AMYSYNTH_MIDI_CLOCK_SLAVE is off.
 */

#include "midi_clock_slave.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "sequencer_core.h"           /* sequencer_core_set_bpm / set_playing */

static const char *TAG = "midi_clock_slave";

#if CONFIG_AMYSYNTH_MIDI_CLOCK_SLAVE

/* Same operating range as the sequencer's own clamp (seq_core_config.h). */
#define CLOCK_MIN_BPM       40u
#define CLOCK_MAX_BPM       300u
#define CLOCK_TICK_EMA      0.25f
#define CLOCK_TICK_JITTERUS 3000     /* min interval below SEQ_MAX_BPM */
#define CLOCK_TICK_DROPUS   100000   /* max interval above SEQ_MIN_BPM  */
#define CLOCK_ALIVE_US      500000   /* ^2 consecutive-tick grace for start/stop */
#define CLOCK_FLUSH_MS      20

#define MIDI_CLOCK_TASK_STACK_WORDS 2048
#define MIDI_CLOCK_TASK_PRIO        2

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* Written by the transport task (tick/start/stop callbacks), read by the
 * flush task under s_mux. Plain scalars; no pointers to walk. */
static int64_t s_last_tick_us = 0;
static float   s_interval_ema_us = 0.0f;
static bool    s_have_interval = false;
static bool    s_clock_seen = false;
static int64_t s_last_any_us = 0;

static uint16_t s_pending_bpm = 0;   /* 0 = none (selected above CLOCK_MIN_BPM) */
static bool     s_pending_start = false;
static bool     s_pending_stop  = false;

/* Last value actually pushed to sequencer_core_set_bpm, so the flush task
 * only pokes the sequencer when the estimate moved. Numeral only, written by
 * the flush task and read (under s_mux) by the transport task: a torn read
 * on core 0 can at worst cost one redundant set_bpm, self-corrected next
 * tick - aligned 16-bit, no pointer to walk. */
static volatile uint16_t s_applied_bpm = 0;

void midi_clock_slave_tick(void)
{
    int64_t now = esp_timer_get_time();

    portENTER_CRITICAL(&s_mux);
    if (s_have_interval) {
        float dt = (float)(now - s_last_tick_us);
        if (dt >= CLOCK_TICK_JITTERUS && dt <= CLOCK_TICK_DROPUS) {
            float ema = s_interval_ema_us + CLOCK_TICK_EMA * (dt - s_interval_ema_us);
            s_interval_ema_us = ema;
            float bpm  = 2500000.0f / ema;
            if (bpm < (float)CLOCK_MIN_BPM) bpm = (float)CLOCK_MIN_BPM;
            if (bpm > (float)CLOCK_MAX_BPM) bpm = (float)CLOCK_MAX_BPM;
            uint16_t sel = (uint16_t)lrintf(bpm);
            s_pending_bpm = (sel != s_applied_bpm) ? sel : 0;
        }
    } else {
        /* First clock tick: seed the estimate with the next one. */
        s_have_interval = true;
        s_interval_ema_us = 0.0f;
    }
    s_clock_seen = true;
    s_last_tick_us = now;
    s_last_any_us = now;
    portEXIT_CRITICAL(&s_mux);
}

void midi_clock_slave_start(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (s_clock_seen && (now - s_last_any_us) < CLOCK_ALIVE_US)
        s_pending_start = true;
    portEXIT_CRITICAL(&s_mux);
}

void midi_clock_slave_stop(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_mux);
    if (s_clock_seen && (now - s_last_any_us) < CLOCK_ALIVE_US)
        s_pending_stop = true;
    portEXIT_CRITICAL(&s_mux);
}

/* Flush task: the pending BPM / transport commands are applied *here*, off
 * the transport's critical section - sequencer_core_set_bpm is too heavy
 * (layer loop, LFO refresh, AMY event push) to run under the parser lock. */
static void midi_clock_slave_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint16_t bpm = 0;
        bool want_start = false;
        bool want_stop  = false;

        portENTER_CRITICAL(&s_mux);
        bpm        = s_pending_bpm;
        want_start = s_pending_start;
        want_stop  = s_pending_stop;
        s_pending_bpm    = 0;
        s_pending_start  = false;
        s_pending_stop   = false;
        portEXIT_CRITICAL(&s_mux);

        if (bpm) {
            if (bpm >= CLOCK_MIN_BPM && bpm <= CLOCK_MAX_BPM) {
                sequencer_core_set_bpm(bpm);
                s_applied_bpm = bpm;          /* single-core task: own it */
            }
        }
        if (want_start) sequencer_core_set_playing(true);
        if (want_stop)  sequencer_core_set_playing(false);

        vTaskDelay(pdMS_TO_TICKS(CLOCK_FLUSH_MS));
    }
    vTaskDelete(NULL);
}

#endif /* CONFIG_AMYSYNTH_MIDI_CLOCK_SLAVE */

esp_err_t midi_clock_slave_init(void)
{
#if CONFIG_AMYSYNTH_MIDI_CLOCK_SLAVE
    if (xTaskCreatePinnedToCore(midi_clock_slave_task, "midi_clock_slave",
                                MIDI_CLOCK_TASK_STACK_WORDS, NULL,
                                MIDI_CLOCK_TASK_PRIO, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "following external MIDI clock (BPM lock), start/stop gated on live clock");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "disabled");
    return ESP_OK;
#endif
}
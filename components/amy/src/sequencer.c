#include "sequencer.h"
#include "amy.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ── LOCAL EDIT (S3-Amysynth): Sequence-array lock ───────────────────────
 * sequences[] is written by sequencer_add_event() (called from any task)
 * and read by sequencer_process_tick() (since v1.2.104 driven per block from
 * amy_execute_deltas() on the render task; previously an esp_timer task).
 * On SMP these run concurrently.
 * Use a dedicated mutex so we don't conflict with amy_queue_lock, which
 * guards delta_queue and is re-acquired inside add_delta_to_queue() while
 * sequencer_process_tick() is still running. (amy_execute_deltas() calls
 * sequencer_check_and_fill() BEFORE it takes amy_queue_lock, so the
 * SEQ_LOCK -> amy_queue_lock ordering here is the only ordering that exists.)
 * See AMY-EDITS.md.
 */
#ifdef ESP_PLATFORM
#  include "freertos/FreeRTOS.h"
#  include "freertos/semphr.h"
static SemaphoreHandle_t s_seq_lock = NULL;
#  define SEQ_LOCK()   xSemaphoreTake(s_seq_lock, portMAX_DELAY)
#  define SEQ_UNLOCK() xSemaphoreGive(s_seq_lock)
#elif defined(_POSIX_THREADS)
#  include <pthread.h>
static pthread_mutex_t s_seq_lock = PTHREAD_MUTEX_INITIALIZER;
#  define SEQ_LOCK()   pthread_mutex_lock(&s_seq_lock)
#  define SEQ_UNLOCK() pthread_mutex_unlock(&s_seq_lock)
#else
#  define SEQ_LOCK()   do {} while(0)
#  define SEQ_UNLOCK() do {} while(0)
#endif

uint32_t sequencer_ticks() { return amy_global.sequencer_tick_count; }

typedef struct sequence_info_t {
    struct delta *deltas;
    //uint32_t tag;  // tag is implicit, it's its index in the table
    uint32_t tick; // 0 means not used
    uint32_t period; // 0 means not used
} sequence_info_t;

struct sequence_info_t *sequences = NULL;  // An array indexed by tag.
int32_t max_sequences = 0;
int32_t highest_tag = -1;
static volatile bool sequencer_running = true;
static volatile bool sequencer_external_clock = false;

/* ── LOCAL EDIT (S3-Amysynth): Active-tag index ──────────────────────────
 * The per-tick scan used to walk 0..highest_tag, where highest_tag is a
 * high-water mark that only ever grows (e.g. the arp pushes it to ~1119).
 * That made every 500µs tick O(highest tag ever used), scanning thousands of
 * mostly-NULL slots on the audio-critical Core-0 timer.
 *
 * Instead we keep a compact list of only the tags that currently have deltas,
 * so the tick is O(active events) regardless of tag magnitude.
 * s_active_tags is the dense list; s_tag_slot maps tag -> its index (-1 = inactive)
 * for O(1) swap-removal. Both are guarded by SEQ_LOCK. See AMY-EDITS.md.
 *
 * int16_t: both arrays hold tag numbers / list indices < max_sequences, and
 * these arrays live in scarce internal RAM (ram_caps_synth) sized per tag —
 * int16 halves their cost (2 x 2 B/tag instead of 2 x 4 B). sequencer_init
 * clamps max_sequences below INT16_MAX so the -1 sentinel and the values
 * always fit. */
static int16_t *s_active_tags = NULL;
static int16_t *s_tag_slot    = NULL;
static int32_t  s_num_active  = 0;

static inline void seq_active_add(int32_t tag) {
    if (s_tag_slot == NULL || s_tag_slot[tag] >= 0) return;
    s_tag_slot[tag] = (int16_t)s_num_active;
    s_active_tags[s_num_active++] = (int16_t)tag;
}

static inline void seq_active_remove(int32_t tag) {
    if (s_tag_slot == NULL) return;
    int32_t pos = s_tag_slot[tag];
    if (pos < 0) return;
    int32_t last = --s_num_active;
    int16_t moved = s_active_tags[last];
    s_active_tags[pos] = moved;
    s_tag_slot[moved]  = (int16_t)pos;
    s_tag_slot[tag]    = -1;
}

void sequencer_init(int max_sequencer_tags) {
    // These are statics, so a stop/start of AMY within one process needs them
    // put back to their boot state (internal clock, running).
    sequencer_running = true;
    sequencer_external_clock = false;
    /* LOCAL EDIT (S3-Amysynth): the active-tag index stores tags as int16_t
     * (see above); clamp so tag values and the -1 sentinel always fit. */
    if (max_sequencer_tags > 32766) max_sequencer_tags = 32766;
    max_sequences = max_sequencer_tags;
    sequences = (struct sequence_info_t *)malloc_caps(max_sequences * sizeof(struct sequence_info_t),
                                                      amy_global.config.ram_caps_synth);
    s_active_tags = (int16_t *)malloc_caps(max_sequences * sizeof(int16_t),
                                           amy_global.config.ram_caps_synth);
    s_tag_slot    = (int16_t *)malloc_caps(max_sequences * sizeof(int16_t),
                                           amy_global.config.ram_caps_synth);
    /* LOCAL EDIT (S3-Amysynth): boot-time allocs, but unchecked NULLs here
     * would crash in the init loop below (delta-pool OOM bug family). On any
     * failure disable the sequencer wholesale: with max_sequences = 0,
     * sequencer_add_event rejects every tag and the tick scans nothing. */
    if (sequences == NULL || s_active_tags == NULL || s_tag_slot == NULL) {
        fprintf(stderr, "sequencer_init: OOM (%d tags) - sequencer disabled\n",
                max_sequencer_tags);
        if (sequences)     { free(sequences);     sequences = NULL; }
        if (s_active_tags) { free(s_active_tags); s_active_tags = NULL; }
        if (s_tag_slot)    { free(s_tag_slot);    s_tag_slot = NULL; }
        max_sequences = 0;
    }
    s_num_active  = 0;
    for (int32_t i = 0; i < max_sequences; ++i) {
        sequences[i].deltas = NULL;
        sequences[i].tick = 0;
        sequences[i].period = 0;
        s_tag_slot[i] = -1;
    }
    // We are read to go.
    sequencer_recompute();
#ifdef ESP_PLATFORM
    if (s_seq_lock == NULL) s_seq_lock = xSemaphoreCreateMutex();
#endif
}

void sequencer_reset() {
    // Remove all events
    for (int32_t i = 0; i < max_sequences; ++i) {
        if (sequences[i].deltas) {
            delta_release_list(sequences[i].deltas);
            sequences[i].deltas = NULL;
            sequences[i].tick = 0;
            sequences[i].period = 0;
        }
        if (s_tag_slot) s_tag_slot[i] = -1;
    }
    s_num_active = 0;
    highest_tag = -1;
}

void sequencer_deinit() {
    sequencer_reset();
    if (sequences != NULL) free(sequences);
    if (s_active_tags != NULL) { free(s_active_tags); s_active_tags = NULL; }
    if (s_tag_slot != NULL)    { free(s_tag_slot);    s_tag_slot = NULL; }
    sequences = NULL;  // sequencer_check_and_fill guards on this
    max_sequences = 0;
}

void sequencer_debug() {
    fprintf(stderr, "sequencer: max_sequences %" PRIi32" highest_tag %" PRIi32 "\n", max_sequences, highest_tag);
    for (int32_t tag = 0; tag < max_sequences; ++tag) {
        if (sequences[tag].deltas) {
            fprintf(stderr, "sequence tag %" PRIu32" tick %" PRIu32 " period %"PRIu32 " num_deltas %"PRIu32 "\n",
                    tag, sequences[tag].tick, sequences[tag].period, delta_list_len(sequences[tag].deltas));
        }
    }
}

void sequencer_recompute() {
    // 60000000 us/min / (bpm * ticks per beat); keep it single-precision -
    // unsuffixed double literals pull in software double emulation on 32-bit.
    amy_global.us_per_tick = (uint32_t) (60000000.0f / (amy_global.tempo * (float)AMY_SEQUENCER_PPQ));
    amy_global.next_amy_tick_us = (amy_sysclock64() * 1000ULL) + (uint64_t)amy_global.us_per_tick;
}

static void sequencer_process_tick(void) {
    amy_global.sequencer_tick_count++;
    midi_clock_out_tick();  // no-op unless in AMY_MIDI_SYNC_SEND mode
    // LOCAL EDIT (S3-Amysynth): scan only the currently-active tags O(active),
    // not 0..highest_tag O(highest_tag_ever). Walk backwards so swap-remove
    // inside seq_active_remove never skips an entry. See AMY-EDITS.md.
    SEQ_LOCK();
    for (int32_t i = s_num_active - 1; i >= 0; --i) {
        int32_t tag = s_active_tags[i];
        if (sequences[tag].deltas != NULL) {
            bool hit = false;
            bool delete = false;
            if(sequences[tag].period != 0) { // period set
                uint32_t offset = amy_global.sequencer_tick_count % sequences[tag].period;
                if (offset == sequences[tag].tick) hit = true;
            } else {
                // Test for absolute tick (no period set)
                if (sequences[tag].tick == amy_global.sequencer_tick_count) { hit = true; delete = true; }
            }
            if(hit) {
                struct delta *d = sequences[tag].deltas;
                while(d) {
                    // assume the d->time is 0 and that's good.
                    add_delta_to_queue(d, &amy_global.delta_queue);
                    d = d->next;
                }
                // Delete absolute tick addressed sequence entry if sent
                if(delete) {
                    delta_release_list(sequences[tag].deltas);
                    sequences[tag].deltas = NULL;
                    seq_active_remove(tag);
                }
            }
        }
    }
    SEQ_UNLOCK();
    // call the right hook:
#ifdef __EMSCRIPTEN__
    EM_ASM({
        if(typeof amy_sequencer_js_hook === 'function') {
            amy_sequencer_js_hook($0);
        }
    }, amy_global.sequencer_tick_count);
#endif
    if(amy_global.config.amy_external_sequencer_hook != NULL) {
        amy_global.config.amy_external_sequencer_hook(amy_global.sequencer_tick_count);
    }
}

#ifdef __EMSCRIPTEN__
// On the web, ticks are counted in the render loop, which runs in the
// AudioWorklet thread -- EM_ASM there can't reach the page's JS, where
// amy_sequencer_js_hook is defined. The emscripten main loop calls this from
// the browser main thread to replay elapsed ticks to the hook.
void sequencer_check_and_call_js_hook() {
    static uint32_t last_reported_tick = 0;
    uint32_t tick = amy_global.sequencer_tick_count;
    if (tick < last_reported_tick) last_reported_tick = tick;  // sequencer was reset
    // If we're more than a second of ticks behind (e.g. the page was
    // backgrounded and the main loop paused), skip ahead rather than firing a
    // burst of stale hook calls.
    if (amy_global.us_per_tick > 0) {
        uint32_t ticks_per_sec = 1000000 / amy_global.us_per_tick;
        if (tick - last_reported_tick > ticks_per_sec) last_reported_tick = tick - ticks_per_sec;
    }
    while (last_reported_tick < tick) {
        ++last_reported_tick;
        EM_ASM({
            if(typeof amy_sequencer_js_hook === 'function') {
                amy_sequencer_js_hook($0);
            }
        }, last_reported_tick);
    }
}
#endif

void sequencer_midi_start() {
    // MIDI "Start" restarts the sequencer.
    // If external clock was not previously enabled, keep using internal clock
    // so the sequencer advances on its own without needing F8 ticks.
    if (sequencer_external_clock) {
        amy_global.sequencer_tick_count = 0;
    }
    // Reset the tick timer to now so sequencer_check_and_fill doesn't try to
    // catch up all the ticks that elapsed while stopped.
    amy_global.next_amy_tick_us = amy_sysclock64() * 1000ULL;
    sequencer_running = true;
    midi_clock_out_start();  // tell downstream slaves, if we're the clock master
}

void sequencer_midi_stop() {
    sequencer_running = false;
    midi_clock_out_stop();  // tell downstream slaves, if we're the clock master
}

void sequencer_midi_clock_tick() {
    sequencer_external_clock = true;
    if (!sequencer_running) return;
    for (uint8_t i = 0; i < AMY_SEQUENCER_PPQ/MIDI_SEQUENCER_PPQ; ++i) {
        sequencer_process_tick();
    }
}

void sequencer_external_clock_disable() {
    // Leave external-clock mode and hand back to the internal timer. Without
    // this, sequencer_external_clock latches true on the first F8 tick and is
    // never cleared, so the internal sequencer stays dead once an external
    // clock stops -- even after the caller turns external sync back off. Called
    // from amy_external_midi_sync(0) so disabling sync is a real recovery path.
    sequencer_external_clock = false;
    sequencer_running = true;
    // Re-anchor the tick timer to now so sequencer_check_and_fill doesn't try to
    // replay every tick that elapsed while we were on external clock.
    amy_global.next_amy_tick_us = amy_sysclock64() * 1000ULL;
}

uint8_t sequencer_add_event(amy_event *e) {
    // add this event to the list of sequencer events in the LL.
    // e->sequence is set up.
    // if the tag already exists - if there's tick/period, overwrite, if there's no tick / period, we should remove the entry
    //fprintf(stderr, "sequencer_add_event: e->instrument %d e->note %.0f e->vel %.2f tick %d period %d tag %d\n", e->instrument, e->midi_note, e->velocity, e->sequence[SEQUENCE_TICK], e->sequence[SEQUENCE_PERIOD], e->sequence[SEQUENCE_TAG]);
    int32_t tag = e->sequence[SEQUENCE_TAG];
    /* LOCAL EDIT (S3-Amysynth): sequencer disabled by init-time OOM. */
    if (sequences == NULL) return 0;
    if (tag > max_sequences) {
        fprintf(stderr, "sequencer tag %" PRIi32" (with tick %" PRIu32", period %" PRIu32") is greater than or eq max_sequences %" PRIi32"\n",
                tag, e->sequence[SEQUENCE_TICK], e->sequence[SEQUENCE_PERIOD], max_sequences);
        // ignore
        return 0;
    }
    SEQ_LOCK();
    // Release any existing deltas for this tag, even if we're just going to rewrite them.
    delta_release_list(sequences[tag].deltas);
    sequences[tag].deltas = NULL;

    // Clearing a tag (no tick/period, or scheduled in the past) makes it
    // inactive: drop it from the active-tag index so the per-tick scan shrinks.
    if(e->sequence[SEQUENCE_TICK] == 0 && e->sequence[SEQUENCE_PERIOD] == 0) {
        seq_active_remove(tag); SEQ_UNLOCK(); return 0;
    }
    if(e->sequence[SEQUENCE_TICK] != 0 && e->sequence[SEQUENCE_PERIOD] == 0 && e->sequence[SEQUENCE_TICK] <= amy_global.sequencer_tick_count) {
        seq_active_remove(tag); SEQ_UNLOCK(); return 0;
    }

    // Save the tick & period.
    sequences[tag].tick = e->sequence[SEQUENCE_TICK];
    sequences[tag].period = e->sequence[SEQUENCE_PERIOD];
    // Copy all the deltas for this event to the sequences entry.
    amy_event_to_deltas_queue(e, 0, &sequences[tag].deltas);

    seq_active_add(tag);
    if (tag > highest_tag) highest_tag = tag;  // kept for sequencer_debug() only
    SEQ_UNLOCK();
    return 1;
}


// Called once per block from amy_execute_deltas(). Ticks are decided against
// amy_sysclock(), which counts rendered samples, so the sequencer advances on
// AMY time in any rendering context (live, offline, tests).
void sequencer_check_and_fill() {
    if (sequences == NULL) return;  // sequencer_init hasn't run
    if (sequencer_external_clock) return;
    // When we're the MIDI clock master, realtime clock keeps flowing even while
    // the transport is stopped so slaves stay tempo-locked; otherwise a stopped
    // sequencer has nothing to do.
    if (!sequencer_running && !midi_clock_out_enabled()) return;
    // If we've fallen behind by more than 1 second (e.g. sequencer was stopped
    // and restarted, or a long blocking operation occurred), skip ahead instead
    // of processing hundreds of backed-up ticks at once.
    // next_amy_tick_us is a 64-bit accumulator, so it must be anchored to the
    // 64-bit clock. Seeding it from the 32-bit amy_sysclock() used to kill the
    // sequencer permanently at the 49.7-day rollover: now_us collapsed to ~0
    // while next_amy_tick_us stayed at ~4.3e12, and neither the catch-up guard
    // (which only handles falling behind) nor the tick loop could ever fire.
    uint64_t now_us = amy_sysclock64() * 1000ULL;
    if (now_us > amy_global.next_amy_tick_us + 1000000ULL) {
        amy_global.next_amy_tick_us = now_us;
    }
    // The while is in case the timer fires later than a tick; (on esp this would be due to SPI or wifi ops)
    while(now_us >= amy_global.next_amy_tick_us) {
        if (sequencer_running) sequencer_process_tick();
        else midi_clock_out_tick();  // transport stopped: clock only, no sequence events
        amy_global.next_amy_tick_us = amy_global.next_amy_tick_us + (uint64_t)amy_global.us_per_tick;
    }
}

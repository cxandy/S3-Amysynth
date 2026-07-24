#include "sequencer.h"
#include "amy.h"

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

/* ── LOCAL EDIT (S3-Amysynth): Sequence-array lock ───────────────────────
 * sequences[] is written by sequencer_add_event() (called from any task)
 * and read by sequencer_process_tick() (called from the esp_timer task on
 * ESP, or the sequencer thread on POSIX).  On SMP these run concurrently.
 * Use a dedicated mutex so we don't conflict with amy_queue_lock, which
 * guards delta_queue and is re-acquired inside add_delta_to_queue() while
 * sequencer_process_tick() is still running. See AMY-EDITS.md.
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

// Declared per-platform below.
void _sequencer_start();
void _sequencer_stop();

void sequencer_init(int max_sequencer_tags) {
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
    //sequencer_start();
    sequencer_recompute();
#ifdef ESP_PLATFORM
    if (s_seq_lock == NULL) s_seq_lock = xSemaphoreCreateMutex();
#endif
    _sequencer_start();
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
    _sequencer_stop();
    sequencer_reset();
    if (sequences != NULL) free(sequences);
    if (s_active_tags != NULL) { free(s_active_tags); s_active_tags = NULL; }
    if (s_tag_slot != NULL)    { free(s_tag_slot);    s_tag_slot = NULL; }
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
    // LOCAL EDIT (S3-Amysynth): all-float, single divide. The unsuffixed
    // 1000000.0 / 60.0 literals promoted the whole expression to double,
    // which is software-emulated on this target (and most 32-bit ports).
    // 60000000 us/min / (bpm * ticks-per-beat) == the original expression.
    // Upstream-PR candidate.
    amy_global.us_per_tick = (uint32_t) (60000000.0f / (amy_global.tempo * (float)AMY_SEQUENCER_PPQ));
    amy_global.next_amy_tick_us = (((uint64_t)amy_sysclock()) * 1000L) + (uint64_t)amy_global.us_per_tick;
}

static void sequencer_process_tick(void) {
    amy_global.sequencer_tick_count++;
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

void sequencer_midi_start() {
    // MIDI "Start" restarts the sequencer.
    // If external clock was not previously enabled, keep using internal clock
    // so the sequencer advances on its own without needing F8 ticks.
    if (sequencer_external_clock) {
        amy_global.sequencer_tick_count = 0;
    }
    // Reset the tick timer to now so sequencer_check_and_fill doesn't try to
    // catch up all the ticks that elapsed while stopped.
    amy_global.next_amy_tick_us = (uint64_t)amy_sysclock() * 1000L;
    sequencer_running = true;
}

void sequencer_midi_stop() {
    sequencer_running = false;
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
    amy_global.next_amy_tick_us = (uint64_t)amy_sysclock() * 1000L;
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


void sequencer_check_and_fill() {
    if (!sequencer_running || sequencer_external_clock) return;
    // If we've fallen behind by more than 1 second (e.g. sequencer was stopped
    // and restarted, or a long blocking operation occurred), skip ahead instead
    // of processing hundreds of backed-up ticks at once.
    uint64_t now_us = (uint64_t)amy_sysclock() * 1000L;
    if (now_us > amy_global.next_amy_tick_us + 1000000ULL) {
        amy_global.next_amy_tick_us = now_us;
    }
    // The while is in case the timer fires later than a tick; (on esp this would be due to SPI or wifi ops)
    // LOCAL EDIT (S3-Amysynth): compare in the us domain against the now_us
    // already computed above. The old ms-domain compare re-read amy_sysclock()
    // AND paid a 64-bit divide (next_amy_tick_us / 1000) on every check - a
    // real __udivdi3 libcall per 500 us timer callback on 32-bit targets,
    // since GCC cannot strength-reduce a 64-bit divide by a constant there.
    // Behavior delta: a tick's deadline is no longer rounded down to the ms
    // boundary, so a tick can fire up to 1 ms later (never earlier) within
    // the same 500 us callback grid; the accumulated tick rate is unchanged.
    // Upstream-PR candidate.
    while(now_us >= amy_global.next_amy_tick_us) {
        sequencer_process_tick();
        amy_global.next_amy_tick_us = amy_global.next_amy_tick_us + (uint64_t)amy_global.us_per_tick;
    }
}

///// Sequencers per platform

#ifdef ESP_PLATFORM
// ESP: do it with hardware timer
#include "esp_timer.h"
esp_timer_handle_t periodic_timer = NULL;

static void sequencer_timer_callback(void* arg) {
    sequencer_check_and_fill();
}

void _sequencer_start() {
    const esp_timer_create_args_t periodic_timer_args = {
            .callback = &sequencer_timer_callback,
            //.dispatch_method = ESP_TIMER_ISR,
            .name = "sequencer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    // 500us = 0.5ms
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 500));
}

void _sequencer_stop() {
    if (periodic_timer) {
        ESP_ERROR_CHECK(esp_timer_stop(periodic_timer));
        ESP_ERROR_CHECK(esp_timer_delete(periodic_timer));
        periodic_timer = NULL;
    }
}

#elif defined(PICO_RP2350) || defined(PICO_RP2040)
// pico: do it with a hardware timer

#include "pico/time.h"
repeating_timer_t pico_sequencer_timer;

static bool sequencer_timer_callback(repeating_timer_t *rt) {
    sequencer_check_and_fill();
    return true;
}

void _sequencer_start() {
    add_repeating_timer_us(-500, sequencer_timer_callback, NULL, &pico_sequencer_timer);
}

void _sequencer_stop() {
    cancel_repeating_timer(&pico_sequencer_timer);
}

#elif defined _POSIX_THREADS
#include <pthread.h>

static volatile bool sequencer_thread_should_exit = false;

// posix: threads
void * sequencer_thread(void *vargs) {
    // Loop forever, checking for time and sleeping
    while(!sequencer_thread_should_exit) {
        sequencer_check_and_fill();            
        // 500000ns = 500us = 0.5ms
        nanosleep((const struct timespec[]){{0, 500000L}}, NULL);
    }
    return NULL;
}
pthread_t sequencer_thread_id;
void _sequencer_start() {
    sequencer_thread_should_exit = false;
    pthread_create(&sequencer_thread_id, NULL, sequencer_thread, NULL);
}
void _sequencer_stop() {
    //pthread_cancel(sequencer_thread_id);
    sequencer_thread_should_exit = true;
}

#elif defined AMY_DAISY
void _sequencer_start() {
    // Set up in DaisyMain.cc
}
void _sequencer_stop() {
    // Not supported on Daisy.
}

#else

void _sequencer_start() {
    fprintf(stderr, "No sequencer support for this chip / platform\n");
}
void _sequencer_stop() {
    fprintf(stderr, "No sequencer support for this chip / platform\n");
}

#endif

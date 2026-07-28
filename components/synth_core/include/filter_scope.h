#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Filter scope: live modulated-cutoff tap for the filter editor ────────
 *
 * Publishes what the filter is ACTUALLY doing so EG1 sweeps and filter LFO
 * motion are visible in the editor rather than inferred from authored numbers.
 *
 * This does not sample audio: AMY already computes the modulated cutoff as a
 * control-rate scalar once per block (msynth[osc]->filter_logfreq), and this
 * just reads it. The response curve stays as approximate as it is today - only
 * the cutoff feeding it becomes live.
 *
 * Concurrency. The render task (Core 1) holds amy_queue_lock across the whole
 * render body and msynth[] is not double-buffered, so the UI task must NEVER
 * read it. The render task instead taps after amy_update() returns, with the
 * lock released, and publishes into the cell. Ownership keeps it lock-free:
 *
 *   - the UI writes exactly one word, read_epoch, when it consumes a band;
 *   - the render tap owns the accumulator and restarts it, rather than folding
 *     into it, whenever it observes a new epoch.
 *
 * Every word has exactly one writer. A torn or one-frame-stale read of the
 * float pair is possible and irrelevant: the data is display-only. */

#if CONFIG_FILTER_SCOPE

/* Hard cap on the armed oscillator list. It is a fixed-size array inside the
 * cell that is never freed, so a torn read during a re-arm can only yield a
 * stale-but-in-range index, never a wild pointer. */
#define FILTER_SCOPE_MAX_OSCS 16

/* Arm the scope on an explicit oscillator list (the caller resolves target ->
 * synth slot -> voices -> base osc). Passing n == 0 disarms.
 *
 * Ordering is load-bearing and handled internally: the armed flag is cleared
 * FIRST, the list and its length are rewritten, and the flag is set LAST, so
 * the flag is the single word that transfers ownership of the list. */
void filter_scope_arm(const uint16_t *oscs, uint8_t n);

/* Stop publishing. One relaxed store; safe to call when already disarmed. */
void filter_scope_disarm(void);

/* Render-task hook. MUST be called from the render task after amy_update()
 * has returned, i.e. with amy_queue_lock released. Costs one load and one
 * predicted-not-taken branch while disarmed. */
void filter_scope_render_tick(void);

/* UI-side drain. False when nothing was published since the last call (not
 * armed, or no target voice sounding). Always bumps read_epoch, which makes
 * the render tap restart its accumulator, so consecutive calls report disjoint
 * time windows. Values are AMY log-frequency (msynth[osc]->filter_logfreq). */
bool filter_scope_read(float *lo_logfreq, float *hi_logfreq);

#endif /* CONFIG_FILTER_SCOPE */

#ifdef __cplusplus
}
#endif

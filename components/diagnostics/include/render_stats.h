#pragma once
// Per-block render-time statistics. Writer: the render task (Core 1), one
// begin/end pair per block. Reader: the app_main idle loop (Core 0). All
// calls compile to nothing when CONFIG_AMYSYNTH_RENDER_STATS is off.
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

#if CONFIG_AMYSYNTH_RENDER_STATS

// Start timing a render block. Also applies any pending window reset
// requested by the reader, so the reset happens on the writer's side.
void render_stats_block_begin(void);

// Finish timing the block and fold it into the current window.
// seq_tick_coincident: a sequencer tick hook ran inside this block (captured
// into the worst-block snapshot to correlate spikes with tick work).
void render_stats_block_end(bool seq_tick_coincident);

// Print the window (min/mean/p99/max, histogram, spike counts, worst-block
// snapshot) and request a window reset. Core-0 only; never call on the
// render task.
void render_stats_report(void);

#else

static inline void render_stats_block_begin(void) {}
static inline void render_stats_block_end(bool seq_tick_coincident)
{
    (void)seq_tick_coincident;
}
static inline void render_stats_report(void) {}

#endif

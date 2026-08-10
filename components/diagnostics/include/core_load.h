#pragma once
// Per-core busy-percentage sampler over the FreeRTOS run-time counters.
// Extracted from the rtos_stats diagnostic dump so lightweight consumers
// (status LED) can read core load without pulling in the full task-table
// report. Load is derived from the IDLE0/IDLE1 run-time counters, so it is
// the full per-core busy share (tasks + ISR time attributed to them), not
// just one task's slice.
//
// Obligations: call from a single task only (static delta state, no
// locking); never from an ISR. Needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
// (which selects the trace facility); without it the stub below always
// returns false. Keep the call interval >= ~100 ms so scheduler noise
// averages out, and well under the run-time counter wrap (~71 min with the
// esp_timer clock source).
//
// Guarantees: returns true with busy_pct[core] = 0..100, the average busy
// share of each core since the previous successful call. Returns false with
// busy_pct untouched on the first call (baseline capture) or when the
// snapshot fails; the next successful call then averages over the whole gap.
// Wrap-safe across one counter wrap between calls.
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

#define CORE_LOAD_NUM_CORES CONFIG_FREERTOS_NUMBER_OF_CORES

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

bool core_load_sample(uint8_t busy_pct[CORE_LOAD_NUM_CORES]);

#else

static inline bool core_load_sample(uint8_t busy_pct[CORE_LOAD_NUM_CORES])
{
    (void)busy_pct;
    return false;
}

#endif

#pragma once
/* Periodic RTOS profiling dump: per-task table (core, priority, stack high
 * water mark, cumulative CPU %), per-core busy/idle % and a heap snapshot.
 *
 * Needs the FreeRTOS trace facility, run-time stats and COREID Kconfig options
 * set alongside CONFIG_AMYSYNTH_RTOS_STATS. Without the gate the call compiles
 * to nothing.
 *
 * The dump allocates a TaskStatus_t array and walks every task, so it belongs
 * on a low-priority task on the non-render core - never the render path.
 */
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_AMYSYNTH_RTOS_STATS
void rtos_stats_report(void);
#else
static inline void rtos_stats_report(void) {}
#endif

#ifdef __cplusplus
}
#endif

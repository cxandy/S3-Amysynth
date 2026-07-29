#pragma once
/* AMY profile-dump plumbing, gated on AMY's own CONFIG_AMY_PROFILE_COARSE /
 * CONFIG_AMY_PROFILE_FULL. Both calls compile to nothing when profiling is off.
 */
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_AMY_PROFILE_COARSE) || defined(CONFIG_AMY_PROFILE_FULL)

/* Measure esp_timer_get_time() cost once at startup so the profile dumps can be
 * read net of instrumentation overhead. Busy-loops for ~20k reads; call from
 * app_main before the audio path matters, not from a running system. */
void amy_profile_overhead_selftest(void);

/* Dump and reset the AMY profile window. In COARSE mode the "% render" column
 * is the parallelizable fraction. */
void amy_profile_report(void);

#else

static inline void amy_profile_overhead_selftest(void) {}
static inline void amy_profile_report(void) {}

#endif

#ifdef __cplusplus
}
#endif

#pragma once
/* Output-level watchdog: flags output that is stuck at clipping level or
 * continuously near full scale (filter self-oscillation, runaway feedback,
 * a stuck voice).
 *
 * Rather than metering inside the render path, it peeks at the most recent
 * audio already committed to the USB ring buffer - the exact PCM the host
 * receives - via usb_audio_peek_levels(). Poll it from one low-priority task
 * on the non-render core (here: synth_ui_task, Core 0, 50 ms cadence); the
 * render task and Core 1 do no work for this feature.
 *
 * Single-client: output_wd_poll() and output_wd_state() must be called from
 * the same task. When CONFIG_OUTPUT_WATCHDOG is disabled the inline stubs
 * below compile every call site away, so callers need no #if guards. */

#include "sdkconfig.h"

typedef enum {
    OUTPUT_WD_OK = 0,
    OUTPUT_WD_LOUD,      /* mean level near full scale for seconds */
    OUTPUT_WD_CLIPPING,  /* peaks pinned at the soft-clip knee for ~0.5 s */
} output_wd_state_t;

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_OUTPUT_WATCHDOG
void output_wd_poll(void);
output_wd_state_t output_wd_state(void);
#else
static inline void output_wd_poll(void) {}
static inline output_wd_state_t output_wd_state(void) { return OUTPUT_WD_OK; }
#endif

#ifdef __cplusplus
}
#endif

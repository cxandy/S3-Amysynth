#pragma once
// Onboard WS2812 status LED: shows Core-1 (render) load as a color band
// (green < 50%, yellow >= 50%, orange >= 75%, red >= 90%, hysteresis at the
// edges) and answers failed heap allocations with a magenta blink burst.
// Load comes from core_load_sample() (diagnostics), i.e. the IDLE1 run-time
// counter - nothing in the render path is instrumented.
// All calls compile to nothing when CONFIG_AMYSYNTH_STATUS_LED is off.
#include "sdkconfig.h"

#if CONFIG_AMYSYNTH_STATUS_LED

// Create the LED driver and start the updater task.
// Obligations: call once, from a Core-0 task, after the scheduler is running
// and init-time allocation is done. Core 0 matters: the RMT backend allocates
// its interrupt on the calling core, and it must stay off the DSP core.
// Guarantees: on any failure (driver init, task create) logs a warning and
// leaves the module inert - never crashes, later API calls stay safe no-ops.
void status_led_start(void);

// Flag an allocation failure for the blink signal. Any task context, not
// ISR-safe. Cheap (one atomic store); safe before status_led_start().
void status_led_notify_alloc_failure(void);

#else

static inline void status_led_start(void) {}
static inline void status_led_notify_alloc_failure(void) {}

#endif

#pragma once
/* Periodic diagnostic reporting.
 *
 * The app's idle loop calls diag_report_all() on an interval and each enabled
 * diagnostic prints its window. Reporters are registered rather than called
 * directly so that adding or gating one never touches the loop: the pile of
 * per-diagnostic #if blocks that used to sit in app_main is now a single call.
 *
 * Reporters run in registration order, sequentially, on the calling task. They
 * must be Core-0 / low-priority safe: reporting walks task tables, formats
 * strings and may briefly allocate, so nothing here may be called from the
 * render path, a tick hook or an ISR.
 *
 * diag_init() registers the reporters that live inside this component (render
 * timing, RTOS stats, AMY profile dumps), each self-gating on its own Kconfig
 * symbol. Other components register their own from their init path.
 */
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Prints one window's worth of output. Must not block. */
typedef void (*diag_reporter_fn)(void);

/* Register the built-in reporters. Call once, before the reporting loop
 * starts; registrations made afterwards still take effect from the next
 * interval. */
void diag_init(void);

/* Add a reporter. `name` must have static lifetime and is used only for the
 * registration log. Returns false if the fixed table is full (no allocation
 * happens here), in which case the reporter is dropped with a warning. */
bool diag_register_reporter(const char *name, diag_reporter_fn fn);

/* Run every registered reporter once, in registration order. */
void diag_report_all(void);

/* Suggested interval between diag_report_all() calls: the period of whichever
 * diagnostic has the tightest requirement, or a slow default when none is
 * enabled. */
uint32_t diag_report_interval_ms(void);

#ifdef __cplusplus
}
#endif

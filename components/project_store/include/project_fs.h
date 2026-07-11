#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Project filesystem layer ────────────────────────────────────────────
 * Mounts the 'storage' LittleFS partition at PROJECT_FS_BASE. Synth-agnostic:
 * no knowledge of what the files contain. All calls are UI-task only (no
 * internal locking); none are safe from ISRs or the render path. */

#define PROJECT_FS_BASE "/proj"

/* Mount (formatting on first use). Returns true on success. Failure is
 * non-fatal: the synth runs normally and project_fs_ok() reports the state
 * so the UI can show a storage-error indicator instead of the slot list. */
bool project_fs_init(void);

bool project_fs_ok(void);

/* Filesystem totals in bytes. Returns false while unmounted. */
bool project_fs_stats(size_t *total_bytes, size_t *used_bytes);

#ifdef __cplusplus
}
#endif
